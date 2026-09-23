# ESP-IDF component: clientside_protocol (Fountain v2.2 protocol client)
idf_component_register(
    SRCS
        "src/fountain_msgs.c"   # generated from fountain_proto_schema/schema.py
        "src/fp_auth.c"
        "src/fp_envelope.c"
        "src/fp_session.c"
        "src/fp_ws.c"
        "src/fp_task.c"
    INCLUDE_DIRS "include"
    REQUIRES json mbedtls esp_websocket_client esp_event esp_timer
)
