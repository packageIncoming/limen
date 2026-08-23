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

# Create the shared library for common logic
add_library(limen_common SHARED src/limen_common.cpp)
target_compile_options(limen_common PRIVATE -Wall -Wextra)

# PUBLIC includes make sure dependent binaries find headers
target_include_directories(limen_common 
    PUBLIC include ${IBVERBS_INCLUDE_DIR} ${RDMACM_INCLUDE_DIR}
)

# PUBLIC link ensures executables pull in ibverbs and rdmacm symbols
target_link_libraries(limen_common PUBLIC ${IBVERBS_LIB} ${RDMACM_LIB})

# Executable 1: limen_devinfo
add_executable(limen_devinfo src/limen_devinfo.cpp)
target_compile_options(limen_devinfo PRIVATE -Wall -Wextra)
target_link_libraries(limen_devinfo PRIVATE limen_common)

# Executable 2: limen_connect
add_executable(limen_connect src/limen_connect.cpp)
target_compile_options(limen_connect PRIVATE -Wall -Wextra)
target_link_libraries(limen_connect PRIVATE limen_common)

# Executable 3: limen_pingpong
add_executable(limen_pingpong src/limen_pingpong.cpp)
target_compile_options(limen_pingpong PRIVATE -Wall -Wextra)
target_link_libraries(limen_pingpong PRIVATE limen_common)
