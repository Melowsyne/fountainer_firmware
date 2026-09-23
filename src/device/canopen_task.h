/* Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved. */
#ifndef CANOPEN_TASK_H
#define CANOPEN_TASK_H
/* =============================================================
 * canopen_task — IDF binding of the CANopen slave core
 * (src/components/canopen/co_core) to this board and its
 * datapoint catalog (DOKU/CANopen.md).
 *
 * Owns the TWAI driver (HW2.x: TX IO5, RX IO4, TJA1051T/3
 * standby IO21 via hal) and runs the protocol under the
 * task_manager (TM_TASK_CANOPEN, 10 ms): drain the RX queue
 * -> co_rx, co_tick (heartbeat, TPDO timers/change detection),
 * bus-off recovery, EMCY on pump faults, Can_* status
 * datapoints, config re-apply (node id / bitrate / heartbeat
 * / enable) once per second.
 *
 * Object dictionary served to the master:
 *   0x1000..0x1FFF  communication profile (inside co_core)
 *   0x2000 + i      datapoint i (dp_id_t order), sub 0 = value
 *   0x2FFF          number of datapoints (U16)
 *   0x3000 + i      descriptor record: sub1 name, sub2 type,
 *                   sub3 access (self-description without EDS)
 * Writes go through task_com_apply_dp_write() — the same
 * validated path as the cloud/local dp_write (range check,
 * cross-field constraints, NVS persist, Network_Save/
 * Log_Command interception). Passwords are write-only on CAN.
 *
 * Boards without transceiver (devkit): canopen_init() succeeds,
 * Can_State stays 0 and the cycle is a no-op.
 * ============================================================= */
#include <stdbool.h>
#include "esp_err.h"
#include "task_manager.h"

/* Cycle period = upper bound of the blocking RX wait; RX frames wake the
 * task immediately (see canopen_task_cycle). */
#define CANOPEN_TASK_PERIOD_MS 10

/* Reads Can_* config, releases the transceiver, installs the TWAI driver
 * and initialises the core with the default PDO set. Never fatal for the
 * device: on driver errors it logs, sets Can_State=0 and returns true. */
bool canopen_init(void);

esp_err_t canopen_task_cycle(tm_task_ctx_t *pstCtx);

/* Watchdog recovery hook (WD_CH_CANOPEN): reinstall + restart the driver. */
bool canopen_recover(void);

#endif /* CANOPEN_TASK_H */
