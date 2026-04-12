file(GLOB UTILS_SOURCES *.cpp *.cc)

add_library(utils STATIC ${UTILS_SOURCES})

target_include_directories(utils
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(utils
    PUBLIC
        tinyxml::tinyxml
    PRIVATE
        ZLIB::ZLIB
)

add_library(DeltaDB::utils ALIAS utils)
