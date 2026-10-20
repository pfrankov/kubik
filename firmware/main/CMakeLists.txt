set(KUBIK_CHARACTER "TESS" CACHE STRING "Firmware character: TESS or PLUSH")
set_property(CACHE KUBIK_CHARACTER PROPERTY STRINGS TESS PLUSH)
if(KUBIK_CHARACTER STREQUAL "TESS")
    set(CHARACTER_ID 1)
elseif(KUBIK_CHARACTER STREQUAL "PLUSH")
    set(CHARACTER_ID 0)
else()
    message(FATAL_ERROR "KUBIK_CHARACTER must be TESS or PLUSH")
endif()
set(CHARACTER_FILE "${CMAKE_BINARY_DIR}/character.txt")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
if(EXISTS "${CHARACTER_FILE}")
    file(STRINGS "${CHARACTER_FILE}" BUILT_CHARACTER)
    if(NOT BUILT_CHARACTER STREQUAL KUBIK_CHARACTER)
        message(FATAL_ERROR "Use a separate build directory for ${KUBIK_CHARACTER}; this one is ${BUILT_CHARACTER}")
    endif()
else()
    file(WRITE "${CHARACTER_FILE}" "${KUBIK_CHARACTER}\n")
endif()
endif()

set(WAKE_SRCS "")
set(WAKE_REQUIRES espressif__esp-sr)
if(KUBIK_CHARACTER STREQUAL "TESS")
    set(WAKE_SRCS wake_model.cc app_wake.c)
    set(WAKE_REQUIRES espressif__esp-tflite-micro esphome__esp-micro-speech-features espressif__esp-sr)
endif()
idf_component_register(
    SRCS sound_levels.c frame_diagnostics.c status_json.c app_game_probe.c input_probe.c app_status.c app_nvs.c connection_record.c connection_store.c event_journal.c event_journal_draw.c app_journal.c screen_lab.c screen_lab_draw.c screen_lab_fixture.c screen_lab_events.c app_lab.c ${WAKE_SRCS} wake_resample.c auto_voice.c lift.c main.c present.c frame_store.c devkey.c board.c canvas.c render.c render_dots.c render_glass.c render_output.c render_pipeline.c render_raster.c render_scene.c render_text.c face.c face_params.c face_body.c face_card.c face_update.c face_setup.c face_menu.c face_status.c ui_kit.c face_agent.c agent_menu.c agent_protocol.c face_plush.c face_rub.c tess_geometry.c tess_growth.c tess_games.c   tess_gather.c tess_sky.c tess_sky_stream.c tess_sky_compose.c tess_sky_plan.c tess_sky_melody.c tess_sky_touch.c tess_sky_summon.c tess_sky_sound.c tess_sky_draw.c tess_play_shape.c tess_stretch.c tess_temper.c tess_motion.c tess_motion_sound.c tess_rub.c rub.c tess_mood.c mood.c tess_feel.c tess_gaze.c tess_play.c tess_touch.c tess_scatter.c tess_fall.c tess_draw.c sprite.c assets.c audio.c audio_board.c audio_output_profile.c audio_levels.c audio_signal.c audio_mic_io.c mic_capture.c mic_capture_task.c mic_delivery.c ima_adpcm.c audio_sfx.c tess_sound.c tess_sound_compose.c tess_sound_body.c tess_sound_motion.c tess_sound_sky.c tess_sound_rules.c heap_probe.c link.c link_mic.c link_tcp.c link_tls.c link_usb.c link_ws.c link_poke.c link_pin.c link_discover.c link_hello.c tls_mem.c wifi.c setup.c setup_dns.c qr.c font.c font_data.c ui_text.c settings.c input.c input_touch.c input_rub.c viewpoint.c app.c app_protocol.c app_models.c app_menu.c app_guide.c app_control.c app_voice.c app_events.c app_runtime.c app_state.c app_mailbox.c app_speech.c power_network.c
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${WAKE_REQUIRES} esp_partition esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_i2s esp_lcd esp_timer esp_pm
                  nvs_flash esp_wifi esp_netif esp_event esp_driver_usb_serial_jtag app_update
                  esp_http_server esp_http_client json mbedtls lwip esp_system bt muse_pairing noise_core muse_chat
)
target_sources(${COMPONENT_LIB} PRIVATE muse_store.c muse_ble.c muse_pair.c muse_vm.c
    muse_noise.cc muse_backend.c muse_options.c muse_control.c muse_json.c setup_request.c)
# Only the CPU FFT workspace leases LP SRAM; keep the SDK algorithm unchanged.
# Source properties must be set in the component's directory, not main's.
if(KUBIK_CHARACTER STREQUAL "TESS")
    idf_component_get_property(features_lib esphome__esp-micro-speech-features COMPONENT_LIB)
    idf_component_get_property(features_dir esphome__esp-micro-speech-features COMPONENT_DIR)
    set_property(SOURCE "${features_dir}/src/fft_util.c" TARGET_DIRECTORY ${features_lib}
        APPEND PROPERTY COMPILE_DEFINITIONS malloc=kubik_wake_fft_alloc)
endif()
# Use the documented project hook; never mutate the global ESP-IDF checkout.
idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
idf_component_get_property(lwip_dir lwip COMPONENT_DIR)
set_property(SOURCE "${lwip_dir}/lwip/src/core/tcp_out.c" TARGET_DIRECTORY ${lwip_lib}
    APPEND PROPERTY COMPILE_DEFINITIONS KUBIK_TCP_OUT_ALLOCATIONS=1)
target_include_directories(${lwip_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(${lwip_lib} PRIVATE ESP_IDF_LWIP_HOOK_FILENAME="lwip_hooks.h")
# The SDK exposes socket/error foundation hooks in this pinned private header.
idf_component_get_property(tcp_dir tcp_transport COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${tcp_dir}/private_include")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    execute_process(COMMAND python3 "${PROJECT_DIR}/../tools/patch-websocket.py"
        "${PROJECT_DIR}/managed_components/espressif__esp_websocket_client"
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND python3 "${PROJECT_DIR}/../tools/patch-cst9217.py"
        "${PROJECT_DIR}/managed_components/waveshare__esp_lcd_touch_cst9217"
        COMMAND_ERROR_IS_FATAL ANY)
    idf_component_get_property(touch_lib waveshare__esp_lcd_touch_cst9217 COMPONENT_LIB)
    target_include_directories(${touch_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
endif()
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_definitions(${COMPONENT_LIB} PRIVATE KUBIK_CHARACTER=${CHARACTER_ID})

target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_transport_ws_send_raw,--wrap=esp_transport_write,--undefined=__wrap_esp_transport_ws_send_raw")
# Keep readable sources; store only gzip bytes and serve them directly to browsers.
idf_build_get_property(python PYTHON)
foreach(asset portal.html portal.css portal_tess.js)
    set(packed "${CMAKE_CURRENT_BINARY_DIR}/${asset}.gz")
    add_custom_command(OUTPUT "${packed}"
        COMMAND "${python}" "${PROJECT_DIR}/../tools/assets/compress-web.py"
                "${CMAKE_CURRENT_LIST_DIR}/${asset}" "${packed}"
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/${asset}" "${PROJECT_DIR}/../tools/assets/compress-web.py"
        VERBATIM)
    target_add_binary_data(${COMPONENT_LIB} "${packed}" BINARY RENAME_TO "${asset}")
endforeach()
# qrcodegen.h (portable encoder) sits in the component root, not its include dir.
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__qrcode)
# Keep the measured rendering optimization; compile the SDK for size.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-format-truncation -Wno-stringop-truncation)
# Tess's point loops dominate interactive frames on the software-float C6.
# Let the compiler inline/unroll them without fast-math or changed arithmetic.
if(KUBIK_CHARACTER STREQUAL "TESS")
    set_source_files_properties(tess_geometry.c tess_play_shape.c tess_draw.c tess_scatter.c render_dots.c
        tess_sky_stream.c tess_sky_compose.c tess_sky_draw.c
        PROPERTIES COMPILE_OPTIONS "-O3")
endif()
# Configuration and diagnostic handlers are cold paths; preserve flash headroom.
set_source_files_properties(sound_levels.c connection_store.c connection_record.c link_discover.c link_hello.c
    app_nvs.c devkey.c link_pin.c link.c wifi.c setup.c setup_request.c app_control.c app_state.c app_events.c app_runtime.c
    muse_store.c muse_ble.c muse_pair.c muse_vm.c muse_backend.c muse_options.c muse_control.c muse_json.c
    status_json.c frame_diagnostics.c setup_dns.c app_speech.c link_usb.c app_lab.c
    agent_menu.c agent_protocol.c settings.c screen_lab_fixture.c screen_lab_events.c screen_lab.c
    link_tls.c app_protocol.c app_models.c app_menu.c app_guide.c app_status.c
    PROPERTIES COMPILE_OPTIONS "-Os")

# Plush has a smaller app partition; diagnostics/event storage are not raster work.
if(KUBIK_CHARACTER STREQUAL "PLUSH")
    set_source_files_properties(app.c app_game_probe.c app_journal.c event_journal.c face_setup.c face_status.c face_agent.c face_menu.c
        screen_lab_draw.c event_journal_draw.c
        PROPERTIES COMPILE_OPTIONS "-Os")
endif()

# The assets image (animation, sounds, voice prompts) is packed from firmware/assets
# and written by `idf.py flash` into the `assets` partition.
set(ASSETS_BIN ${CMAKE_BINARY_DIR}/assets.bin)
file(GLOB_RECURSE ASSET_SRCS CONFIGURE_DEPENDS ${PROJECT_DIR}/assets/*)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/asset-inputs.txt" CONTENT "${ASSET_SRCS}\n")
add_custom_command(OUTPUT ${ASSETS_BIN}
    COMMAND python3 ${PROJECT_DIR}/../tools/assets/pack.py ${ASSETS_BIN} --character ${KUBIK_CHARACTER}
    DEPENDS ${ASSET_SRCS} ${PROJECT_DIR}/../tools/assets/pack.py ${PROJECT_DIR}/../tools/firmware_layout.py
        ${CHARACTER_FILE} "${CMAKE_BINARY_DIR}/asset-inputs.txt"
    COMMENT "Packing assets")
add_custom_target(assets_image ALL DEPENDS ${ASSETS_BIN})
esptool_py_flash_to_partition(flash "assets" ${ASSETS_BIN})

# Debug builds only: heap checkpoints with the lowest point between them (heap_probe.c).
if(KUBIK_HEAP_PROBE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KUBIK_HEAP_PROBE=1)
endif()
