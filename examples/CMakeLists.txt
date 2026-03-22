add_executable(fiberuv_demo
    demo.cpp
    demo_startup.cpp
    demo_file.cpp
    demo_blocking.cpp
    demo_network.cpp
    demo_ipc.cpp
)
target_link_libraries(fiberuv_demo PRIVATE Fiberuv)

if(FIBERUV_LOW_MEMORY_DEBUG AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(fiberuv_demo PRIVATE
        $<$<CONFIG:Debug>:-g0>
        $<$<CONFIG:Debug>:-fno-var-tracking>
        $<$<CONFIG:Debug>:-fno-var-tracking-assignments>
    )
endif()

if(FIBERUV_BUILD_LIBPQ_EXAMPLE)
    find_path(FIBERUV_LIBPQ_INCLUDE_DIR
        NAMES libpq-fe.h
        PATHS
            ${PROJECT_SOURCE_DIR}/third_party/libpq-install/include
    )
    find_library(FIBERUV_LIBPQ_LIBRARY
        NAMES pq libpq
        PATHS
            ${PROJECT_SOURCE_DIR}/third_party/libpq-install/lib
            ${PROJECT_SOURCE_DIR}/third_party/libpq-install/lib64
    )

    if(NOT FIBERUV_LIBPQ_INCLUDE_DIR OR NOT FIBERUV_LIBPQ_LIBRARY)
        message(FATAL_ERROR "libpq not found under third_party/libpq-install. Run scripts/fetch_libpq_source.sh and scripts/build_libpq.sh first.")
    endif()

    add_executable(fiberuv_libpq_demo libpq_fd_demo.cpp)
    target_include_directories(fiberuv_libpq_demo PRIVATE
        ${FIBERUV_LIBPQ_INCLUDE_DIR}
        ${PROJECT_SOURCE_DIR}/third_party/libpq-src/src/include
    )
    target_link_libraries(fiberuv_libpq_demo PRIVATE Fiberuv ${FIBERUV_LIBPQ_LIBRARY})
    if(FIBERUV_LOW_MEMORY_DEBUG AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(fiberuv_libpq_demo PRIVATE
            $<$<CONFIG:Debug>:-g0>
            $<$<CONFIG:Debug>:-fno-var-tracking>
            $<$<CONFIG:Debug>:-fno-var-tracking-assignments>
        )
    endif()
    set_target_properties(fiberuv_libpq_demo PROPERTIES
        BUILD_RPATH "${PROJECT_SOURCE_DIR}/third_party/libpq-install/lib"
    )
endif()
