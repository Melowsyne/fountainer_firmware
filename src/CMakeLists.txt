# Main component: Fountain controller, modular structure (Modularisation.md):
#   main/    key module — app_main, task start + supervision (monitor)
#   core/    cross-cutting — debug, data_store, power_mgmt, system (no
#            upward dependency; decoupling via injected hooks)
#   network/ WLAN, protocol binding, OTA, provisioning (network.json, certs)
#   device/  replaceable device layer — HAL, sensors, pump logic, commands
# Hierarchy: main -> {network, device} -> core -> (ESP-IDF/components).
# src/components/ contains STANDALONE IDF components (see the project
# CMakeLists EXTRA_COMPONENT_DIRS) — exclude them from the main component's
# glob, otherwise their sources would be compiled/linked twice.
FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)
list(FILTER app_sources EXCLUDE REGEX "/src/components/")

# network.json (network provisioning, see network_config.c) is embedded via
# platformio.ini (board_build.embed_txtfiles) — IDF's own EMBED_TXTFILES
# collides with the PlatformIO build wrapper. The fallback generation from
# network.json.example is handled by tools/ensure_network_json.py.

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "." "main" "core" "network" "network/local_server" "device"
    REQUIRES
        driver
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        nvs_flash
        json
        factory_config
        datapoints
        event_manager
        task_manager
        logging
        pressure_history
        watchdog
        clientside_protocol
        esp_websocket_client
        esp_http_client
        esp_http_server
        esp_https_server
        esp_https_ota
        app_update
        esp_partition
        mbedtls
)
