cmake_minimum_required(VERSION 3.16)
project(3dgungame CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(SOURCES
    src/main.cpp
    src/Client.cpp
    src/Server.cpp
    src/Map.cpp
    src/Player.cpp
)

add_executable(3dgungame ${SOURCES})
target_include_directories(3dgungame PRIVATE src)

# ---------------------------------------------------------------------------
# raylib
# ---------------------------------------------------------------------------
find_package(raylib 4.0 REQUIRED)
target_link_libraries(3dgungame PRIVATE raylib)

# ---------------------------------------------------------------------------
# ENet
# ---------------------------------------------------------------------------
find_path(ENET_INCLUDE_DIR enet/enet.h)
find_library(ENET_LIB NAMES enet)

if(NOT ENET_INCLUDE_DIR OR NOT ENET_LIB)
    message(FATAL_ERROR
        "ENet not found.  Install it with:\n"
        "  Arch Linux : sudo pacman -S enet\n"
        "  Ubuntu/Deb : sudo apt install libenet-dev\n"
        "  Windows    : vcpkg install enet:x64-windows")
endif()

target_include_directories(3dgungame PRIVATE ${ENET_INCLUDE_DIR})
target_link_libraries(3dgungame PRIVATE ${ENET_LIB})

# ---------------------------------------------------------------------------
# Platform-specific system libraries
# ---------------------------------------------------------------------------
if(WIN32)
    target_link_libraries(3dgungame PRIVATE ws2_32 winmm)

elseif(UNIX AND NOT APPLE)
    target_link_libraries(3dgungame PRIVATE pthread dl m)

    # Raylib's CMake config normally exports OpenGL/X11 transitively, so on
    # many systems nothing below is needed.  We add fallbacks for devices like
    # the Steam Deck where GL include dirs are absent but libGL is still present.
    find_package(OpenGL QUIET)
    if(OpenGL_FOUND)
        target_link_libraries(3dgungame PRIVATE OpenGL::GL)
    else()
        target_link_libraries(3dgungame PRIVATE GL)
    endif()

    find_package(X11 QUIET)
    if(X11_FOUND)
        target_link_libraries(3dgungame PRIVATE X11)
        foreach(_xlib Xrandr Xi Xcursor Xinerama)
            if(X11_${_xlib}_LIB)
                target_link_libraries(3dgungame PRIVATE ${X11_${_xlib}_LIB})
            endif()
        endforeach()
    endif()
endif()
