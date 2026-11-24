cmake_minimum_required(VERSION 3.21)
project(termica VERSION 0.3.0 LANGUAGES C)

# The font is embedded as Windows RCDATA resources.
if(WIN32)
    enable_language(RC)
endif()

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

# ---- tomlc17: the TOML settings parser --------------------------------------
# Upstream ships an amalgamated src/tomlc17.c + tomlc17.h and a Makefile, but
# no CMakeLists.txt, so the library target is declared here.
# Offline? Point this at a local checkout:
#   -DFETCHCONTENT_SOURCE_DIR_TOMLC17=C:/src/tomlc17
FetchContent_Declare(tomlc17
    GIT_REPOSITORY https://github.com/cktan/tomlc17.git
    GIT_TAG        R261116
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(tomlc17)

add_library(tomlc17 STATIC ${tomlc17_SOURCE_DIR}/src/tomlc17.c)
target_include_directories(tomlc17 PUBLIC ${tomlc17_SOURCE_DIR}/src)
# termica's own code stays C99; the library is written for C17 and compiled
# as such on its own target.
set_target_properties(tomlc17 PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED ON)
if(NOT WIN32)
    target_link_libraries(tomlc17 PUBLIC m)
endif()

# ---- PCRE2: Unicode-aware regular expressions for scrollback search ----------
set(PCRE2_BUILD_PCRE2_8 ON CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2_16 OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2_32 OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PCRE2_SUPPORT_UNICODE ON CACHE BOOL "" FORCE)
FetchContent_Declare(pcre2
    GIT_REPOSITORY https://github.com/PCRE2Project/pcre2.git
    GIT_TAG pcre2-10.46
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(pcre2)

# ---- libghostty-vt -----------------------------------------------------------
# Docs: https://lib.ghostty.org/vt/installing.html
#
# On Windows there is an official prebuilt zip, so no Zig is needed. Everywhere
# else the library is built from source (that does need Zig 0.17) and found
# through CMAKE_PREFIX_PATH / ghostty-vt_DIR.
set(GHOSTTY_VERSION "1.0.1" CACHE STRING "libghostty-vt release version")
if(WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
    set(GHOSTTY_ARCH "aarch64" CACHE STRING "libghostty-vt Windows arch")
else()
    set(GHOSTTY_ARCH "x86_64" CACHE STRING "libghostty-vt Windows arch")
endif()

if(WIN32)
    set(ghostty_zip "${CMAKE_BINARY_DIR}/ghostty-vt-${GHOSTTY_VERSION}-${GHOSTTY_ARCH}-windows-gnu.zip")
    set(ghostty_root "${CMAKE_BINARY_DIR}/ghostty-vt-${GHOSTTY_VERSION}-${GHOSTTY_ARCH}-windows-gnu/ghostty-vt-${GHOSTTY_VERSION}-${GHOSTTY_ARCH}-windows-gnu")

    # The checksum of the release we default to. A pinned GHOSTTY_VERSION
    # that we have no checksum for is downloaded unverified (and CMake says
    # so), so bumping the version does not mean editing this file first.
    set(ghostty_hash_args)
    if(GHOSTTY_VERSION STREQUAL "1.0.1")
        set(ghostty_hash_args EXPECTED_HASH SHA256=8e2eb9e1822e85d786b456c8beb86f49d09c318c91f0c2913f91e7ffa12cda29)
    endif()

    if(NOT EXISTS "${ghostty_root}/lib/cmake/ghostty-vt")
        file(DOWNLOAD
            https://release.files.ghostty.org/libghostty-vt/${GHOSTTY_VERSION}/ghostty-vt-${GHOSTTY_VERSION}-${GHOSTTY_ARCH}-windows-gnu.zip
            ${ghostty_zip}
            ${ghostty_hash_args}
            STATUS ghostty_dl_status SHOW_PROGRESS)
        list(GET ghostty_dl_status 0 ghostty_dl_err)
        if(NOT ghostty_dl_err EQUAL 0)
            message(FATAL_ERROR "Failed to download libghostty-vt: ${ghostty_dl_status}")
        endif()
        file(ARCHIVE_EXTRACT INPUT ${ghostty_zip} DESTINATION ${ghostty_root})
    endif()
    list(APPEND CMAKE_PREFIX_PATH ${ghostty_root})
    find_package(ghostty-vt CONFIG REQUIRED)
else()
    find_package(ghostty-vt CONFIG QUIET)
    if(NOT TARGET ghostty-vt::ghostty-vt)
        message(FATAL_ERROR
            "libghostty-vt not found. This milestone needs it; only Windows has a "
            "prebuilt release. Build it from source (needs Zig 0.17):\n"
            "  zig build -Doptimize=ReleaseFast --prefix <dir>\n"
            "then configure with -DCMAKE_PREFIX_PATH=<dir>.\n"
            "See https://lib.ghostty.org/vt/installing.html")
    endif()
endif()

# ---- The demo program --------------------------------------------------------
add_executable(termica
    src/main.c
    src/session.c
    src/input.c
    src/render.c
    src/chrome.c
    src/colors.c
    src/keys.c
    src/term.c
    src/pty.c
    src/embed.c
    src/console.c
    src/settings.c
    src/statusbar.c
    src/font_lookup.c
    src/selftest.c
    src/clipboard.c
    src/config_watch.c
    src/config_test.c
    src/desktop_signal.c
    src/desktop_signal_test.c
    src/diagnostics.c
    src/diagnostics_test.c
    src/hyperlink.c
    src/hyperlink_test.c
    src/search.c
    src/search_test.c
    src/toast.c
    src/ui_settings.c
    src/glyph.c
    src/glyph_atlas_test.c
    src/git.c
    src/style.c
    src/panel.c
    src/geometry.c
    src/window.c
    src/app.h
    src/session.h
    src/input.h
    src/render.h
    src/chrome.h
    src/colors.h
    src/keys.h
    src/term.h
    src/pty.h
    src/embed.h
    src/console.h
    src/settings.h
    src/statusbar.h
    src/font_lookup.h
    src/clipboard.h
    src/config_watch.h
    src/desktop_signal.h
    src/diagnostics.h
    src/hyperlink.h
    src/search.h
    src/toast.h
    src/ui_settings.h
    src/glyph.h
    src/git.h
    src/style.h
    src/panel.h
    src/geometry.h
    src/window.h)

if(WIN32)
    # A GUI-subsystem executable: starting termica from Explorer or a
    # shortcut no longer flashes a console window first. src/console.c
    # reclaims the launching console so the diagnostics still show up when
    # it is started from a shell.
    set_target_properties(termica PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# Maple Mono NF, built into the executable. Configured rather
# than checked in so the .rc always points at this checkout.
if(WIN32)
    set(TERMICA_FONT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts")
    configure_file(src/fonts.rc.in
                   "${CMAKE_CURRENT_BINARY_DIR}/termica_fonts.rc" @ONLY)
    target_sources(termica PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/termica_fonts.rc")
endif()
target_link_libraries(termica PRIVATE SDL3_ttf::SDL3_ttf SDL3::SDL3)
target_link_libraries(termica PRIVATE ghostty-vt::ghostty-vt)
target_link_libraries(termica PRIVATE tomlc17)
target_link_libraries(termica PRIVATE pcre2-8-static)
target_compile_definitions(termica PRIVATE
    TERMICA_VERSION="${PROJECT_VERSION}-dev" TERMICA_GENERATED_REVISION=1)
target_include_directories(termica PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
add_custom_target(termica_build_revision
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/termica_build_revision.h"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildRevision.cmake"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/termica_build_revision.h"
    VERBATIM)
add_dependencies(termica termica_build_revision)

if(WIN32)
    # ghostty-vt.dll must sit next to the exe (or on PATH).
    add_custom_command(TARGET termica POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:ghostty-vt::ghostty-vt> $<TARGET_FILE_DIR:termica>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:ghostty-vt::ghostty-vt>/ghostty-vt.pdb" $<TARGET_FILE_DIR:termica>
        VERBATIM)
endif()

if(MSVC)
    target_compile_options(termica PRIVATE /W4)
else()
    target_compile_options(termica PRIVATE -Wall -Wextra -pedantic)
endif()

# ---- Sanitizers --------------------------------------------------------------
# -DTERMICA_SANITIZE=ON builds with AddressSanitizer and
# UndefinedBehaviorSanitizer: buffer overflows, use after free, double frees,
# misaligned access and other undefined behaviour are reported at run time.
# LeakSanitizer does not exist for the windows-gnu target, so leaks are not
# reported; the self test and the app free everything they own.
option(TERMICA_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(TERMICA_SANITIZE)
    target_compile_options(termica PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_compile_options(tomlc17 PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(termica PRIVATE -fsanitize=address,undefined)
endif()

if(WIN32)
    target_link_libraries(termica PRIVATE dwmapi shell32 ole32 runtimeobject dbghelp)
endif()
