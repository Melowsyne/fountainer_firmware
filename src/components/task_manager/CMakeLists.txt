# Task manager: generic lifecycle/periodics layer for cyclic application
# tasks (registration, fixed time grid, cooperative stopping, cycle/stack
# supervision with reporting via the event_manager).
idf_component_register(
    SRCS "src/task_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer event_manager
)
