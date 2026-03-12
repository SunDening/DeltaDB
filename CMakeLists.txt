cmake_minimum_required(VERSION 3.20)
project(DeltaDB VERSION 1.0.0 LANGUAGES CXX)

# C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 编译选项
add_compile_options(-Wall -Wextra -O2)

include(FetchContent)

# ==================== spdlog (FetchContent) ====================

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
    GIT_SHALLOW    TRUE
)

set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(spdlog)

message(STATUS "spdlog ready: ${spdlog_SOURCE_DIR}")

# ==================== 系统依赖 ====================

# TinyXML1
find_path(TINYXML_INCLUDE_DIR tinyxml.h PATHS /usr/include /usr/local/include)
find_library(TINYXML_LIBRARY tinyxml PATHS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib)
if(TINYXML_INCLUDE_DIR AND TINYXML_LIBRARY)
    message(STATUS "Found TinyXML: ${TINYXML_LIBRARY}")
    add_library(tinyxml::tinyxml UNKNOWN IMPORTED)
    set_target_properties(tinyxml::tinyxml PROPERTIES
        IMPORTED_LOCATION "${TINYXML_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${TINYXML_INCLUDE_DIR}")
else()
    message(FATAL_ERROR "TinyXML not found! Run: sudo apt install libtinyxml-dev")
endif()

# xxHash
find_path(XXHASH_INCLUDE_DIR xxhash.h PATHS /usr/include /usr/local/include)
find_library(XXHASH_LIBRARY NAMES xxhash libxxhash PATHS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib)
if(XXHASH_INCLUDE_DIR AND XXHASH_LIBRARY)
    message(STATUS "Found xxHash: ${XXHASH_LIBRARY}")
    add_library(xxhash::xxhash UNKNOWN IMPORTED)
    set_target_properties(xxhash::xxhash PROPERTIES
        IMPORTED_LOCATION "${XXHASH_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${XXHASH_INCLUDE_DIR}")
else()
    message(FATAL_ERROR "xxHash not found! Run: sudo apt install libxxhash-dev")
endif()

# Snappy
find_package(Snappy REQUIRED)

# ZLIB
find_package(ZLIB REQUIRED)

# GTest
find_package(GTest REQUIRED)

# ==================== 子目录 ====================

add_subdirectory(src)
add_subdirectory(app)
add_subdirectory(test)