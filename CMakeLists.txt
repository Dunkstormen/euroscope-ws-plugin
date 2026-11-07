cmake_minimum_required(VERSION 3.15)
project(EuroScopeWsData CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Folder holding EuroScopePlugIn.h and EuroScopePlugInDll.lib (copy them from your EuroScope install).
set(ES_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdk" CACHE PATH "EuroScope plugin SDK folder")

if(MSVC)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "EuroScope is 32-bit. Configure with: cmake -A Win32 ..")
    endif()
    if(NOT EXISTS "${ES_SDK_DIR}/EuroScopePlugIn.h")
        message(FATAL_ERROR "EuroScopePlugIn.h not found in ${ES_SDK_DIR}. See README.")
    endif()

    # Static CRT so the DLL has no VC++ redistributable dependency.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    add_library(WsDataPlugin SHARED src/Plugin.cpp src/WsServer.cpp)
    target_include_directories(WsDataPlugin PRIVATE src "${ES_SDK_DIR}")
    target_link_libraries(WsDataPlugin PRIVATE "${ES_SDK_DIR}/EuroScopePlugInDll.lib" ws2_32)
    target_compile_definitions(WsDataPlugin PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(WsDataPlugin PRIVATE /W4 /utf-8)
else()
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
