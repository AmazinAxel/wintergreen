cmake_minimum_required(VERSION 3.16)

# Use /Z7 (embedded debug info per .obj) instead of shared /Zi .pdb to avoid
# C1041 PDB lock errors from parallel MSVC compilation.
# CMP0141 must be set before project() to take effect.
if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")
endif()

project(wintergreen_desktop C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# ---------------------------------------------------------------------------
# SDL2 – fetched at configure time; static build, no tests, no shared lib.
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-2.30.10
    GIT_SHALLOW    TRUE
)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL2)

# miniz (vendored in third_party/miniz/ — all .c files compiled as separate TUs)
set(MINIZ_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/miniz)
add_library(miniz STATIC
    ${MINIZ_DIR}/miniz.c
    ${MINIZ_DIR}/miniz_tdef.c
    ${MINIZ_DIR}/miniz_tinfl.c
    ${MINIZ_DIR}/miniz_zip.c
)
target_include_directories(miniz PUBLIC ${MINIZ_DIR})

# ---------------------------------------------------------------------------
# Content library (EPUB, ZIP, XML, layout)
# ---------------------------------------------------------------------------
set(CONTENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/wintergreen/content)

add_library(wintergreen_content STATIC
    ${CONTENT_DIR}/HtmlEntities.cpp
    ${CONTENT_DIR}/ZipReader.cpp
    ${CONTENT_DIR}/XmlReader.cpp
    ${CONTENT_DIR}/CssParser.cpp
    ${CONTENT_DIR}/EpubParser.cpp
    ${CONTENT_DIR}/ImageDecoder.cpp
    ${CONTENT_DIR}/JpegDecoder.cpp
    ${CONTENT_DIR}/PngDecoder.cpp
    ${CONTENT_DIR}/Book.cpp
    ${CONTENT_DIR}/BookIndex.cpp
    ${CONTENT_DIR}/TextLayout.cpp
    ${CONTENT_DIR}/hyphenation/Hyphenation.cpp
    ${CONTENT_DIR}/mrb/MrbWriter.cpp
    ${CONTENT_DIR}/mrb/MrbReader.cpp
    ${CONTENT_DIR}/mrb/MrbConverter.cpp
)
target_include_directories(wintergreen_content PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)
target_link_libraries(wintergreen_content PUBLIC miniz)

# ---------------------------------------------------------------------------
# Platform-agnostic core library
# ---------------------------------------------------------------------------
add_library(wintergreen_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/wintergreen/Application.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/wintergreen/Loop.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/wintergreen/screens/ListMenuScreen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/wintergreen/screens/MainMenu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/wintergreen/screens/ReaderScreen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/wintergreen/screens/ReaderOptionsScreen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/wintergreen/screens/HiddenBooksMenu.cpp
)
target_include_directories(wintergreen_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)
target_link_libraries(wintergreen_core PUBLIC wintergreen_content)

# ---------------------------------------------------------------------------
# Desktop executable
# ---------------------------------------------------------------------------
add_executable(wintergreen_desktop
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
)
target_link_libraries(wintergreen_desktop PRIVATE
    wintergreen_core
    SDL2::SDL2-static
)
# Statically link MinGW runtime so the exe runs without libstdc++/libgcc DLLs.
if(MINGW)
  target_link_options(wintergreen_desktop PRIVATE -static-libgcc -static-libstdc++)
  target_link_libraries(wintergreen_desktop PRIVATE -Wl,-Bstatic winpthread -Wl,-Bdynamic)
endif()
# Prevent SDL_main.h from renaming main() so we keep a plain console entry point.
target_compile_definitions(wintergreen_desktop PRIVATE SDL_MAIN_HANDLED)
# Generate a header with the repo-root sd/ path (avoids MSVC /D drive-letter colon issue).
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(WINTERGREEN_SD_DIR "${REPO_ROOT}/sd")
# REPO_ROOT is also used to locate resources/ for the reader font.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/desktop_config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/desktop_config.h
    @ONLY
)
target_include_directories(wintergreen_desktop PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
