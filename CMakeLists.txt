cmake_minimum_required(VERSION 3.20)

project(torirender VERSION 0.1.0 LANGUAGES CXX)

# Build toggles for optional components and dependency fetching.
option(TORIRENDER_BUILD_TESTS "Build unit tests" ON)
option(TORIRENDER_ENABLE_MPI "Enable MPI integration when available" ON)
option(TORIRENDER_ENABLE_OPENMP "Enable OpenMP integration when available" ON)
option(TORIRENDER_FETCH_CATCH2 "Fetch Catch2 if not found" ON)
option(TORIRENDER_FETCH_STB "Fetch stb headers if not found locally" ON)

# Project-wide C++ language settings.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Core library shared by apps and tests.
add_library(torirender_core
    src/math/Vec3.cpp
    src/core/Ray.cpp
    src/core/Camera.cpp
    src/geometry/AABB.cpp
    src/geometry/Torus.cpp
    src/geometry/BVH.cpp
    src/io/Image.cpp
)

target_include_directories(torirender_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# parallel/runtime backends.
if(TORIRENDER_ENABLE_MPI)
    find_package(MPI QUIET COMPONENTS CXX)
    if(MPI_CXX_FOUND)
        target_link_libraries(torirender_core PUBLIC MPI::MPI_CXX)
        target_compile_definitions(torirender_core PUBLIC TORIRENDER_USE_MPI=1)
    else()
        message(STATUS "MPI CXX not found. Continuing without MPI link target.")
    endif()
endif()

if(TORIRENDER_ENABLE_OPENMP)
    find_package(OpenMP QUIET)
    if(OpenMP_CXX_FOUND)
        target_link_libraries(torirender_core PUBLIC OpenMP::OpenMP_CXX)
        target_compile_definitions(torirender_core PUBLIC TORIRENDER_USE_OPENMP=1)
    else()
        message(STATUS "OpenMP not found. Continuing without OpenMP link target.")
    endif()
endif()

# stb is used for PNG writing support.
set(TORIRENDER_STB_INCLUDE_DIR "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb/stb_image_write.h")
    set(TORIRENDER_STB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb")
elseif(TORIRENDER_FETCH_STB)
    include(FetchContent)
    FetchContent_Declare(
        stb
        GIT_REPOSITORY https://github.com/nothings/stb.git
        GIT_TAG master
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(stb)
    set(TORIRENDER_STB_INCLUDE_DIR "${stb_SOURCE_DIR}")
endif()

if(TORIRENDER_STB_INCLUDE_DIR)
    target_include_directories(torirender_core PUBLIC ${TORIRENDER_STB_INCLUDE_DIR})
    target_compile_definitions(torirender_core PUBLIC TORIRENDER_HAS_STB_IMAGE_WRITE=1)
else()
    message(STATUS "stb_image_write.h unavailable. PNG output disabled; PPM output remains available.")
endif()

# JSON dependency is for scene/config handling.
find_package(nlohmann_json QUIET)
if(nlohmann_json_FOUND)
    target_link_libraries(torirender_core PUBLIC nlohmann_json::nlohmann_json)
    target_compile_definitions(torirender_core PUBLIC TORIRENDER_HAS_JSON=1)
else()
    message(STATUS "nlohmann_json not found. JSON support can be enabled later for scene/config parsing.")
endif()

# Week-1 CLI utility.
add_executable(point_classifier cli/point_classifier.cpp)
target_link_libraries(point_classifier PRIVATE torirender_core)

# Unit tests (Catch2) are enabled when available.
include(CTest)
if(BUILD_TESTING AND TORIRENDER_BUILD_TESTS)
    find_package(Catch2 3 QUIET)

    if(NOT Catch2_FOUND AND TORIRENDER_FETCH_CATCH2)
        include(FetchContent)
        FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG v3.6.0
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(Catch2)
    endif()

    if(TARGET Catch2::Catch2WithMain)
        add_executable(torirender_tests
            tests/unit/test_vec3.cpp
            tests/unit/test_torus.cpp
            tests/unit/test_bvh.cpp
            tests/unit/test_camera.cpp
        )
        target_link_libraries(torirender_tests PRIVATE torirender_core Catch2::Catch2WithMain)
        add_test(NAME torirender_unit_tests COMMAND torirender_tests)
    else()
        message(WARNING "Catch2 not available. Unit test target was not generated.")
    endif()
endif()
