#pragma once
#include "limen/pattern.hpp"
#include <cstdio>


namespace limen 
{
    void fill_pattern(void *buf, size_t len, unsigned iter) noexcept
    {
        unsigned char *p = static_cast<unsigned char*>(buf);
        for (size_t i = 0; i < len; i++)
            p[i] = (unsigned char)((i * 31u + iter * 131u) & 0xff);
    }

    size_t verify_pattern(const void *buf, size_t len, unsigned iter) noexcept 
    {
        const unsigned char *p = static_cast<const unsigned char*>(buf);
        for (size_t i = 0; i < len; i++) {
            unsigned char want = (unsigned char)((i * 31u + iter * 131u) & 0xff);
            if (p[i] != want) {
                fprintf(stderr, "mismatch at offset %zu: expected 0x%02x got 0x%02x "
                                "(iteration %u)\n", i, want, p[i], iter);
                return i + 1;                 /* non-zero = mismatch, and where */
            }
        }
        return 0;
    }
}
