cmake_minimum_required(VERSION 3.16)
project(limen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_library(IBVERBS_LIB ibverbs REQUIRED)
find_library(RDMACM_LIB  rdmacm  REQUIRED)
find_path(IBVERBS_INCLUDE_DIR infiniband/verbs.h REQUIRED)
find_path(RDMACM_INCLUDE_DIR  rdma/rdma_cma.h    REQUIRED)

message(STATUS "libibverbs: ${IBVERBS_LIB}")
message(STATUS "librdmacm:  ${RDMACM_LIB}")

# TRD-01 adds the first target here.
include_directories(include)
add_executable(limen_devinfo src/limen_devinfo.cpp)
set_target_properties(limen_devinfo PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(limen_devinfo PRIVATE -Wall -Wextra)
target_link_libraries(limen_devinfo PRIVATE ${IBVERBS_LIB})
target_include_directories(limen_devinfo PRIVATE ${IBVERBS_INCLUDE_DIR} include)