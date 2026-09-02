cmake_minimum_required(VERSION 3.16)
project(limen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to an optimized build. Without this, single-config generators leave
# CMAKE_BUILD_TYPE empty and GCC compiles at -O0, which makes any TRD-08
# latency or throughput number meaningless.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
message(STATUS "build type: ${CMAKE_BUILD_TYPE}")

option(LIMEN_ASAN "Build with AddressSanitizer" OFF)
if(LIMEN_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address)
endif()

# Every target in this project wants these. trd-04's test_builds counts
# warnings, so a new source file must not be able to opt out by omission.
add_compile_options(-Wall -Wextra)

# Find RDMA packages
find_library(IBVERBS_LIB ibverbs REQUIRED)
find_library(RDMACM_LIB rdmacm REQUIRED)
find_path(IBVERBS_INCLUDE_DIR infiniband/verbs.h REQUIRED)
find_path(RDMACM_INCLUDE_DIR rdma/rdma_cma.h REQUIRED)

message(STATUS "libibverbs: ${IBVERBS_LIB}")
message(STATUS "librdmacm: ${RDMACM_LIB}")

# ─────────────────────────────────────────────────────────────────────────────
# The transport. Public surface is whatever <limen.hpp> reaches.
# Named "limen" because the trd-04 probes link with -llimen.
#
# src/verbs.cpp must remain compilable on its own with only -libverbs:
# trd-04 builds two probes as `$CXX probe.cpp src/verbs.cpp -libverbs`.
# Do not let it depend on another TU in this target.
# ─────────────────────────────────────────────────────────────────────────────
add_library(limen STATIC
  src/verbs.cpp
  src/cm.cpp
  src/session.cpp
  src/format.cpp
  src/pattern.cpp
)

target_include_directories(limen PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${IBVERBS_INCLUDE_DIR}
  ${RDMACM_INCLUDE_DIR}
)

target_link_libraries(limen PUBLIC ${IBVERBS_LIB} ${RDMACM_LIB})

# ─────────────────────────────────────────────────────────────────────────────
# Binary-only helpers: argv parsing and program exit codes. Deliberately a
# separate target so nothing under include/limen/app/ can reach the transport's
# public surface, and so the static-contract tests below prove the library
# links without any CLI code.
# ─────────────────────────────────────────────────────────────────────────────
add_library(limen_app STATIC
  src/cli.cpp
)

target_link_libraries(limen_app PUBLIC limen)

# ─────────────────────────────────────────────────────────────────────────────
# Programs
# ─────────────────────────────────────────────────────────────────────────────

# limen_devinfo calls no argv parser, so it links the transport directly.
add_executable(limen_devinfo src/limen_devinfo.cpp)
target_link_libraries(limen_devinfo PRIVATE limen)

# Manual bringup reference. Does not use the CM path and is not ported to Session.
add_executable(limen_connect src/limen_connect.cpp)
target_link_libraries(limen_connect PRIVATE limen_app)

add_executable(limen_pingpong src/limen_pingpong.cpp)
target_link_libraries(limen_pingpong PRIVATE limen_app)

# Uncomment once limen_onesided.cpp is ported to PendingConnection/Session.
# add_executable(limen_onesided src/limen_onesided.cpp)
# target_link_libraries(limen_onesided PRIVATE limen_app)

# ─────────────────────────────────────────────────────────────────────────────
# Tests
# These link "limen" and never "limen_app". If a static-contract test ever
# fails to link because it needs a CLI symbol, the boundary has leaked.
# ─────────────────────────────────────────────────────────────────────────────
add_executable(static_contract tests/static_contract.cpp)
target_link_libraries(static_contract PRIVATE limen)

add_executable(cm_static_contract tests/cm_static_contract.cpp)
target_link_libraries(cm_static_contract PRIVATE limen)

# Must NOT compile. Excluded from the default build; the suite asks for them directly.
add_executable(copy_must_not_compile EXCLUDE_FROM_ALL tests/copy_must_not_compile.cpp)
target_link_libraries(copy_must_not_compile PRIVATE limen)

add_executable(no_raw_private_data EXCLUDE_FROM_ALL tests/no_raw_private_data.cpp)
target_link_libraries(no_raw_private_data PRIVATE limen)