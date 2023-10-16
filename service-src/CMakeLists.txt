
add_library(logger SHARED service_logger.cpp)
target_link_libraries(logger PUBLIC skynet)

add_library(harbor SHARED service_harbor.cpp)
target_link_libraries(harbor PUBLIC skynet)

add_library(gate SHARED service_gate.cpp)
target_link_libraries(gate PUBLIC skynet)