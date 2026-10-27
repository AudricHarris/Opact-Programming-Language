cmake_minimum_required(VERSION 3.10)
project(OpactProject C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(LLVM REQUIRED CONFIG)

include_directories(include ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")

add_executable(opact ${SOURCES})
add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
add_link_options(-fsanitize=address,undefined)

if(TARGET LLVM)
    target_link_libraries(opact PRIVATE LLVM)
else()
    target_link_libraries(opact PRIVATE ${LLVM_LIBRARIES})
endif()
