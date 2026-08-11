#include "limen/limen_common.h"
#include <stdlib.h>
#include <errno.h>
#include <cstdlib>  


bool parse_u64_strict(const char* str, uint64_t* val_addr)
{
    char *end = NULL;
    errno = 0;
    uint64_t v = strtoull(str, &end, 10);
    if (errno || end == str || *end != '\0' || v == 0) { return 1;}
    *val_addr= v;
    return 0;
}
