#include <cstdio>
#include <iostream>
#include <getopt.h>
#include <stdlib.h>
#include <errno.h>
#include <infiniband/verbs.h>
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

int enumerate_devices()
{
    //  open device list
    ibv_device ** device_list = ibv_get_device_list(NULL);
    //  enumerate devices (if no args given)
    //  check if the call failed (exit code 3)
    if 
    (device_list == NULL)
    {
        perror("ibv_get_device_list");
        return 3;
    }
    int device_idx=0;
    ibv_device* device;
    while 
    ((device = device_list[device_idx])!=NULL)
    {
        // need to get device name and guid for each device
        const char* name = ibv_get_device_name(device);
        uint64_t guid = ibv_get_device_guid(device);
        printf("device: %s guid: 0x%lx\n",name,guid);
        device_idx++;
    }

    // check if empty (device_idx==0 meaning no devices)
    if
    (device_idx ==0)
    {
        std::cerr << "No RDMA devices registered with kernel\n";
        return 2;
    }
    
    printf("devices: %i\n",device_idx);
    ibv_free_device_list(device_list);
    return 0;
}

int main(int argc, char* argv[])
{
    //  handle arguments
    parsed_args args_container;
    args_container.port = 1; // default port num
    parse_argv(argc,argv,&args_container);


    if
    (argc ==1)
    {
        enumerate_devices();
    }

    if 
    (args_container.device_name != nullptr)
    {
        //  (-d) open a named device context


         //  query device & print fields

    }

    //  query port & print fields

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




    printf("hello world\n");
    return 0;
}