cmake_minimum_required(VERSION 3.20)
project(DeltaDB VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)

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

    # 创建导入目标，仅暴露 crc32c 子目录而非整个 /usr/local/include
    add_library(crc32c::crc32c UNKNOWN IMPORTED)
    set_target_properties(crc32c::crc32c PROPERTIES
        IMPORTED_LOCATION "${CRC32C_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${CRC32C_INCLUDE_DIR}"
        # 使用 SYSTEM 标记，降低其优先级并抑制警告
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${CRC32C_INCLUDE_DIR}"
    )
else()
    message(FATAL_ERROR "crc32c not found! Please install it first.")
endif()


find_package(ZLIB REQUIRED)

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

# Optional: build app and tests (off by default when used as subproject)
option(DELTA_DB_BUILD_APP "Build the deltadb application" OFF)
option(DELTA_DB_BUILD_TESTS "Build tests" OFF)

if(DELTA_DB_BUILD_TESTS)
    find_package(GTest REQUIRED)
endif()

# Build library subdirectories
add_subdirectory(src/utils)
add_subdirectory(src/wal)
add_subdirectory(src/table)
add_subdirectory(src/db)

# Optionally build app and tests
if(DELTA_DB_BUILD_APP)
    add_subdirectory(app)
endif()

if(DELTA_DB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

# ============================================================
# Install rules (for `cmake --install` or `make install`)
# ============================================================

# Install the unified library target (you can choose one or keep separate libs)
install(TARGETS db table wal utils
    EXPORT DeltaDBTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Install the single public header tree used by the build:
#   src/include/deltadb/... -> <prefix>/<includedir>/deltadb/...
install(DIRECTORY src/include/deltadb/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/deltadb
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

# Export targets for downstream CMake projects
install(EXPORT DeltaDBTargets
    FILE DeltaDBTargets.cmake
    NAMESPACE DeltaDB::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DeltaDB
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeltaDBConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/DeltaDBConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DeltaDB
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/DeltaDBConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/DeltaDBConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/DeltaDBConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DeltaDB
)
