cmake_minimum_required(VERSION 3.22)
project(ex3_os)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS -pthread)


add_executable(ex3_os MapReduceFramework.cpp test4.cpp)
