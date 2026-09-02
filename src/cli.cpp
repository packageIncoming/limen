#include "limen/app/cli.hpp"
#include <cerrno>
#include <cstdlib>

namespace limen::app 
{
    int parse_u64_strict(const char* str, uint64_t* val_addr)
    {
        char *end = NULL;
        errno = 0;
        uint64_t v = strtoull(str, &end, 10);
        if (errno || end == str || *end != '\0' || v == 0) { return 1;}
        *val_addr= v;
        return 0;
    }

    int parse_int_strict(const char* str, int* val_addr)
    {
        errno = 0;
        int v = atoi(str);
        *val_addr= v;
        if (v == 0)
        {
            return 1;
        }
        return 0;
    }

}
