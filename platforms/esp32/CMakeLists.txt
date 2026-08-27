FILE(GLOB app_sources
    ${CMAKE_CURRENT_LIST_DIR}/*.c
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp)

set(LIB ${CMAKE_CURRENT_LIST_DIR}/../../wintergreen)

set(lib_sources
    ${LIB}/Application.cpp
    ${LIB}/screens/ListMenuScreen.cpp
    ${LIB}/screens/MainMenu.cpp
    ${LIB}/screens/ReaderScreen.cpp
    ${LIB}/screens/QuickmenuScreen.cpp
    ${LIB}/screens/HomeScreen.cpp
    ${LIB}/content/CoverPaths.cpp
    ${LIB}/content/TextLayout.cpp
    ${LIB}/content/hyphenation/Hyphenation.cpp
    ${LIB}/content/BookIndex.cpp
    ${LIB}/content/wgb/WgbReader.cpp)


idf_component_register(
    SRCS ${app_sources} ${lib_sources}
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../.."
    REQUIRES fatfs driver wear_levelling
             esp_wifi esp_netif esp_event esp_http_client lwip nvs_flash)

# IDF 5.x adds -Wno-old-style-declaration (C-only) globally; GCC -Werror rejects
# it when compiling C++ files. Remove both flags from the entire IDF build so that:
#   - C++ files: no longer get an invalid-for-C++ flag error
#   - C files with old-style declarations (e.g. esp_ota_ops.c): no longer error out
idf_build_get_property(compile_opts COMPILE_OPTIONS)
list(REMOVE_ITEM compile_opts "-Wno-old-style-declaration" "-Werror=old-style-declaration")
idf_build_set_property(COMPILE_OPTIONS "${compile_opts}")



