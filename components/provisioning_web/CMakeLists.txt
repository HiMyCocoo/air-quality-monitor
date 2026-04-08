idf_component_register(SRCS "provisioning_web.c"
                    INCLUDE_DIRS "include"
                    REQUIRES device_state_json ota_manager platform esp_http_server cjson)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    set(index_html_src "${CMAKE_CURRENT_SOURCE_DIR}/index.html")
    set(index_html_gz "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
    set(index_html_gzip_script "${CMAKE_CURRENT_SOURCE_DIR}/gzip_asset.py")

    add_custom_command(
        OUTPUT "${index_html_gz}"
        COMMAND ${Python3_EXECUTABLE} "${index_html_gzip_script}" "${index_html_src}" "${index_html_gz}"
        DEPENDS "${index_html_src}" "${index_html_gzip_script}"
        VERBATIM)

    target_add_binary_data(${COMPONENT_LIB} "${index_html_gz}" BINARY
                        RENAME_TO index_html_gz
                        DEPENDS "${index_html_gz}")
endif()
