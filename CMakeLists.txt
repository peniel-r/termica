cmake_minimum_required(VERSION 3.21)
project(termica LANGUAGES C)

# Your own code is strict C99.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(FetchContent)

# ---- SDL3 + SDL3_ttf, built from source and linked statically ---------------
# Offline? Point these at local checkouts instead of downloading:
#   -DFETCHCONTENT_SOURCE_DIR_SDL3=C:/src/SDL
#   -DFETCHCONTENT_SOURCE_DIR_SDL3_TTF=C:/src/SDL_ttf
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)  # SDL3_ttf keys off this
set(SDL_STATIC       ON  CACHE BOOL "" FORCE)
set(SDL_SHARED       OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS        OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL      OFF CACHE BOOL "" FORCE)

set(SDLTTF_VENDORED  ON  CACHE BOOL "" FORCE)  # bundled FreeType + HarfBuzz
set(SDLTTF_PLUTOSVG  OFF CACHE BOOL "" FORCE)  # not needed, saves build time
set(SDLTTF_SAMPLES   OFF CACHE BOOL "" FORCE)
set(SDLTTF_INSTALL   OFF CACHE BOOL "" FORCE)

FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.20
    GIT_SHALLOW    TRUE)

FetchContent_Declare(SDL3_ttf
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
    GIT_TAG        release-3.2.2
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES_RECURSE TRUE)

FetchContent_MakeAvailable(SDL3)
FetchContent_MakeAvailable(SDL3_ttf)

# ---- The demo program --------------------------------------------------------
add_executable(termica src/main.c)
target_link_libraries(termica PRIVATE SDL3_ttf::SDL3_ttf SDL3::SDL3)

if(MSVC)
    target_compile_options(termica PRIVATE /W4)
else()
    target_compile_options(termica PRIVATE -Wall -Wextra -pedantic)
endif()

if(WIN32)
    target_link_libraries(termica PRIVATE dwmapi)
endif()
