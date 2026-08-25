cmake_minimum_required(VERSION 3.16)
project(epub2wgb CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(LIB ${REPO_ROOT}/wintergreen)

# miniz, vendored — same sources the desktop and ESP32 builds use.
set(MINIZ_DIR ${REPO_ROOT}/third_party/miniz)
add_library(miniz STATIC
    ${MINIZ_DIR}/miniz.c
    ${MINIZ_DIR}/miniz_tdef.c
    ${MINIZ_DIR}/miniz_tinfl.c
    ${MINIZ_DIR}/miniz_zip.c)
target_include_directories(miniz PUBLIC ${MINIZ_DIR})

# The content pipeline only. No display, no SDL: WgbConverter includes
# DrawBuffer.h purely for kWidth/kHeight, which are header constants.
add_executable(epub2wgb
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${LIB}/content/HtmlEntities.cpp
    ${LIB}/content/ZipReader.cpp
    ${LIB}/content/XmlReader.cpp
    ${LIB}/content/CssParser.cpp
    ${LIB}/content/EpubParser.cpp
    ${LIB}/content/ImageDecoder.cpp
    ${LIB}/content/JpegDecoder.cpp
    ${LIB}/content/PngDecoder.cpp
    ${LIB}/content/Book.cpp
    ${LIB}/content/TextLayout.cpp
    ${LIB}/content/hyphenation/Hyphenation.cpp
    ${LIB}/content/wgb/WgbWriter.cpp
    ${LIB}/content/wgb/WgbReader.cpp
    ${LIB}/content/wgb/WgbConverter.cpp)

target_include_directories(epub2wgb PRIVATE ${REPO_ROOT})
target_link_libraries(epub2wgb PRIVATE miniz Threads::Threads)
