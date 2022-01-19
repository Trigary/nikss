#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <net/if.h>
#include <unistd.h>
#include <linux/if_link.h>
#include "bpf/bpf.h"
#include "bpf/libbpf.h"
#include <string.h>

#include "../include/psabpf_pipeline.h"
#include "../include/bpf_defs.h"
#include "common.h"
#include "btf.h"

#define bpf_object__for_each_program(pos, obj)		\
	for ((pos) = bpf_program__next(NULL, (obj));	\
	     (pos) != NULL;				\
	     (pos) = bpf_program__next((pos), (obj)))

static char *program_pin_name(struct bpf_program *prog)
{
    char *name, *p;

    name = p = strdup(bpf_program__section_name(prog));
    while ((p = strchr(p, '/')))
        *p = '_';

    return name;
}

static int do_initialize_maps(int prog_fd)
{
    __u32 duration, retval, size;
    char in[128], out[128];
    return bpf_prog_test_run(prog_fd, 1, &in[0], 128,
                             out, &size, &retval, &duration);
}

static int open_obj_by_name(psabpf_pipeline_id_t pipeline_id, const char *prog)
{
    char pinned_file[256];
    snprintf(pinned_file, sizeof(pinned_file), "%s/%s%d/%s",
             BPF_FS, PIPELINE_PREFIX, pipeline_id, prog);

    return bpf_obj_get(pinned_file);  // error in errno
}

static int xdp_attach_prog_to_port(int *fd, psabpf_pipeline_id_t pipeline_id, int ifindex, const char *prog)
{
    __u32 flags;
    int ret;

    *fd = open_obj_by_name(pipeline_id, prog);
    if (*fd < 0)
        return errno;  // from sys_call

    /* TODO: add support for hardware offload mode (XDP_FLAGS_HW_MODE) */

    flags = XDP_FLAGS_DRV_MODE;
    ret = bpf_set_link_xdp_fd(ifindex, *fd, flags);
    if (ret != -EOPNOTSUPP) {
        if (ret < 0) {
            close_object_fd(fd);
            return -ret;
        }
        return NO_ERROR;
    }

    fprintf(stderr, "XDP native mode not supported by driver, retrying with generic SKB mode\n");
    flags = XDP_FLAGS_SKB_MODE;
    ret = bpf_set_link_xdp_fd(ifindex, *fd, flags);
    if (ret < 0) {
        close_object_fd(fd);
        return -ret;
    }

    return NO_ERROR;
}

static int update_prog_devmap(psabpf_bpf_map_descriptor_t *devmap, int ifindex, const char *intf, int egress_prog_fd)
{
    struct bpf_devmap_val devmap_val;

    devmap_val.ifindex = ifindex;
    devmap_val.bpf_prog.fd = -1;

    /* install egress program only if it's found */
    if (egress_prog_fd >= 0) {
        devmap_val.bpf_prog.fd = egress_prog_fd;
    }
    if (ifindex > (int) devmap->max_entries) {
        fprintf(stderr,
                "Warning: the index(=%d) of the interface %s is higher than the DEVMAP size (=%d)\n"
                "Applying modulo ... \n", ifindex, intf, devmap->max_entries);
    }
    int index = ifindex % ((int) devmap->max_entries);
    int ret = bpf_map_update_elem(devmap->fd, &index, &devmap_val, 0);
    if (ret)
        return errno;

    return NO_ERROR;
}

static int xdp_port_add(psabpf_pipeline_id_t pipeline_id, const char *intf)
{
    int ret;
    int ig_prog_fd, eg_prog_fd;

    char base_map_path[256];
    build_ebpf_map_path(base_map_path, sizeof(base_map_path), pipeline_id);

    int ifindex = (int) if_nametoindex(intf);
    if (!ifindex) {
        return EINVAL;
    }

    /* TODO: Should we attach ingress pipeline at the end of whole procedure?
     *  For short time packets will be served only in ingress but not in egress pipeline. */
    ret = xdp_attach_prog_to_port(&ig_prog_fd, pipeline_id, ifindex, XDP_INGRESS_PROG);
    if (ret != NO_ERROR)
        return ret;
    close_object_fd(&ig_prog_fd);

    /* may not exist, ignore errors */
    eg_prog_fd = open_obj_by_name(pipeline_id, XDP_EGRESS_PROG);

    psabpf_bpf_map_descriptor_t devmap;
    ret = open_bpf_map(NULL, XDP_DEVMAP, base_map_path, &devmap);
    if (ret != NO_ERROR) {
        close_object_fd(&eg_prog_fd);
        return ret;
    }

    ret = update_prog_devmap(&devmap, ifindex, intf, eg_prog_fd);
    close_object_fd(&eg_prog_fd);
    close_object_fd(&devmap.fd);
    if (ret != NO_ERROR) {
        return ret;
    }

    eg_prog_fd = open_obj_by_name(pipeline_id, XDP_EGRESS_PROG_OPTIMIZED);
    if (eg_prog_fd >= 0) {
        psabpf_bpf_map_descriptor_t jmpmap;
        ret = open_bpf_map(NULL, XDP_JUMP_TBL, base_map_path, &jmpmap);
        if (ret != NO_ERROR) {
            close_object_fd(&eg_prog_fd);
            return ENOENT;
        }

        int index = 0;
        ret = bpf_map_update_elem(jmpmap.fd, &index, &eg_prog_fd, 0);
        close_object_fd(&eg_prog_fd);
        close_object_fd(&jmpmap.fd);
        if (ret) {
            return errno;
        }
    }

    /* FIXME: using bash command only for the PoC purpose */
    char cmd[256];
    sprintf(cmd, "tc qdisc add dev %s clsact", intf);
    system(cmd);
    memset(cmd, 0, sizeof(cmd));
    sprintf(cmd, "tc filter add dev %s ingress bpf da fd %s/%s%d/%s",
            intf, BPF_FS, PIPELINE_PREFIX, pipeline_id, TC_INGRESS_PROG);
    system(cmd);
    memset(cmd, 0, sizeof(cmd));
    sprintf(cmd, "tc filter add dev %s egress bpf da fd %s/%s%d/%s",
            intf, BPF_FS, PIPELINE_PREFIX, pipeline_id, TC_EGRESS_PROG);
    system(cmd);

    return 0;
}

static int tc_port_add(psabpf_pipeline_id_t pipeline_id, const char *intf)
{
    int xdp_helper_fd;

    int ifindex = (int) if_nametoindex(intf);
    if (!ifindex) {
        return EINVAL;
    }

    int ret = xdp_attach_prog_to_port(&xdp_helper_fd, pipeline_id, ifindex, XDP_HELPER_PROG);
    if (ret != NO_ERROR)
        return ret;
    close_object_fd(&xdp_helper_fd);

    /* FIXME: using bash command only for the PoC purpose */
    char cmd[256];
    sprintf(cmd, "tc qdisc add dev %s clsact", intf);
    system(cmd);
    memset(cmd, 0, sizeof(cmd));
    sprintf(cmd, "tc filter add dev %s ingress bpf da fd %s/%s%d/%s",
            intf, BPF_FS, PIPELINE_PREFIX, pipeline_id, TC_INGRESS_PROG);
    system(cmd);
    memset(cmd, 0, sizeof(cmd));
    sprintf(cmd, "tc filter add dev %s egress bpf da fd %s/%s%d/%s",
            intf, BPF_FS, PIPELINE_PREFIX, pipeline_id, TC_EGRESS_PROG);
    system(cmd);
    return 0;
}

void psabpf_pipeline_init(psabpf_pipeline_t *pipeline)
{
    memset(pipeline, 0, sizeof(psabpf_pipeline_t));
}

void psabpf_pipeline_free(psabpf_pipeline_t *pipeline)
{
    if ( pipeline == NULL )
        return;

    memset(pipeline, 0, sizeof(psabpf_pipeline_t));
}

void psabpf_pipeline_setid(psabpf_pipeline_t *pipeline, int pipeline_id)
{
    pipeline->id = pipeline_id;
}

void psabpf_pipeline_setobj(psabpf_pipeline_t *pipeline, const char *obj)
{
    pipeline->obj = obj;
}

bool psabpf_pipeline_exists(psabpf_pipeline_t *pipeline)
{
    char mounted_path[256];
    snprintf(mounted_path, sizeof(mounted_path), "%s/%s%d", BPF_FS,
             PIPELINE_PREFIX, pipeline->id);

    return access(mounted_path, F_OK) == 0;
}

int psabpf_pipeline_load(psabpf_pipeline_t *pipeline)
{
    struct bpf_object *obj;
    int ret, prog_fd;
    char pinned_file[256];
    struct bpf_program *pos;

    const char *file = pipeline->obj;

    ret = bpf_prog_load(file, BPF_PROG_TYPE_UNSPEC, &obj, &prog_fd);
    if (ret < 0 || obj == NULL) {
        fprintf(stderr, "cannot load the BPF program, code = %d\n", ret);
        return -1;
    }

    bpf_object__for_each_program(pos, obj) {
        const char *sec_name = bpf_program__section_name(pos);
        int prog_fd = bpf_program__fd(pos);
        if (!strcmp(sec_name, TC_INIT_PROG) || !strcmp(sec_name, XDP_INIT_PROG)) {
            ret = do_initialize_maps(prog_fd);
            if (ret) {
                goto err_close_obj;
            }
            // do not pin map initializer
            continue;
        }

        snprintf(pinned_file, sizeof(pinned_file), "%s/%s%d/%s", BPF_FS,
                 PIPELINE_PREFIX, pipeline->id, program_pin_name(pos));

        ret = bpf_program__pin(pos, pinned_file);
        if (ret < 0) {
            goto err_close_obj;
        }
    }

    struct bpf_map *map;
    bpf_object__for_each_map(map, obj) {
        if (bpf_map__is_pinned(map)) {
            if (bpf_map__unpin(map, NULL)) {
                goto err_close_obj;
            }
        }

        memset(pinned_file, 0, sizeof(pinned_file));
        snprintf(pinned_file, sizeof(pinned_file), "%s/%s%d/%s/%s", BPF_FS,
                 PIPELINE_PREFIX, pipeline->id, "maps", bpf_map__name(map));
        if (bpf_map__set_pin_path(map, pinned_file)) {
            goto err_close_obj;
        }

        if (bpf_map__pin(map, pinned_file)) {
            goto err_close_obj;
        }
    }

err_close_obj:
    bpf_object__close(obj);

    return ret;
}

int psabpf_pipeline_unload(psabpf_pipeline_t *pipeline)
{
    // FIXME: temporary solution [PoC-only].
    char cmd[256];
    sprintf(cmd, "rm -rf %s/%s%d",
            BPF_FS, PIPELINE_PREFIX, pipeline->id);
    return system(cmd);
}

int psabpf_pipeline_add_port(psabpf_pipeline_t *pipeline, const char *intf)
{
    char pinned_file[256];
    bool isXDP = false;

    /* Determine firstly if we have TC-based or XDP-based pipeline.
     * We can do this by just checking if XDP helper exists under a mount path. */
    snprintf(pinned_file, sizeof(pinned_file), "%s/%s%d/%s", BPF_FS,
             PIPELINE_PREFIX, pipeline->id, XDP_HELPER_PROG);
    isXDP = access(pinned_file, F_OK) != 0;

    return isXDP ? xdp_port_add(pipeline->id, intf) : tc_port_add(pipeline->id, intf);
}

int psabpf_pipeline_del_port(psabpf_pipeline_t *pipeline, const char *intf)
{
    char cmd[256];
    __u32 flags = 0;
    int ifindex;

    ifindex = if_nametoindex(intf);
    if (!ifindex)
        return EINVAL;

    int ret = bpf_set_link_xdp_fd(ifindex, -1, flags);
    if (ret) {
        return ret;
    }

    // FIXME: temporary solution [PoC-only].
    sprintf(cmd, "tc qdisc del dev %s clsact", intf);
    ret = system(cmd);
    if (ret) {
        return ret;
    }

    return 0;
}