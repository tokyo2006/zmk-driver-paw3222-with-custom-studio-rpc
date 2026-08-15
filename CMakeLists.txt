zephyr_include_directories(include)

if(CONFIG_PAW3222 OR CONFIG_ZMK_PAW3222_CUSTOM_SETTINGS)
    target_sources(app PRIVATE src/paw3222_settings_id.c)
endif()

if(CONFIG_PAW3222)
    target_sources(app PRIVATE src/paw3222.c)
endif()

if(CONFIG_ZMK_PAW3222_CUSTOM_SETTINGS)
    target_sources(app PRIVATE src/settings/paw3222_settings.c)
endif()

if(CONFIG_ZMK_PAW3222_PROTOBUF)
    if(NOT CONFIG_PAW3222)
        target_sources(app PRIVATE src/paw3222_api_stub.c)
    endif()

    target_sources(app PRIVATE src/studio/paw3222_request_exec.c)

    list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
    include(nanopb)
    set(NANOPB_GENERATE_CPP_APPEND_PATH TRUE)
    set(NANOPB_GENERATE_CPP_STANDALONE OFF)

    zephyr_library()
    file(GLOB_RECURSE PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/proto/*.proto)

    nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${CMAKE_CURRENT_SOURCE_DIR} ${PROTO_FILES})
    target_include_directories(${ZEPHYR_CURRENT_LIBRARY} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
    target_sources(${ZEPHYR_CURRENT_LIBRARY} PRIVATE ${proto_srcs} ${proto_hdrs})

    target_include_directories(app PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/proto)
    add_dependencies(app ${ZEPHYR_CURRENT_LIBRARY})
endif()

if(CONFIG_ZMK_PAW3222_STUDIO_RPC)
    target_sources(app PRIVATE src/studio/paw3222_handler.c)
endif()
