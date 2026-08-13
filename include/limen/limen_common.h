#ifndef limen_common
#define limen_common
#include <stdint.h>
#include <infiniband/verbs.h>

// parses out a uint64_t value into val_addr; returns 0 on success and 1 on failure
bool parse_u64_strict(const char* str, uint64_t* val_addr);

//  returns the index of the first ibv_device whose name 
//  matches device_name; returns -1 if not found
int find_device_by_name(ibv_device** devices_list, const char* device_name);

//  todo docstring
void print_device_info(ibv_device_attr* attr);

//  todo docstring
void print_port_info(ibv_port_attr* attr);

#endif