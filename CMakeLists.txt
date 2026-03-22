cmake_minimum_required(VERSION 3.24)

project(Fiberuv VERSION 0.1.0 LANGUAGES CXX)

if(WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN)
endif()

option(FIBERUV_USE_LLD "Use lld linker when available" ON)
if(FIBERUV_USE_LLD AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    find_program(FIBERUV_LLD_PROGRAM NAMES ld.lld lld)
    if(FIBERUV_LLD_PROGRAM)
        add_link_options("-fuse-ld=lld")
    else()
        message(STATUS "lld linker not found; using default linker.")
    endif()
endif()

# Avoid OOM from parallel link steps under Ninja (especially with large FetchContent deps).
if(CMAKE_GENERATOR MATCHES "Ninja")
    set_property(GLOBAL PROPERTY JOB_POOLS link_pool=1)
    set(CMAKE_JOB_POOL_LINK link_pool)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FIBERUV_BUILD_EXAMPLES "Build Fiberuv examples" OFF)
option(FIBERUV_BUILD_LIBPQ_EXAMPLE "Build libpq fd-wait example (requires third_party/libpq-install)" OFF)
option(FIBERUV_LOW_MEMORY_DEBUG "Reduce GCC Debug compile memory footprint" ON)

include(FetchContent)
find_package(Threads REQUIRED)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# libuv: download source and build locally.
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.28")
    FetchContent_Declare(
        libuv
        URL https://dist.libuv.org/dist/v1.49.2/libuv-v1.49.2.tar.gz
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(libuv)
else()
    FetchContent_GetProperties(libuv)
    if(NOT libuv_POPULATED)
        FetchContent_Populate(libuv)
        add_subdirectory("${libuv_SOURCE_DIR}" "${libuv_BINARY_DIR}" EXCLUDE_FROM_ALL)
    endif()
endif()

# Boost (with CMake package): download source and build fiber/context locally.
set(BOOST_INCLUDE_LIBRARIES fiber context)
set(BOOST_ENABLE_CMAKE ON)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.28")
    FetchContent_Declare(
        boost
        URL https://github.com/boostorg/boost/releases/download/boost-1.85.0/boost-1.85.0-cmake.tar.xz
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(boost)
else()
    FetchContent_GetProperties(boost)
    if(NOT boost_POPULATED)
        FetchContent_Populate(boost)
        add_subdirectory("${boost_SOURCE_DIR}" "${boost_BINARY_DIR}" EXCLUDE_FROM_ALL)
    endif()
endif()

set(FIBERUV_UV_TARGET "")
if(TARGET uv_a)
    set(FIBERUV_UV_TARGET uv_a)
elseif(TARGET uv)
    set(FIBERUV_UV_TARGET uv)
else()
    message(FATAL_ERROR "Unable to find libuv target.")
endif()

function(fiberuv_pick_target out_var display_name)
    set(_picked "")
    foreach(_candidate IN LISTS ARGN)
        if(TARGET "${_candidate}")
            set(_picked "${_candidate}")
            break()
        endif()
    endforeach()

    if(_picked STREQUAL "")
        get_property(_all_targets GLOBAL PROPERTY TARGETS)
        set(_hints "")
        foreach(_t IN LISTS _all_targets)
            string(TOLOWER "${_t}" _t_lower)
            if(display_name STREQUAL "fiber")
                if(_t_lower MATCHES "boost.*fiber|fiber.*boost")
                    list(APPEND _hints "${_t}")
                endif()
            elseif(display_name STREQUAL "context")
                if(_t_lower MATCHES "boost.*context|context.*boost")
                    list(APPEND _hints "${_t}")
                endif()
            endif()
        endforeach()
        list(REMOVE_DUPLICATES _hints)
        message(STATUS "Detected related CMake targets for Boost ${display_name}: ${_hints}")
        message(FATAL_ERROR "Unable to find Boost ${display_name} target.")
    endif()

    set(${out_var} "${_picked}" PARENT_SCOPE)
endfunction()

set(FIBERUV_BOOST_FIBER_TARGET "")
set(FIBERUV_BOOST_CONTEXT_TARGET "")
fiberuv_pick_target(
    FIBERUV_BOOST_FIBER_TARGET
    fiber
    Boost::fiber
    boost_fiber
    Boost::fibers
    boost_fibers
    boost::fiber
    boost::fibers
)
fiberuv_pick_target(
    FIBERUV_BOOST_CONTEXT_TARGET
    context
    Boost::context
    boost_context
    boost::context
)

add_library(Fiberuv
    src/event_loop.cpp
    src/file_io.cpp
    src/ipc.cpp
    src/io_unified.cpp
    src/tcp.cpp
    src/udp.cpp
)

if(FIBERUV_LOW_MEMORY_DEBUG AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC Debug info generation for heavy template code can spike memory usage.
    # These flags keep Debug usable on lower-memory machines.
    target_compile_options(Fiberuv PRIVATE
        $<$<CONFIG:Debug>:-g0>
        $<$<CONFIG:Debug>:-fno-var-tracking>
        $<$<CONFIG:Debug>:-fno-var-tracking-assignments>
    )
endif()

target_include_directories(Fiberuv
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(Fiberuv
    PUBLIC
        ${FIBERUV_UV_TARGET}
        ${FIBERUV_BOOST_FIBER_TARGET}
        ${FIBERUV_BOOST_CONTEXT_TARGET}
        Threads::Threads
)

if(FIBERUV_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
