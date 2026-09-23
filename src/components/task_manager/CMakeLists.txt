# Task manager: generic lifecycle/periodic layer for cyclic
# application tasks (registration, fixed time grid, cooperative stopping,
# cycle/stack monitoring with reporting via the event_manager).
idf_component_register(
    SRCS "src/task_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer event_manager
)
