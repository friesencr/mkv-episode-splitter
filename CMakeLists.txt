cmake_minimum_required(VERSION 3.20)
project(mkv-splitter VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find FFmpeg using PkgConfig
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(AVFILTER REQUIRED IMPORTED_TARGET libavfilter)

# Core Shared Engine Library
add_library(mkvsplit_core STATIC
    src/core.cpp
    src/ffmpeg_demuxer.cpp
    src/utils.cpp
)

target_include_directories(mkvsplit_core PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(mkvsplit_core PUBLIC 
    PkgConfig::AVFORMAT 
    PkgConfig::AVCODEC 
    PkgConfig::AVUTIL
    PkgConfig::AVFILTER
)

# CLI Binary
add_executable(mkvsplit-cli src/cli_main.cpp)
target_link_libraries(mkvsplit-cli PRIVATE mkvsplit_core)

include(CTest)
if(BUILD_TESTING)
    find_program(FFMPEG_EXECUTABLE ffmpeg REQUIRED)
    add_executable(mkvsplit_tests tests/test_splitter.cpp)
    target_link_libraries(mkvsplit_tests PRIVATE mkvsplit_core)
    add_test(NAME splitter COMMAND mkvsplit_tests)
    set_tests_properties(splitter PROPERTIES TIMEOUT 180)
endif()
