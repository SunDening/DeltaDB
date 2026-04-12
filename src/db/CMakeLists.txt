file(GLOB DB_SOURCES *.cpp *.cc)

if(DB_SOURCES)
    add_library(db STATIC ${DB_SOURCES})

    target_include_directories(db
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )

    target_link_libraries(db
        PUBLIC
            DeltaDB::utils
            DeltaDB::wal
            DeltaDB::table
    )

    add_library(DeltaDB::db ALIAS db)
else()
    add_library(db INTERFACE)
    target_include_directories(db
        INTERFACE
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    add_library(DeltaDB::db ALIAS db)
endif()
