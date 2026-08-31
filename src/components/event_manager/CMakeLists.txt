# Event manager: central, decoupling distribution layer for system events
# (publish/subscribe via a FreeRTOS queue + dispatch task).
idf_component_register(
    SRCS "src/event_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer
)
