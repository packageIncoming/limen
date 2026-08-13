#ifndef limen_devinfo_h
#define limen_devinfo_h

typedef struct {
    const char* device_name;
    unsigned long long  port;
    unsigned long long buffer_size;
    int check_access_flag;
} parsed_args;


// performs getopt_long & packs results into args_container
void parse_argv(int argc, char* argv[],parsed_args* args_container);

// enumerates devices according to TRD01:R2 & returns a return code
// 0-> successful
// 2-> no devices registered
// 3-> verbs error 
// print_to_err =1-> print to std::cerr
int enumerate_devices(ibv_device** devices_list, int print_to_err);

//  todo docstring
const char* error_enumstr(int errn);

//  todo docstring
void graceful_exit(
    int exit_code,
    ibv_device** device_list=nullptr,
    ibv_context* device_context=nullptr,
    ibv_pd* pd=nullptr,
    ibv_mr* mr=nullptr,
    void* mr_buffer=nullptr
);

#endif