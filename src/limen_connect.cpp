#include <cstdio>
#include <stdlib.h>
#include <unistd.h>
#include <iostream>
#include <infiniband/verbs.h>
#include "limen/limen_common.h"
#include "limen/limen_connect.h"

void parse_argv(int argc, char* argv[],connect_parsed_args* args)
{
    int opt;
    while ((opt = getopt(argc, argv, "d:g:p:t:s:")) != -1)
    {
        switch (opt)
        {
            case 0:
            {
                break;
            }
            case 'd':
            {
                args->device_name = optarg;
                break;
            }
            case 'g':
            {
                int rc = parse_u64_strict(optarg,&args->gid_index);
                if (rc != 0)
                {
                    exit(1);
                }
                break;
            }
            case 'p': 
            {
                int rc = parse_u64_strict(optarg,&args->port);
                if (rc != 0)
                {
                    exit(1);
                }
                break;
            }
            case 's':
            {
                int rc = parse_u64_strict(optarg,&args->buffer_size);
                if (rc != 0)
                {
                    exit(1);
                }
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
    if (optind < argc)
    {
        // address was given
        char* addr = argv[argc-1];
        args->addr = addr;

    }
}

void print_help(bool to_error)
{
    if (to_error)
    {
        fprintf(stderr, "./build/limen_connect -d <device> -g <gid_index> [-p <port>] [-t <tcp_port>] [-s <bytes>] [<peer>]\n");
    }
    else
    {
        printf("./build/limen_connect -d <device> -g <gid_index> [-p <port>] [-t <tcp_port>] [-s <bytes>] [<peer>]\n");

    }
}

int main(int argc, char* argv[])
{
    // Misc. variables
    connect_parsed_args args;
    int rc;

    //  Device-based variables
    ibv_device** devices_list;
    ibv_context* device_context;
    ibv_device_attr device_attr;

    //  Port-based variables
    ibv_port_attr port_attr;

    //  QP-based variables
    ibv_cq* completion_queue;
    ibv_qp queue_pair;

    // parse args
    parse_argv(argc,argv,&args);

    // make sure device_name & gid_index are supplied
    if (args.device_name == nullptr)
    {
        fprintf(stderr,"-d required\n");
        print_help(true); 
        exit(1);
    }
    if (args.gid_index == UINT64_MAX )
    {
        fprintf(stderr,"-g required\n");
        print_help(true); 
        exit(1);
    }

    //  open devices list
    devices_list = ibv_get_device_list(NULL);
    if (devices_list == NULL)
    {
        //  failed to open device list
        perror("ibv_get_device_list");
        exit(1);

    }

    //  figure out index of associated device (-d)
    int device_idx = find_device_by_name(devices_list,args.device_name);
    if (device_idx == -1)
    {
        //  did not find device, exit early
        fprintf(stderr,"did not find device %s\n",args.device_name);
        exit(1);
    }

    //  get device context
    device_context = ibv_open_device(devices_list[device_idx]);
    if (device_context == NULL)
    {
        //  failed to open device context
        perror("ibv_open_device");
        exit(1);
    }

    //  get device attributes
    rc = ibv_query_device(device_context,&device_attr);
    if (rc != 0)
    {
        //  failed to get device attributes
        perror("ibv_query_device");
        exit(1);
    }

    //  get port attributes
    rc = ibv_query_port(device_context,args.port,&port_attr);
    if (rc != 0)
    {
        //  failed to get port attributes
        perror("ibv_query_port");
        exit(1);
    }

    //  verify the GID index against gid_tbl_len
    if (args.gid_index > port_attr.gid_tbl_len)
    {
        fprintf(stderr, "invalid gid_index: gid_index " PRIu64 " > gid_tbl_len " PRIu64 "\n",args.gid_index,port_attr.gid_tbl_len);
        exit(1);
    }


    //  create the queues

    //  create completion queue
    int depth = COMPLETE_QUEUE_DEPTH;
    completion_queue = ibv_create_cq(device_context,COMPLETE_QUEUE_DEPTH,NULL,NULL,0);
    printf("cq: cqe=%i (requested %i)\n",completion_queue->cqe, COMPLETE_QUEUE_DEPTH);

    //  fill qp_init_attr to make the QP

    //  create reliable-connected queue pair

    //  populate local identity struct

    //  perform side-channel exchange (send struct as text not struct data)

    //  perform RESET->INIT transition

    //  perform INIT->RTR transition

    //  perform RTR->RTS transition

    //  Verify QPs are both RTS




    return 0;
}