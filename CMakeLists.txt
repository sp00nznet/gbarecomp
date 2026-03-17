cmake_minimum_required(VERSION 3.16)
project(gbarecomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Warnings
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Sources
set(SOURCES
    src/main.c
    src/rom.c
    src/arm.c
    src/thumb.c
    src/disasm.c
)

# Main executable
add_executable(gbarecomp ${SOURCES})
target_include_directories(gbarecomp PRIVATE include)

# Install
install(TARGETS gbarecomp RUNTIME DESTINATION bin)
