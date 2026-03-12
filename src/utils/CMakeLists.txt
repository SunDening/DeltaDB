file(GLOB_RECURSE UTILS_SOURCES *.cpp)

add_library(db_utils STATIC ${UTILS_SOURCES})

target_include_directories(db_utils
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(db_utils
    PUBLIC
        spdlog::spdlog
    PRIVATE
        xxhash::xxhash
        Snappy::snappy
        ZLIB::ZLIB
)

add_library(DeltaDB::utils ALIAS db_utils)