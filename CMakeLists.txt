cmake_minimum_required(VERSION 3.20)
project(microps C)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_FLAGS "-g -W -Wall -Wno-unused-parameter")
set(CMAKE_C_FLAGS_DEBUG "-DHEXDUMP")
include_directories(.)
add_executable(test0 test/step0.c util.c)
add_executable(test1 test/step1.c util.c net.c driver/null.c)

