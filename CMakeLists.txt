cmake_minimum_required(VERSION 3.16)
project(tinyphaser C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(USE_GLES "Use OpenGL ES 2.0 instead of desktop GL" OFF)

# Find dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSC REQUIRED javascriptcoregtk-4.1)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(SDL2_MIXER REQUIRED SDL2_mixer)
pkg_check_modules(SDL2_TTF REQUIRED SDL2_ttf)
pkg_check_modules(SOUP REQUIRED libsoup-3.0)
pkg_check_modules(RSVG REQUIRED librsvg-2.0)

if(USE_GLES)
    pkg_check_modules(GL REQUIRED glesv2 egl)
    add_definitions(-DUSE_GLES2)
else()
    pkg_check_modules(GL REQUIRED gl)
    add_definitions(-DGL_GLEXT_PROTOTYPES)
endif()

# litehtml (pre-built static library)
set(LITEHTML_DIR ${CMAKE_SOURCE_DIR}/third_party/litehtml)
set(LITEHTML_LIB ${LITEHTML_DIR}/build/liblitehtml.a)
set(GUMBO_LIB ${LITEHTML_DIR}/build/src/gumbo/libgumbo.a)

# libdatachannel (pre-built static library)
set(DATACHANNEL_DIR ${CMAKE_SOURCE_DIR}/third_party/libdatachannel)
set(DATACHANNEL_LIB ${DATACHANNEL_DIR}/build/libdatachannel.a)
set(JUICE_LIB ${DATACHANNEL_DIR}/build/deps/libjuice/libjuice.a)
set(USRSCTP_LIB ${DATACHANNEL_DIR}/build/deps/usrsctp/usrsctplib/libusrsctp.a)

# Source files
set(SOURCES
    src/main.c
    src/engine.c
    src/shims/console.c
    src/shims/timers.c
    src/shims/performance.c
    src/shims/window.c
    src/shims/document.c
    src/shims/canvas.c
    src/shims/webgl.c
    src/shims/image.c
    src/shims/xhr.c
    src/shims/events.c
    src/shims/audio.c
    src/shims/text.c
    src/shims/websocket.c
    src/shims/webrtc.c
    src/shims/fetch_net.c
    src/shims/dom_bridge.cpp
    src/util/file_utils.c
)

add_executable(tinyphaser ${SOURCES})

target_include_directories(tinyphaser PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
    ${LITEHTML_DIR}/include
    ${DATACHANNEL_DIR}/include
    ${JSC_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${SDL2_MIXER_INCLUDE_DIRS}
    ${SDL2_TTF_INCLUDE_DIRS}
    ${GL_INCLUDE_DIRS}
    ${SOUP_INCLUDE_DIRS}
    ${RSVG_INCLUDE_DIRS}
)

target_link_libraries(tinyphaser
    ${LITEHTML_LIB}
    ${GUMBO_LIB}
    ${DATACHANNEL_LIB}
    ${JUICE_LIB}
    ${USRSCTP_LIB}
    ${JSC_LIBRARIES}
    ${SDL2_LIBRARIES}
    ${SDL2_MIXER_LIBRARIES}
    ${SDL2_TTF_LIBRARIES}
    ${GL_LIBRARIES}
    ${SOUP_LIBRARIES}
    ${RSVG_LIBRARIES}
    ssl crypto
    stdc++
    m
)

target_compile_options(tinyphaser PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(tinyphaser PRIVATE RTC_STATIC)

# Copy runtime files to build directory on every build
add_custom_command(TARGET tinyphaser POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_BINARY_DIR}/runtime
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/test ${CMAKE_BINARY_DIR}/test
    COMMENT "Syncing runtime/ and test/ to build directory"
)
