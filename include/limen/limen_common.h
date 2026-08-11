#ifndef limen_common
#define limen_common
#include <stdint.h>

// parses out a uint64_t value into val_addr; returns 0 on success and 1 on failure
bool parse_u64_strict(const char* str, uint64_t* val_addr);

#endif