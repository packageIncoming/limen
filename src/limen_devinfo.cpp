#include <cstdio>
#include <iostream>
#include <getopt.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
#include <infiniband/verbs.h>
#include <endian.h>
#include "limen/limen_devinfo.h"


void parse_argv(int argc, char* argv[],parsed_args* args_container)
{
    int opt;
    int check_access_flag=0;

    static struct option long_options[] = {
        {"check-access",no_argument,&check_access_flag,1},
        {0,0,0,0}
    };

    while 
    ((opt = getopt_long(argc, argv, "d:p:s:h", long_options, NULL)) != -1)
    {
        switch (opt)
        {
            case 0:
            {
                break;
            }
            case 'd':
            {
                args_container->device_name = optarg;
                break;
            }
            case 'p': 
            {
                char *end = NULL;
                errno = 0;
                unsigned long long v = strtoull(optarg, &end, 10);
                if (errno || end == optarg || *end != '\0' || v == 0) { exit(1);}
                args_container->port = v;
                break;
            }
            case 's':
            {
                char *end = NULL;
                errno = 0;
                unsigned long long v = strtoull(optarg, &end, 10);
                if (errno || end == optarg || *end != '\0' || v == 0) { exit(1); }
                args_container->buffer_size = v;
                break;
            }
            case 'h':
            {
                printf("./build/limen_devinfo [-d <device>] [-p <port>] [-s <bytes>] [--check-access] [-h]\n");
                exit(0);
                return;
            }
            case '?':
            {
                exit(1);
                break;
            }
            default:
            {
                break;
            }
        }
    }
}

int enumerate_devices(ibv_device** devices_list,int print_to_err)
{

    int device_idx=0;
    ibv_device* device;
    while 
    ((device = devices_list[device_idx])!=NULL)
    {
        // need to get device name and guid for each device
        const char* name = ibv_get_device_name(device);
        uint64_t guid = be64toh(ibv_get_device_guid(device));
        if
        (print_to_err)
        {
            fprintf(stderr,"device: %s guid: 0x%016" PRIx64 "\n",name,guid);
        }
        else
        {
            printf("device: %s guid: 0x%016" PRIx64 "\n",name,guid);
        }
        device_idx++;
    }

    // check if empty (device_idx==0 meaning no devices)
    if
    (device_idx ==0)
    {
        std::cerr << "No RDMA devices registered with kernel\n";
        return 2;
    }
    if
    (print_to_err)
    {
        fprintf(stderr,"devices: %i\n",device_idx);
    }
    else
    {
        printf("devices: %i\n",device_idx);
    }
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
        if 
        (strcmp(name,device_name) == 0)
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
    printf("\tstate: ")
}

int main(int argc, char* argv[])
{
    //  VARIABLES
    ibv_device** device_list;
    ibv_device* device;
    ibv_context* device_context;
    ibv_device_attr device_attr;
    ibv_port_attr port_attr;

    //  handle arguments
    parsed_args args_container;
    args_container.port = 1; // default port num
    parse_argv(argc,argv,&args_container);

    //  open device list
    device_list = ibv_get_device_list(NULL);
    if 
    (device_list == NULL)
    {
        perror("ibv_get_device_list");
        //  TODO graceful exit
        exit(3);
    }

    if
    (argc ==1)
    {
        //  enumerate mode
        int rc = enumerate_devices(device_list,0);
        ibv_free_device_list(device_list);
        //  TODO graceful exit
        exit(rc);
    }

    if 
    (args_container.device_name != nullptr)
    {
        //  (-d) open a named device context
        int device_idx = find_device_by_name(device_list,args_container.device_name);
        if
        (device_idx == -1)
        {
            // not found, enumerate & exit 2
            enumerate_devices(device_list,1);
            ibv_free_device_list(device_list);
            //  TODO graceful exit
            exit(2);
        }
        //  query device & print fields
        device = device_list[device_idx];
        //  open device to get ibv_context* ptr
        device_context = ibv_open_device(device);
        printf("device: %s\n",args_container.device_name);
        if
        (ibv_query_device(device_context,&device_attr) != 0)
        {
            perror("ibv_query_device");
            //  TODO graceful exit
            exit(3);
        }
        print_device_info(&device_attr);
    }

    //  query port & print fields
    if
    (args_container.port > device_attr.phys_port_cnt)
    {
        fprintf(stderr,"port number cannot be greater than phys_port_cnt\n");
        //  TODO graceful exit
        exit(1);
    }

    if
    (ibv_query_port(device_context,args_container.port,&port_attr) != 0)
    {
        //  TODO GRACEFUL EXIT
        perror("ibv_query_port");
        exit(3);
    }

    printf("port: %i\n",args_container.port);
    print_port_info(&port_attr);
    

    //  allocate protection domain on context 

    if 
    (args_container.buffer_size != 0)
    {

    }
    // (-s) allocate buffer 

    //  register buffer against pd & print info

    //  gracefully handle registration failure

    //  (--check-access) access flag constraint

    //  teardown & handle errors



    ibv_free_device_list(device_list);
    return 0;
}