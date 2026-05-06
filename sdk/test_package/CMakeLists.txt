cmake_minimum_required(VERSION 3.18)
project(test_package CXX)

find_package(loom REQUIRED)

add_executable(test_package test_package.cpp)
target_link_libraries(test_package PRIVATE loom::sdk)

