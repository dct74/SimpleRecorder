cmake_minimum_required(VERSION 3.15)
project(SimpleRecorder VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(SimpleRecorder WIN32
    src/main.cpp
    src/AudioCaptureEngine.cpp
    src/MfAudio.cpp
    src/MediaPlayer.cpp
    src/WavWriter.cpp
    app.rc
    app.manifest
)

target_include_directories(SimpleRecorder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(SimpleRecorder PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Everything comes from the operating system: WASAPI for capture,
# Media Foundation for AAC/MPEG-4 encoding and playback.
target_link_libraries(SimpleRecorder PRIVATE
    Avrt.lib
    Comctl32.lib
    Mf.lib
    Mfplat.lib
    Mfplay.lib
    Mfreadwrite.lib
    Mfuuid.lib
    Mmdevapi.lib
    Ole32.lib
    OleAut32.lib
    Propsys.lib
    Shell32.lib
    Shlwapi.lib
    Uuid.lib
)

set_target_properties(SimpleRecorder PROPERTIES
    WIN32_EXECUTABLE TRUE
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

if(MSVC)
    target_compile_options(SimpleRecorder PRIVATE /W4 /utf-8)
else()
    target_compile_options(SimpleRecorder PRIVATE -Wall -Wextra)
endif()
