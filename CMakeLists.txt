cmake_minimum_required(VERSION 3.20)
project(transit_router LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Protobuf REQUIRED)

add_executable(router
    main.cpp
    gtfs/feed.cpp
    routing/connections.cpp
    routing/csa.cpp
    routing/journey.cpp
    realtime/realtime.cpp
    realtime/build/gen/gtfs-realtime.pb.cc
)

target_include_directories(router PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(router PRIVATE protobuf::libprotobuf)

if(MSVC)
    target_compile_options(router PRIVATE /W4 /utf-8)
else()
    target_compile_options(router PRIVATE -Wall -Wextra -Wpedantic)
endif()
