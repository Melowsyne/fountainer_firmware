# CANopen slave core (CiA 301 subset): NMT slave, heartbeat producer, SDO
# server (expedited + segmented), TPDO/RPDO with dynamic mapping, SYNC, EMCY.
# PURE C — no IDF/FreeRTOS dependency; the CAN driver and the object
# dictionary are injected via callbacks. Host-tested in test/host/test_co_core.c.
idf_component_register(
    SRCS "src/co_core.c"
    INCLUDE_DIRS "include"
)
