file(GLOB UTILS_SOURCES *.cpp *.cc)

add_library(utils STATIC ${UTILS_SOURCES})

target_include_directories(utils
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/include
        ${CMAKE_SOURCE_DIR}/src/table/include
)

target_link_libraries(utils
    PUBLIC
        # spdlog::spdlog
        tinyxml::tinyxml
    PRIVATE
        ZLIB::ZLIB
)

add_library(DeltaDB::utils ALIAS utils)