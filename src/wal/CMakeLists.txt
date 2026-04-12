file(GLOB WAL_SOURCES *.cpp *.cc)

if(WAL_SOURCES)
    add_library(wal STATIC ${WAL_SOURCES})

    target_include_directories(wal
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )

    target_link_libraries(wal
        PUBLIC
            DeltaDB::utils
    )

    add_library(DeltaDB::wal ALIAS wal)
else()
    add_library(wal INTERFACE)
    target_include_directories(wal
        INTERFACE
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    add_library(DeltaDB::wal ALIAS wal)
endif()
