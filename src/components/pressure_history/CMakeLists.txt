# Pressure history: 1 Hz measurement ring (drucksensor_datenstruktur.md) —
# gapless recording with server catch-up via history_read/
# history_batch (clientside_protocol + task_com). pressure_ring.c is pure
# (host-tested in test/host/test_pressure_ring.c).
idf_component_register(
    SRCS "src/pressure_ring.c" "src/pressure_history.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer
)
