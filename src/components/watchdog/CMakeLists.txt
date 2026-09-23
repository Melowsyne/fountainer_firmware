# App watchdog: channel-based progress monitoring with multi-stage
# recovery, RTC reboot diagnosis and reboot-loop brake (Watchdog_v1.md).
# The SOLE decision-maker on recovery/reboot; the ESP-IDF TWDT (60 s, PANIC)
# remains beneath it as the hard fallback level.
idf_component_register(
    SRCS "src/app_watchdog.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer event_manager logging
)
