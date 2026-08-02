cmake_minimum_required(VERSION 3.15)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-tree builds are not supported. Use: cmake -S . -B build -A x64")
endif()

cmake_policy(SET CMP0091 NEW)

project(aidbg LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "aidbg only supports Windows")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "aidbg currently requires an x64 build (configure with -A x64)")
endif()

# Keep all runtime artifacts together so aidbg.exe finds TitanEngine.dll.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set("CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper}" "${CMAKE_BINARY_DIR}/bin")
endforeach()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/TitanEngine/CMakeLists.txt")
    message(FATAL_ERROR
        "TitanEngine submodule is missing. Run: git submodule update --init --recursive")
endif()

# Use the same official TitanEngine submodule and pinned revision as x64dbg.
set(TITANENGINE_RESOURCES ON CACHE BOOL "Build TitanEngine resources" FORCE)
add_subdirectory(third_party/TitanEngine)

add_executable(aidbg aidbg.cpp)
target_compile_features(aidbg PRIVATE cxx_std_17)
target_link_libraries(aidbg PRIVATE TitanEngine)

if(MSVC)
    target_compile_options(aidbg PRIVATE /utf-8)
    set_target_properties(aidbg PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # Delay-load TitanEngine.dll so main() can put the real module directory on
    # the DLL search path first (needed when launched via a symlink, e.g. the
    # winget portable Links\aidbg.exe -> Packages\aidbg.exe indirection).
    target_link_libraries(aidbg PRIVATE Delayimp.lib)
    target_link_options(aidbg PRIVATE "/DELAYLOAD:TitanEngine.dll")
endif()

set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT aidbg)

install(TARGETS aidbg TitanEngine RUNTIME DESTINATION .)
