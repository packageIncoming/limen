#include <limen/cm.hpp>
#include <type_traits>

template <typename T>
constexpr void check() {
    static_assert(!std::is_copy_constructible_v<T>,        "must not be copyable");
    static_assert(!std::is_copy_assignable_v<T>,           "must not be copy-assignable");
    static_assert(std::is_nothrow_move_constructible_v<T>, "move ctor must be noexcept");
    static_assert(std::is_nothrow_move_assignable_v<T>,    "move assign must be noexcept");
    static_assert(std::is_nothrow_destructible_v<T>,       "dtor must be noexcept");
    static_assert(std::is_nothrow_default_constructible_v<T>, "empty state must exist");
}

int main() {
    check<limen::EventChannel>();
    check<limen::ConnectionId>();
    check<limen::Event>();

}