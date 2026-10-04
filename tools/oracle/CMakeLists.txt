# mgba_oracle: reference runs on mGBA for docs/conformance.md. Optional and
# separate from the toolkit build; needs a built libmgba:
#   cmake -S tools/oracle -B build-oracle -DMGBA_DIR=<mgba checkout with build/>
#   cmake --build build-oracle --config Release
cmake_minimum_required(VERSION 3.16)
project(mgba_oracle C)

set(MGBA_DIR "" CACHE PATH "mGBA source checkout containing a built build/ (Release/mgba.lib)")
if(NOT MGBA_DIR)
    message(FATAL_ERROR "Set -DMGBA_DIR=<path to mgba checkout>")
endif()

add_executable(mgba_oracle mgba_oracle.c)
target_include_directories(mgba_oracle PRIVATE ${MGBA_DIR}/include ${MGBA_DIR}/build/include ${MGBA_DIR}/src)
target_compile_definitions(mgba_oracle PRIVATE
    M_CORE_GBA ENABLE_VFS ENABLE_VFS_FD ENABLE_DIRECTORIES BUILD_STATIC
    HAVE_CRC32 HAVE_STRDUP HAVE_SETLOCALE NOMINMAX WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS color_t=uint32_t)
if(WIN32)
    target_link_libraries(mgba_oracle PRIVATE ${MGBA_DIR}/build/Release/mgba.lib ws2_32 shlwapi)
else()
    target_link_libraries(mgba_oracle PRIVATE ${MGBA_DIR}/build/libmgba.a m pthread z)
endif()
