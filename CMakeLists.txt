cmake_minimum_required(VERSION 3.15)
project(EuroScopeWsData CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Folder holding EuroScopePlugIn.h and EuroScopePlugInDll.lib (copy them from your EuroScope install).
set(ES_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdk" CACHE PATH "EuroScope plugin SDK folder")
# OFF builds only fake_feed on Windows (used by CI when the SDK isn't available).
option(ES_BUILD_PLUGIN "Build the EuroScope plugin DLL (needs the SDK in ES_SDK_DIR)" ON)
# Version reported to EuroScope; the release workflow sets it from the git tag.
set(WSDATA_VERSION "0.1.0" CACHE STRING "Plugin version string")

# Static CRT so the DLL/exe have no VC++ redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC AND ES_BUILD_PLUGIN)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "EuroScope is 32-bit. Configure with: cmake -A Win32 ..")
    endif()
    if(NOT EXISTS "${ES_SDK_DIR}/EuroScopePlugIn.h")
        message(FATAL_ERROR "EuroScopePlugIn.h not found in ${ES_SDK_DIR}. See docs/development.md.")
    endif()

    add_library(WsDataPlugin SHARED src/Plugin.cpp src/WsServer.cpp)
    target_include_directories(WsDataPlugin PRIVATE src)
    # Treat the SDK as an external header: /W4 would otherwise report ~90 C4100 "unreferenced
    # parameter" warnings from its default virtual handlers, which we can't change.
    target_compile_options(WsDataPlugin PRIVATE "/external:I${ES_SDK_DIR}" /external:W0)
    target_link_libraries(WsDataPlugin PRIVATE "${ES_SDK_DIR}/EuroScopePlugInDll.lib" ws2_32)
    target_compile_definitions(WsDataPlugin PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX
        WSDATA_VERSION="${WSDATA_VERSION}")
    target_compile_options(WsDataPlugin PRIVATE /W4 /utf-8)
elseif(NOT MSVC)
    # Off Windows we can't build the plugin, but we can run the same server with fake traffic.
    find_package(Threads REQUIRED)
    add_executable(fake_feed tools/fake_feed.cpp src/WsServer.cpp)
    target_include_directories(fake_feed PRIVATE src)
    target_link_libraries(fake_feed PRIVATE Threads::Threads)
endif()

# Windows can build the fake feed too (handy for testing clients without EuroScope running).
if(MSVC)
    add_executable(fake_feed tools/fake_feed.cpp src/WsServer.cpp)
    target_include_directories(fake_feed PRIVATE src)
    target_link_libraries(fake_feed PRIVATE ws2_32)
endif()
