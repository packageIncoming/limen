#pragma once
#include <cstddef>
#include <cstdint>

namespace limen {
    constexpr uint64_t slot_addr(uint64_t base, uint32_t slot, uint32_t msg_size) noexcept {
        return base + static_cast<uint64_t>(slot) * msg_size;
    }

    void   fill_pattern(void* buf, size_t len, unsigned iter) noexcept;
    size_t verify_pattern(const void* buf, size_t len, unsigned iter) noexcept;  // 0 = match, else offset+1
}