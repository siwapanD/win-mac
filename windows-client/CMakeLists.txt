cmake_minimum_required(VERSION 3.24)
project(windows-client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(windows-client
    src/main.cpp
    src/session/HPClientSession.cpp
    src/decode/MediaFoundationDecoder.cpp
    src/render/D3DRenderer.cpp
    src/input/RawInputCapture.cpp
)

target_include_directories(windows-client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(windows-client PRIVATE
    ws2_32       # WinSock (HP UDP transport)
    d3d11        # D3D11 device/context (renderer + decode surfaces)
    dxgi         # DXGI flip-model swapchain
    mf           # Media Foundation
    mfplat
    mfuuid
    mfreadwrite
    ole32        # CoInitialize
)

# Raw input / video: user32 comes with the default set.
if (MSVC)
    target_compile_options(windows-client PRIVATE /W4 /permissive-)
    target_compile_definitions(windows-client PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
