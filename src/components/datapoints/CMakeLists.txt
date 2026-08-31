# ESP-IDF component registration for the data-point store.
# Drop this folder into your project's  components/datapoints/  directory.
#
# REQUIRES:
#   nvs_flash  - NVS config persistence
#   json       - cJSON (bundled with ESP-IDF) for value encode/decode
#   esp_system - esp_get_free_heap_size(), etc.
#
# dp_list.def is a header-like include (no compiled TU of its own); it is
# pulled in by datapoints.h / datapoints.c, so it only needs to be on the
# include path, which "." provides.

idf_component_register(
    SRCS        "datapoints.c"
    INCLUDE_DIRS "."
    REQUIRES    nvs_flash json esp_system
)
