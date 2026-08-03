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

//  returns the index of the first ibv_device whose name 
//  matches device_name; returns -1 if not found
int find_device_by_name(ibv_device** devices_list, const char* device_name);

void print_device_info(ibv_device_attr* attr);

void print_port_info(ibv_port_attr* attr);

void allocate_protection_domain();

void register_memregion();




#endif