cmake_minimum_required(VERSION 3.24)
project(windows-client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Console subsystem on purpose: the client prints per-second telemetry
# (spec §45) to the terminal it was started from; the video is its own window.
add_executable(windows-client
    src/main.cpp
    src/session/HPClientSession.cpp
    src/decode/MediaFoundationDecoder.cpp
    src/render/D3DRenderer.cpp
    src/input/InputCapture.cpp
)

# Test stand-in for `mac-host serve --mode hp` (loopback on one Windows PC).
add_executable(fake-host
    tools/fake-host.cpp
)

foreach(target windows-client fake-host)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${target} PRIVATE
        ws2_32          # WinSock (HP UDP transport)
        d3d11           # D3D11 device/context (renderer + decode surfaces)
        dxgi            # DXGI flip-model swapchain
        mfplat          # Media Foundation
        mfuuid
        wmcodecdspuuid  # CLSID_CMSH264DecoderMFT / EncoderMFT
        ole32           # CoInitialize / CoCreateInstance
    )
    if (MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
endforeach()
