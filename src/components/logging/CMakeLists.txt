# Logging: structured, switchable diagnostics records (RAM ring; the flash tier
# follows with the logstore partition, see Logging_v1.md). Server pull via the
# protocol messages log_read/log_batch (clientside_protocol + task_com).
idf_component_register(
    SRCS "src/log_ring.c" "src/logging_core.c" "src/logging_bridge.c" "src/logging_flash.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer event_manager esp_partition
)
