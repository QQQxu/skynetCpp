cmake_minimum_required(VERSION 3.10)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-glldb)

project(Skynet VERSION 0.1
                  DESCRIPTION "skynet"
                  LANGUAGES CXX)


add_subdirectory(skynet-src)
add_subdirectory(service-src)


add_executable(demo main.cpp)
target_link_libraries(demo PUBLIC skynet)

set(MAIN_PATH ${PROJECT_BINARY_DIR})
configure_file (
    "${PROJECT_SOURCE_DIR}/config.h.in"
    "${PROJECT_SOURCE_DIR}/config.h"
)

