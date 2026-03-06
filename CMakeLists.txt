cmake_minimum_required(VERSION 3.16)
project(phaserquest C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(USE_GLES "Use OpenGL ES 2.0 instead of desktop GL" OFF)

# Find dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSC REQUIRED javascriptcoregtk-4.1)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(SDL2_MIXER REQUIRED SDL2_mixer)
pkg_check_modules(SDL2_TTF REQUIRED SDL2_ttf)

if(USE_GLES)
    pkg_check_modules(GL REQUIRED glesv2 egl)
    add_definitions(-DUSE_GLES2)
else()
    pkg_check_modules(GL REQUIRED gl)
    add_definitions(-DGL_GLEXT_PROTOTYPES)
endif()

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
    src/util/file_utils.c
)

add_executable(phaserquest ${SOURCES})

target_include_directories(phaserquest PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
    ${JSC_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${SDL2_MIXER_INCLUDE_DIRS}
    ${SDL2_TTF_INCLUDE_DIRS}
    ${GL_INCLUDE_DIRS}
)

target_link_libraries(phaserquest
    ${JSC_LIBRARIES}
    ${SDL2_LIBRARIES}
    ${SDL2_MIXER_LIBRARIES}
    ${SDL2_TTF_LIBRARIES}
    ${GL_LIBRARIES}
    m
)

target_compile_options(phaserquest PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Copy runtime files to build directory on every build
add_custom_command(TARGET phaserquest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_BINARY_DIR}/runtime
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/test ${CMAKE_BINARY_DIR}/test
    COMMENT "Syncing runtime/ and test/ to build directory"
)
