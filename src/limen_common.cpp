#include "limen/limen_common.h"
#include <stdlib.h>
#include <errno.h>
#include <cstdlib>  
#include <cinttypes>
#include <cstdio>


bool parse_u64_strict(const char* str, uint64_t* val_addr)
{
    char *end = NULL;
    errno = 0;
    uint64_t v = strtoull(str, &end, 10);
    if (errno || end == str || *end != '\0' || v == 0) { return 1;}
    *val_addr= v;
    return 0;
}

int find_device_by_name(ibv_device** devices_list, const char* device_name)
{
    int device_idx=0;
    ibv_device* device;
    while 
    ((device = devices_list[device_idx])!=NULL)
    {
        // need to get device name and guid for each device
        const char* name = ibv_get_device_name(device);
        if (strcmp(name,device_name) == 0)
        {
            return device_idx;
        }
        device_idx++;
    }
    return -1;
}

void print_device_info(ibv_device_attr* attr)
{


    // print out all the data here:
    const char* fmt64 = "\t%s: %llu\n";
    const char* fmtint = "\t%s: %i\n";
    uint64_t guid = be64toh(attr->node_guid);

    printf("\t%s: 0x%016" PRIx64 "\n","guid",guid);
    printf("\tfw_ver: %s\n",attr->fw_ver);
    printf(fmtint,"phys_port_cnt",attr->phys_port_cnt);
    printf(fmtint,"max_qp",attr->max_qp);
    printf(fmtint,"max_qp_wr",attr->max_qp_wr);
    printf(fmtint,"max_cq",attr->max_cq);
    printf(fmtint,"max_cqe",attr->max_cqe);
    printf(fmtint,"max_mr",attr->max_mr);
    printf(fmt64,"max_mr_size",attr->max_mr_size);
    printf(fmtint,"max_sge",attr->max_sge);
    printf(fmtint,"max_qp_rd_atom",attr->max_qp_rd_atom);
}

void print_port_info(ibv_port_attr* attr)
{
    const char* link_layer;
    int active_mtu_bytes;
    const char* state;

    //  figure out the state string
    switch(attr->state)
    {
        case IBV_PORT_NOP:
            state = "PORT_NOP";
            break;
        case IBV_PORT_DOWN:
            state = "PORT_DOWN";
            break;
        case IBV_PORT_INIT:
            state = "PORT_INIT";
            break;
        case IBV_PORT_ARMED:
            state = "PORT_ARMED";
            break;
        case IBV_PORT_ACTIVE:
            state = "PORT_ACTIVE";
            break;
        case IBV_PORT_ACTIVE_DEFER:
            state = "PORT_ACTIVE_DEFER";
            break;
    }

    //  figure out link layer
    switch(attr->link_layer)
    {
        case IBV_LINK_LAYER_INFINIBAND:
            link_layer = "InfiniBand";
            break;
        case IBV_LINK_LAYER_ETHERNET:
            link_layer = "Ethernet";
            break;
        case IBV_LINK_LAYER_UNSPECIFIED:
            link_layer = "Unspecified";
            break;
    }

    switch (attr->active_mtu)
    {
        case IBV_MTU_256:
            active_mtu_bytes = 256;
            break;
        case IBV_MTU_512:
            active_mtu_bytes = 512;
            break;
        case IBV_MTU_1024:
            active_mtu_bytes = 1024;
            break;
        case IBV_MTU_2048:
            active_mtu_bytes = 2048;
            break;
        case IBV_MTU_4096:
            active_mtu_bytes = 4096;
            break;
    }



    printf("\tstate: %s\n",state);
    printf("\tlink_layer: %s\n",link_layer);
    printf("\tactive_mtu: %i\n",active_mtu_bytes);
    printf("\tmax_msg_sz: %" PRIu32 "\n", attr->max_msg_sz);
    printf("\tgid_tbl_len: %i\n",attr->gid_tbl_len);
}