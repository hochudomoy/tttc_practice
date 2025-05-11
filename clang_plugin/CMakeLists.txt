cmake_minimum_required(VERSION 3.10)
project(UserTypeInfoPlugin)

find_package(Clang REQUIRED CONFIG)

add_library(UserTypeInfoPlugin MODULE UserTypeInfoPlugin.cpp)

target_include_directories(UserTypeInfoPlugin PRIVATE ${CLANG_INCLUDE_DIRS})
target_link_libraries(UserTypeInfoPlugin PRIVATE clang)

