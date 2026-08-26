cmake_minimum_required(VERSION 3.16)
project(limen LANGUAGES CXX)

option(LIMEN_ASAN "Build with AddressSanitizer" OFF)
if(LIMEN_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find RDMA packages
find_library(IBVERBS_LIB ibverbs REQUIRED)
find_library(RDMACM_LIB rdmacm REQUIRED)
find_path(IBVERBS_INCLUDE_DIR infiniband/verbs.h REQUIRED)
find_path(RDMACM_INCLUDE_DIR rdma/rdma_cma.h REQUIRED)

message(STATUS "libibverbs: ${IBVERBS_LIB}")
message(STATUS "librdmacm: ${RDMACM_LIB}")

# Shared library. Named "limen" so the test probes' -llimen resolves.
add_library(limen STATIC src/limen_common.cpp src/verbs.cpp)
target_compile_options(limen PRIVATE -Wall -Wextra)

target_include_directories(limen
  PUBLIC include ${IBVERBS_INCLUDE_DIR} ${RDMACM_INCLUDE_DIR}
)

target_link_libraries(limen PUBLIC ${IBVERBS_LIB} ${RDMACM_LIB})

# Executable 1: limen_devinfo
add_executable(limen_devinfo src/limen_devinfo.cpp)
target_compile_options(limen_devinfo PRIVATE -Wall -Wextra)
target_link_libraries(limen_devinfo PRIVATE limen)

# Executable 2: limen_connect
add_executable(limen_connect src/limen_connect.cpp)
target_compile_options(limen_connect PRIVATE -Wall -Wextra)
target_link_libraries(limen_connect PRIVATE limen)

# Executable 3: limen_pingpong
add_executable(limen_pingpong src/limen_pingpong.cpp)
target_compile_options(limen_pingpong PRIVATE -Wall -Wextra)
target_link_libraries(limen_pingpong PRIVATE limen)

# Tests
add_executable(static_contract tests/static_contract.cpp)
target_compile_options(static_contract PRIVATE -Wall -Wextra)
target_link_libraries(static_contract PRIVATE limen)

# Must NOT compile. Excluded from the default build; the suite asks for it directly.
add_executable(copy_must_not_compile EXCLUDE_FROM_ALL tests/copy_must_not_compile.cpp)
target_link_libraries(copy_must_not_compile PRIVATE limen)