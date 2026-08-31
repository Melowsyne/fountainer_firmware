# App watchdog: channel-based progress supervision with staged recovery,
# RTC reboot diagnosis and reboot-loop brake (Watchdog_v1.md).
# The ONLY decision maker for recovery/reboot; the ESP-IDF TWDT (60 s, PANIC)
# remains the hard fallback level below it.
idf_component_register(
    SRCS "src/app_watchdog.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer event_manager logging
)
