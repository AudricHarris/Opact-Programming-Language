cmake_minimum_required(VERSION 3.10)
project(OpactProject CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include_directories(src)
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")
# Change the target name here:
add_executable(opact ${SOURCES})

