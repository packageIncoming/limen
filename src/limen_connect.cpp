#include <cstdio>
#include <stdlib.h>
#include <unistd.h>
#include <iostream>
#include "limen/limen_common.h"
#include "limen/limen_connect.h"

void parse_argv(int argc, char* argv[],connect_parsed_args* args_container)
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
                args_container->device_name = optarg;
                break;
            }
            case 'g':
            {
                int rc = parse_u64_strict(optarg,&args_container->gid_index);
                if (rc != 0)
                {
                    exit(1);
                }
                break;
            }
            case 'p': 
            {
                int rc = parse_u64_strict(optarg,&args_container->port);
                if (rc != 0)
                {
                    exit(1);
                }
                break;
            }
            case 's':
            {
                int rc = parse_u64_strict(optarg,&args_container->buffer_size);
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
        args_container->addr = addr;

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
    // parse args
    connect_parsed_args args_container;
    parse_argv(argc,argv,&args_container);

    // make sure device_name & gid_index are supplied
    if (args_container.device_name == nullptr)
    {
        fprintf(stderr,"-d required\n");
        print_help(true); 
        exit(1);
    }
    if (args_container.gid_index == UINT64_MAX )
    {
        fprintf(stderr,"-g required\n");
        print_help(true); 
        exit(1);
    }

    // verify the GID index against gid_tbl_len
    

    //  create the queues

    //  create completion queue

    //  create reliable-connected queue pair

    //  populate local identity struct

    //  perform side-channel exchange (send struct as text not struct data)

    //  perform RESET->INIT transition

    //  perform INIT->RTR transition

    //  perform RTR->RTS transition

    //  Verify QPs are both RTS




    return 0;
}