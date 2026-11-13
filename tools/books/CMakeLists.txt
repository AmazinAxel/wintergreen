cmake_minimum_required(VERSION 3.16)
project(books CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(LIB ${REPO_ROOT}/wintergreen)

# miniz, vendored: unzip for review, zip for the reviewed EPUB.
set(MINIZ_DIR ${REPO_ROOT}/third_party/miniz)
add_library(miniz STATIC
    ${MINIZ_DIR}/miniz.c
    ${MINIZ_DIR}/miniz_tdef.c
    ${MINIZ_DIR}/miniz_tinfl.c
    ${MINIZ_DIR}/miniz_zip.c)
target_include_directories(miniz PUBLIC ${MINIZ_DIR})

# Review + bake, without the terminal front end. The converter is the content
# pipeline only: WgbConverter includes DrawBuffer.h purely for constants.
add_library(books_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/dom.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/labels.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/review.cpp
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
    ${LIB}/content/wgb/WgbParagraphs.cpp
    ${LIB}/content/wgb/WgbConverter.cpp
    ${LIB}/content/wgb/WgbPaginator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/reader_font.cpp)
# Pages are baked with the device's own font: embed it, and rebuild when it changes.
set(READER_FONT ${REPO_ROOT}/resources/Libron.bin)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/reader_font.cpp PROPERTIES
    COMPILE_DEFINITIONS "WG_READER_FONT=\"${READER_FONT}\""
    OBJECT_DEPENDS ${READER_FONT})
target_include_directories(books_core PUBLIC ${REPO_ROOT} ${REPO_ROOT}/wintergreen ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(books_core PUBLIC miniz)

add_executable(books ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)
target_link_libraries(books PRIVATE books_core)
install(TARGETS books)

# Test driver for tools/tests/run.sh: bakes EPUBs with the proposed chapters,
# no prompts, no upload. Not installed.
add_executable(convert_epubs EXCLUDE_FROM_ALL ${REPO_ROOT}/tools/tests/convert_epubs.cpp)
target_link_libraries(convert_epubs PRIVATE books_core)
