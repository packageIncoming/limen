#pragma once 
#include <cstdint>

namespace limen::app 
{
    // parses out a uint64_t value into val_addr; returns 0 on success and 1 on failure
    int parse_u64_strict(const char* str, uint64_t* val_addr);

    // parses out a int value into val_addr; returns 0 on success and 1 on failure
    int parse_int_strict(const char* str, int* val_addr);

}
