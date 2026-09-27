cmake_minimum_required(VERSION 3.20)

project(RealtimeModel LANGUAGES CXX)

# Header-only library: just the include paths, no sources to compile.
add_library(RealtimeModel INTERFACE
        RealtimeValue.hpp
        Writer.hpp
        RealtimeAssetMap.hpp)
target_include_directories(RealtimeModel INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(RealtimeModel INTERFACE cxx_std_23)

include(cmake/CPM.cmake)
CPMAddPackage("gh:catchorg/Catch2@3.5.4")
CPMAddPackage("gh:cameron314/concurrentqueue@1.0.4")
add_subdirectory(tests)
