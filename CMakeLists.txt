cmake_minimum_required(VERSION 3.20)
project(DeltaDB VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_compile_options(-Wall -Wextra -O2)


# 手动查找 crc32c
find_path(CRC32C_INCLUDE_DIR
    NAMES crc32c/crc32c.h
    PATHS /usr/local/include /usr/include
    REQUIRED
)

find_library(CRC32C_LIBRARY
    NAMES crc32c libcrc32c
    PATHS /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu
    REQUIRED
)

if(CRC32C_INCLUDE_DIR AND CRC32C_LIBRARY)
    message(STATUS "Found crc32c: ${CRC32C_LIBRARY}")
    message(STATUS "crc32c include dir: ${CRC32C_INCLUDE_DIR}")
    
    # 创建导入目标
    add_library(crc32c::crc32c UNKNOWN IMPORTED)
    set_target_properties(crc32c::crc32c PROPERTIES
        IMPORTED_LOCATION "${CRC32C_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${CRC32C_INCLUDE_DIR}"
    )
else()
    message(FATAL_ERROR "crc32c not found! Please install it first.")
endif()


find_package(ZLIB REQUIRED)
find_package(GTest REQUIRED)     # <-- ?????

# TinyXML 1.x????? 1.x ??? 2?
find_path(TINYXML_INCLUDE_DIR tinyxml.h 
          PATHS /usr/include /usr/local/include)
find_library(TINYXML_LIBRARY tinyxml 
             PATHS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib)

if(TINYXML_INCLUDE_DIR AND TINYXML_LIBRARY)
    message(STATUS "Found TinyXML: ${TINYXML_LIBRARY}")
    add_library(tinyxml::tinyxml UNKNOWN IMPORTED)
    set_target_properties(tinyxml::tinyxml PROPERTIES
        IMPORTED_LOCATION "${TINYXML_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${TINYXML_INCLUDE_DIR}")
else()
    message(FATAL_ERROR "TinyXML not found! Run: sudo apt install libtinyxml-dev")
endif()

# ???
add_subdirectory(src/utils)
add_subdirectory(src/wal)
add_subdirectory(src/table)
add_subdirectory(src/db)
add_subdirectory(app)

enable_testing()
add_subdirectory(test)