file(GLOB TABLE_SOURCES *.cpp *.cc)

if(TABLE_SOURCES)
    add_library(table STATIC ${TABLE_SOURCES})

    target_include_directories(table
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )

    target_link_libraries(table
        PUBLIC
            DeltaDB::utils
            DeltaDB::wal
    )

    add_library(DeltaDB::table ALIAS table)
else()
    add_library(table INTERFACE)
    target_include_directories(table
        INTERFACE
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    add_library(DeltaDB::table ALIAS table)
endif()
