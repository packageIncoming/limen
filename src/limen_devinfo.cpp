#include <cstdio>
#include <exception>
#include <iostream>
#include <getopt.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
#include <infiniband/verbs.h>
#include <endian.h>
#include <cstring>
#include <sys/resource.h>
#include "limen/limen_devinfo.h"
#include "limen/limen_common.h"
#include "limen/verbs.hpp"


void parse_argv(int argc, char* argv[],parsed_args* args_container)
{
    int opt;

    static struct option long_options[] = {
        {"check-access",no_argument,&args_container->check_access_flag,1},
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
    if (device_idx ==0)
    {
        std::cerr << "No RDMA devices registered with kernel\n";
        return 2;
    }
    if (print_to_err)
    {
        fprintf(stderr,"devices: %i\n",device_idx);
    }
    else
    {
        printf("devices: %i\n",device_idx);
    }
    return 0;
}



void print_mr_diag_block(int errn,unsigned long long int req_bytes)
{

    const char* hint;
    struct rlimit rl;

    switch (errn)
    {
        case ENOMEM:
            hint = "raise the locked-memory limit with 'ulimit -l' or register a smaller region";
            break;
        case EINVAL:
            hint = "check the flags you passed to ibv_reg_mr";
            break;
        case EFAULT:
            hint = "the buffer + length might be outside the valid virtual address space, or the kernel might've failed to pin the physical pages of memory";
            break;
        default:
            hint = "unknown error, cannot provide hint";
    }

    fprintf(stderr,"mr: registration failed: %s (%s)\n",error_enumstr(errn),std::strerror(errn));
    fprintf(stderr,"\trequested: %llu bytes\n",req_bytes);
    if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0) {
        if (rl.rlim_cur == RLIM_INFINITY)
        {
            fprintf(stderr, "  RLIMIT_MEMLOCK soft: unlimited\n");
        }
        else
        {
            fprintf(stderr, "  RLIMIT_MEMLOCK soft: %llu bytes\n",
                    (unsigned long long)rl.rlim_cur);
        }

    }    
    fprintf(stderr,"hint: %s\n",hint);

}

const char* error_enumstr(int errn)
{
    switch (errn)
    {
        case ENOMEM:
            return "ENOMEM";
        case EFAULT:
            return "EFAULT";
        case EINVAL:
            return "EINVAL";
        default:
            return "UNKNOWN_ERROR";

    }
}


int main(int argc, char* argv[])
{
    try 
    {

        //  VARIABLES
        // ibv_device** device_list= nullptr;
        // ibv_device* device= nullptr;
        // ibv_context* device_context= nullptr;
        limen::DeviceList dl;
        limen::Context ctx;
        limen::ProtectionDomain pd;
        limen::MemoryRegion mr;
        ibv_device_attr device_attr;
        ibv_port_attr port_attr;
        // ibv_pd* pd= nullptr;
        // ibv_mr* mr= nullptr;
        // void* mr_buffer= nullptr;

        //  handle arguments
        parsed_args args_container;
        args_container.port = 1; // default port num
        parse_argv(argc,argv,&args_container);

        //  open device list
        // device_list = ibv_get_device_list(NULL);
        // if (device_list == NULL)
        // {


        // }

        if (argc ==1)
        {
            //  enumerate mode
            int rc = enumerate_devices(dl.get(),0);
            return rc;
        }

        if (args_container.device_name != nullptr)
        {
            //  (-d) open a named device context
            int device_idx = find_device_by_name(dl.get(),args_container.device_name);
            if (device_idx == -1)
            {
                // not found, enumerate & exit 2
                enumerate_devices(dl.get(),1);
                return 2;

            }
            //  query device & print fields
            // device = device_list[device_idx];
            //  open device to get ibv_context* ptr
            ctx = limen::Context(args_container.device_name);
            printf("device: %s\n",args_container.device_name);
            if (ibv_query_device(ctx.get(),&device_attr) != 0)
            {
                perror("ibv_query_device");
                return 3;
            }
            print_device_info(&device_attr);
        }

        //  query port & print fields
        if (args_container.port > device_attr.phys_port_cnt)
        {
            fprintf(stderr,"port number cannot be greater than phys_port_cnt\n");
            return 1;

        }

        if (ibv_query_port(ctx.get(),args_container.port,&port_attr) != 0)
        {
            //  TODO GRACEFUL EXIT
            perror("ibv_query_port");
            return 3;

        }

        printf("port %lli:\n",args_container.port);
        print_port_info(&port_attr);
        

        //  allocate protection domain on context 
        pd = limen::ProtectionDomain(ctx);
        
        // pd = ibv_alloc_pd(ctx.get());
        // if (pd == nullptr)
        // {
        //     std::cerr << "failed to allocate protection domain" << std::endl;
        //     return 3;

        // }
        printf("pd: allocated\n");


        if (args_container.buffer_size != 0)
        {
            // (-s) allocate buffer 

            //  register buffer against pd & print info
            int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
            try {
                mr = limen::MemoryRegion(pd, args_container.buffer_size, access);
            } catch (const limen::VerbsError& e) {
                print_mr_diag_block(e.error(), args_container.buffer_size);
                return 3;
            }            // mr_buffer =calloc(1,args_container.buffer_size);
            // mr = ibv_reg_mr(pd,mr_buffer,args_container.buffer_size,access);
            // if (mr == nullptr)
            // {
            //     print_mr_diag_block( errno,args_container.buffer_size);
            //     return 3;

            // }
            printf(
                "mr: addr=%p length=%zu lkey=0x%08x rkey=0x%08x ",
                mr.get()->addr,mr.get()->length,mr.get()->lkey,mr.get()->rkey);
            printf("access=LOCAL_WRITE|REMOTE_READ|REMOTE_WRITE\n");
            
        }

        //  (--check-access) access flag constraint
        if (args_container.check_access_flag)
        {
            char probe[4096] = {0};
            errno = 0;
            struct ibv_mr *check_access_mr = ibv_reg_mr(pd.get(), probe, sizeof probe, IBV_ACCESS_REMOTE_WRITE);
            if (check_access_mr==nullptr)
            {
                printf("access-check: REMOTE_WRITE without LOCAL_WRITE rejected: %s (%s)\n",error_enumstr(errno),std::strerror(errno));
            }
            else
            {
                fprintf(stderr,"unexpected successful register from --check-access\n");
                return 3;

            }

        }
        //  teardown & handle errors
        std::printf("teardown: mr=%s pd=%s context=%s\n",
                    static_cast<bool>(mr)  ? "ok" : "n/a",
                    static_cast<bool>(pd)  ? "ok" : "n/a",
                    static_cast<bool>(ctx) ? "ok" : "n/a");
        return 0;
    }
    catch (const limen::VerbsError& e)
    {
        std::fprintf(stderr, "%s\n", e.what());
        return 3;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}