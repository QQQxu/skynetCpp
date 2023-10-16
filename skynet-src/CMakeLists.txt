file(GLOB_RECURSE skynet_src ${PROJECT_SOURCE_DIR}/skynet-src/*.cpp)
add_library(skynet SHARED ${skynet_src})
target_include_directories(skynet PUBLIC include)


