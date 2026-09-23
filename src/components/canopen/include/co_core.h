/* Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved. */
#ifndef CO_CORE_H
#define CO_CORE_H
/* ============================================================================
 * co_core — CANopen slave core (CiA 301 subset), PURE C.
 * ----------------------------------------------------------------------------
 * Framework component without domain knowledge: the object dictionary of
 * the application (manufacturer area 0x2000..0x5FFF) and the CAN driver are
 * injected through callbacks; time comes in as a millisecond argument.
 * Therefore the whole protocol logic runs unchanged on the host
 * (test/host/test_co_core.c) and on the ESP32 (src/device/canopen_task.c).
 *
 * Implemented:
 *   - NMT slave state machine (Initialising -> Pre-operational ->
 *     Operational / Stopped), boot-up message, reset node / reset comm
 *   - Heartbeat producer (0x1017), error register (0x1001), EMCY producer
 *   - SDO server: expedited + segmented upload/download (block transfer is
 *     rejected with abort 0x05040001), one transfer at a time
 *   - 4 TPDOs / 2 RPDOs with dynamic mapping (0x1A0x / 0x160x), COB-ID,
 *     transmission type 0..240 (SYNC), 254/255 (event timer + on-change with
 *     inhibit time), SYNC consumer (0x80)
 *   - Communication profile objects 0x1000, 0x1001, 0x1005, 0x1008, 0x1009,
 *     0x100A, 0x1014, 0x1017, 0x1018, 0x1200, 0x1400.., 0x1600.., 0x1800..,
 *     0x1A00.. served internally; everything else goes to the application.
 *
 * Threading: NOT thread-safe by itself — the owner calls co_rx()/co_tick()
 * from ONE context (the CAN task). Application OD callbacks may lock.
 * ========================================================================== */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Limits ---------------------------------------------------------- */
#define CO_TPDO_COUNT       4
#define CO_RPDO_COUNT       2
#define CO_PDO_MAP_MAX      8      /* mapping entries per PDO (8 x 8 bit) */
#define CO_SDO_BUF_SIZE     128    /* largest transferable object (bytes) */
#define CO_STR_MAX          32     /* device name / versions (bytes)      */

/* ---- NMT states (CiA 301 §7.3.2.2, heartbeat encoding) --------------- */
typedef enum {
    CO_NMT_INITIALISING   = 0,
    CO_NMT_STOPPED        = 4,
    CO_NMT_OPERATIONAL    = 5,
    CO_NMT_PREOPERATIONAL = 127,
} co_nmt_state_t;

/* ---- SDO abort codes (CiA 301 §7.2.4.3.17) ---------------------------- */
#define CO_ABORT_TOGGLE          0x05030000u  /* toggle bit not alternated   */
#define CO_ABORT_TIMEOUT         0x05040000u
#define CO_ABORT_CMD             0x05040001u  /* invalid/unknown command     */
#define CO_ABORT_OUT_OF_MEMORY   0x05040005u
#define CO_ABORT_UNSUPPORTED     0x06010000u  /* unsupported access          */
#define CO_ABORT_WRITE_ONLY      0x06010001u  /* read of a write-only object */
#define CO_ABORT_READ_ONLY       0x06010002u  /* write of a read-only object */
#define CO_ABORT_NO_OBJECT       0x06020000u  /* object does not exist       */
#define CO_ABORT_PDO_MAP         0x06040041u  /* object cannot be mapped     */
#define CO_ABORT_PDO_LENGTH      0x06040042u  /* mapping exceeds PDO length  */
#define CO_ABORT_PARAM_INCOMPAT  0x06040043u
#define CO_ABORT_HW              0x06060000u
#define CO_ABORT_TYPE_MISMATCH   0x06070010u  /* data type / length mismatch */
#define CO_ABORT_TYPE_TOO_HIGH   0x06070012u
#define CO_ABORT_TYPE_TOO_LOW    0x06070013u
#define CO_ABORT_NO_SUBINDEX     0x06090011u
#define CO_ABORT_VALUE_RANGE     0x06090030u  /* value range exceeded        */
#define CO_ABORT_VALUE_HIGH      0x06090031u
#define CO_ABORT_VALUE_LOW       0x06090032u
#define CO_ABORT_GENERAL         0x08000000u
#define CO_ABORT_DATA_TRANSFER   0x08000020u
#define CO_ABORT_LOCAL_CONTROL   0x08000021u
#define CO_ABORT_DEVICE_STATE    0x08000022u

/* ---- EMCY error codes (CiA 301 table 26, subset) ---------------------- */
#define CO_EMCY_NO_ERROR         0x0000u
#define CO_EMCY_GENERIC          0x1000u
#define CO_EMCY_DEVICE_SPECIFIC  0xFF00u

/* ---- Error register bits (0x1001) ------------------------------------- */
#define CO_ERR_GENERIC           0x01u
#define CO_ERR_DEVICE_SPECIFIC   0x80u

/* ---- CAN frame (classic, 11-bit identifiers only) --------------------- */
typedef struct {
    uint16_t id;                 /* 11-bit COB-ID                           */
    uint8_t  dlc;                /* 0..8                                    */
    bool     rtr;                /* remote frame (answered for TPDOs)       */
    uint8_t  data[8];
} co_frame_t;

/* ---- Application callbacks --------------------------------------------
 * od_read / od_write serve every index the core does not own (i.e. all of
 * 0x2000..0xFFFF plus unknown communication-profile indices). They return
 * 0 on success or a CO_ABORT_* code. `cap` is the buffer capacity; the
 * callback stores the object's byte length in *len (<= cap). For PDO
 * mapping the object length must equal the mapped bit length / 8. */
typedef uint32_t (*co_od_read_fn)(void *arg, uint16_t index, uint8_t sub,
                                  uint8_t *buf, size_t cap, size_t *len);
typedef uint32_t (*co_od_write_fn)(void *arg, uint16_t index, uint8_t sub,
                                   const uint8_t *buf, size_t len);
typedef bool     (*co_can_send_fn)(void *arg, const co_frame_t *frame);
/* Optional: called after an NMT "reset node"/"reset communication" — the
 * owner re-initialises the driver as needed; the core re-enters
 * pre-operational (and sends boot-up) by itself. */
typedef void     (*co_reset_fn)(void *arg, bool bResetNode);
typedef void     (*co_nmt_changed_fn)(void *arg, co_nmt_state_t eOld,
                                      co_nmt_state_t eNew);

typedef struct {
    uint8_t     node_id;         /* 1..127                                  */
    uint16_t    heartbeat_ms;    /* 0 = heartbeat off                       */
    uint32_t    device_type;     /* 0x1000                                  */
    uint32_t    vendor_id;       /* 0x1018 sub 1                            */
    uint32_t    product_code;    /* 0x1018 sub 2                            */
    uint32_t    revision;        /* 0x1018 sub 3                            */
    uint32_t    serial;          /* 0x1018 sub 4                            */
    const char *device_name;     /* 0x1008 (<= CO_STR_MAX-1 chars used)     */
    const char *hw_version;      /* 0x1009                                  */
    const char *sw_version;      /* 0x100A                                  */
    co_can_send_fn    send;
    co_od_read_fn     od_read;
    co_od_write_fn    od_write;
    co_reset_fn       on_reset;        /* optional                          */
    co_nmt_changed_fn on_nmt_changed;  /* optional                          */
    void       *arg;
} co_config_t;

/* ---- PDO parameter sets (RAM, reset to the defaults on reset comm) ---- */
typedef struct {
    uint32_t cob_id;             /* bit 31 = invalid (disabled)             */
    uint8_t  trans_type;         /* 0..240 sync, 254/255 event-driven       */
    uint16_t inhibit_100us;      /* minimum spacing (0.1 ms units)          */
    uint16_t event_ms;           /* event timer (0 = off)                   */
    uint8_t  map_count;
    uint32_t map[CO_PDO_MAP_MAX];/* (index << 16) | (sub << 8) | bits       */
    /* runtime */
    uint32_t last_tx_ms;
    bool     have_last;
    uint8_t  last_len;
    uint8_t  last_data[8];
    uint8_t  sync_count;
} co_tpdo_t;

typedef struct {
    uint32_t cob_id;
    uint8_t  trans_type;
    uint8_t  map_count;
    uint32_t map[CO_PDO_MAP_MAX];
    /* runtime: frame buffered for synchronous application */
    bool     pending;
    uint8_t  len;
    uint8_t  data[8];
} co_rpdo_t;

typedef struct {
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t tx_failed;
    uint32_t sdo_requests;
    uint32_t sdo_aborts;
    uint32_t pdo_tx;
    uint32_t pdo_rx;
    uint32_t emcy_tx;
} co_stats_t;

typedef struct {
    co_config_t    cfg;
    co_nmt_state_t nmt;
    uint8_t        error_register;
    uint32_t       now_ms;
    uint32_t       hb_last_ms;
    bool           bootup_pending;
    /* SDO server — a single transfer at a time */
    struct {
        uint8_t  state;          /* 0 idle, 1 upload segmented, 2 download segmented */
        uint16_t index;
        uint8_t  sub;
        uint8_t  toggle;
        size_t   len;            /* total length (upload) / expected (download, 0=unknown) */
        size_t   pos;
        uint8_t  buf[CO_SDO_BUF_SIZE];
        uint32_t last_ms;        /* for the transfer timeout                */
    } sdo;
    co_tpdo_t      tpdo[CO_TPDO_COUNT];
    co_rpdo_t      rpdo[CO_RPDO_COUNT];
    co_stats_t     stats;
    uint16_t       last_emcy;
    /* Master presence: time of the last frame addressed to this node (NMT,
     * SYNC, SDO request, RPDO, RTR); master_seen = false until the first. */
    bool           master_seen;
    uint32_t       last_master_ms;
} co_node_t;

#define CO_SDO_TIMEOUT_MS   1000u   /* stalled segmented transfer is dropped */

/* ---- API --------------------------------------------------------------- */

/* Initialise the node: applies cfg, resets all PDO parameters to their
 * defaults (all disabled) and queues the boot-up message; the node enters
 * PRE-OPERATIONAL. Call co_tpdo_configure()/co_rpdo_configure() afterwards
 * for the application defaults (they are re-applied on "reset comm" through
 * the on_reset callback — the owner configures again). */
void co_init(co_node_t *node, const co_config_t *cfg, uint32_t now_ms);

/* Configure TPDO n (0..3): COB-ID (0 = default 0x180/0x280/0x380/0x480 +
 * node id), transmission type, inhibit time in ms, event timer in ms and
 * the mapping list. Returns false for an invalid mapping (bit length). */
bool co_tpdo_configure(co_node_t *node, uint8_t n, uint16_t cob_id,
                       uint8_t trans_type, uint16_t inhibit_ms,
                       uint16_t event_ms, const uint32_t *map, uint8_t count);
/* Configure RPDO n (0..1): COB-ID (0 = default 0x200/0x300 + node id). */
bool co_rpdo_configure(co_node_t *node, uint8_t n, uint16_t cob_id,
                       uint8_t trans_type, const uint32_t *map, uint8_t count);

/* Feed a received frame. */
void co_rx(co_node_t *node, const co_frame_t *frame, uint32_t now_ms);

/* Time-driven work: boot-up, heartbeat, TPDO event timers / change
 * detection, SDO timeout. Call periodically (e.g. every 10 ms). */
void co_tick(co_node_t *node, uint32_t now_ms);

/* Send an emergency (0x80 + node id) and update the error register:
 * code != 0 sets `err_bits`, code == 0 ("error reset") clears them. */
void co_emcy(co_node_t *node, uint16_t code, uint8_t err_bits,
             const uint8_t mfr[5]);

/* Force TPDO n out on the next tick regardless of inhibit/change (used by
 * the owner after an application event). */
void co_tpdo_request(co_node_t *node, uint8_t n);

co_nmt_state_t co_nmt_state(const co_node_t *node);
/* true while a master frame addressed to this node arrived within timeout. */
bool co_master_active(const co_node_t *node, uint32_t now_ms, uint32_t timeout_ms);
void co_heartbeat_set(co_node_t *node, uint16_t ms);

/* Little-endian helpers (shared with the OD glue). */
static inline void co_put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void co_put_u32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline uint16_t co_get_u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static inline uint32_t co_get_u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

#ifdef __cplusplus
}
#endif
#endif /* CO_CORE_H */
