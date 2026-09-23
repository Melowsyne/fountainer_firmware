/* Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved. */
/* ============================================================================
 * co_core.c — CANopen slave core (CiA 301 subset). PURE C, see co_core.h.
 *
 * Structure:
 *   1. frame helpers / statistics
 *   2. communication-profile object dictionary (0x1000..0x1FFF) served here
 *   3. OD dispatch (internal vs. application callbacks)
 *   4. PDO engine (mapping validation, TPDO build/send, RPDO apply)
 *   5. SDO server (expedited + segmented, aborts, timeout)
 *   6. NMT / SYNC / heartbeat / EMCY
 *   7. public API (co_init, co_rx, co_tick, ...)
 * ========================================================================== */
#include "co_core.h"
#include <string.h>

/* CANopen function codes (11-bit COB-ID = function code << 7 | node id) */
#define COB_NMT          0x000u
#define COB_SYNC         0x080u
#define COB_EMCY_BASE    0x080u
#define COB_TPDO1_BASE   0x180u
#define COB_RPDO1_BASE   0x200u
#define COB_SDO_TX_BASE  0x580u   /* server -> client                     */
#define COB_SDO_RX_BASE  0x600u   /* client -> server                     */
#define COB_HB_BASE      0x700u

#define COB_ID_INVALID   0x80000000u

#define NMT_CMD_START        0x01u
#define NMT_CMD_STOP         0x02u
#define NMT_CMD_PREOP        0x80u
#define NMT_CMD_RESET_NODE   0x81u
#define NMT_CMD_RESET_COMM   0x82u

#define SDO_IDLE          0u
#define SDO_UPLOAD_SEG    1u
#define SDO_DOWNLOAD_SEG  2u

/* ---------------------------------------------------------------------------
 * 1. Frame helpers
 * ------------------------------------------------------------------------- */
static bool tx(co_node_t *node, uint16_t id, const uint8_t *data, uint8_t dlc)
{
    co_frame_t f;
    memset(&f, 0, sizeof f);
    f.id  = (uint16_t)(id & 0x7FFu);
    f.dlc = dlc > 8 ? 8 : dlc;
    if (data && f.dlc) memcpy(f.data, data, f.dlc);
    bool ok = node->cfg.send ? node->cfg.send(node->cfg.arg, &f) : false;
    if (ok) node->stats.tx_frames++; else node->stats.tx_failed++;
    return ok;
}

static void nmt_set(co_node_t *node, co_nmt_state_t eNew)
{
    co_nmt_state_t eOld = node->nmt;
    if (eOld == eNew) return;
    node->nmt = eNew;
    if (node->cfg.on_nmt_changed)
        node->cfg.on_nmt_changed(node->cfg.arg, eOld, eNew);
}

static size_t str_len_capped(const char *s)
{
    if (!s) return 0;
    size_t n = strlen(s);
    return n > (CO_STR_MAX - 1) ? (CO_STR_MAX - 1) : n;
}

/* ---------------------------------------------------------------------------
 * 2. Communication-profile object dictionary
 * ------------------------------------------------------------------------- */
static uint32_t rd_u8(uint8_t *buf, size_t cap, size_t *len, uint8_t v)
{
    if (cap < 1) return CO_ABORT_OUT_OF_MEMORY;
    buf[0] = v; *len = 1; return 0;
}
static uint32_t rd_u16(uint8_t *buf, size_t cap, size_t *len, uint16_t v)
{
    if (cap < 2) return CO_ABORT_OUT_OF_MEMORY;
    co_put_u16(buf, v); *len = 2; return 0;
}
static uint32_t rd_u32(uint8_t *buf, size_t cap, size_t *len, uint32_t v)
{
    if (cap < 4) return CO_ABORT_OUT_OF_MEMORY;
    co_put_u32(buf, v); *len = 4; return 0;
}
static uint32_t rd_str(uint8_t *buf, size_t cap, size_t *len, const char *s)
{
    size_t n = str_len_capped(s);
    if (cap < n) return CO_ABORT_OUT_OF_MEMORY;
    if (n) memcpy(buf, s, n);
    *len = n; return 0;
}

static bool is_internal_index(uint16_t index)
{
    return index >= 0x1000u && index < 0x2000u;
}

static uint32_t od_internal_read(co_node_t *node, uint16_t index, uint8_t sub,
                                 uint8_t *buf, size_t cap, size_t *len)
{
    const co_config_t *c = &node->cfg;
    switch (index) {
    case 0x1000: if (sub) return CO_ABORT_NO_SUBINDEX;
                 return rd_u32(buf, cap, len, c->device_type);
    case 0x1001: if (sub) return CO_ABORT_NO_SUBINDEX;
                 return rd_u8(buf, cap, len, node->error_register);
    case 0x1005: if (sub) return CO_ABORT_NO_SUBINDEX;
                 return rd_u32(buf, cap, len, COB_SYNC);
    case 0x1008: if (sub) return CO_ABORT_NO_SUBINDEX;
                 return rd_str(buf, cap, len, c->device_name);
    case 0x1009: if (sub) return CO_ABORT_NO_SUBINDEX;
                 return rd_str(buf, cap, len, c->hw_version);
    case 0x100A: if (sub) return CO_ABORT_NO_SUBINDEX;
                 return rd_str(buf, cap, len, c->sw_version);
    case 0x1014: if (sub) return CO_ABORT_NO_SUBINDEX;
                 return rd_u32(buf, cap, len, (uint32_t)(COB_EMCY_BASE + c->node_id));
    case 0x1017: if (sub) return CO_ABORT_NO_SUBINDEX;
                 return rd_u16(buf, cap, len, c->heartbeat_ms);
    case 0x1018:
        switch (sub) {
        case 0: return rd_u8(buf, cap, len, 4);
        case 1: return rd_u32(buf, cap, len, c->vendor_id);
        case 2: return rd_u32(buf, cap, len, c->product_code);
        case 3: return rd_u32(buf, cap, len, c->revision);
        case 4: return rd_u32(buf, cap, len, c->serial);
        default: return CO_ABORT_NO_SUBINDEX;
        }
    case 0x1200:
        switch (sub) {
        case 0: return rd_u8(buf, cap, len, 2);
        case 1: return rd_u32(buf, cap, len, (uint32_t)(COB_SDO_RX_BASE + c->node_id));
        case 2: return rd_u32(buf, cap, len, (uint32_t)(COB_SDO_TX_BASE + c->node_id));
        default: return CO_ABORT_NO_SUBINDEX;
        }
    default: break;
    }

    if (index >= 0x1400u && index < 0x1400u + CO_RPDO_COUNT) {          /* RPDO comm */
        const co_rpdo_t *p = &node->rpdo[index - 0x1400u];
        switch (sub) {
        case 0: return rd_u8(buf, cap, len, 2);
        case 1: return rd_u32(buf, cap, len, p->cob_id);
        case 2: return rd_u8(buf, cap, len, p->trans_type);
        default: return CO_ABORT_NO_SUBINDEX;
        }
    }
    if (index >= 0x1600u && index < 0x1600u + CO_RPDO_COUNT) {          /* RPDO map */
        const co_rpdo_t *p = &node->rpdo[index - 0x1600u];
        if (sub == 0) return rd_u8(buf, cap, len, p->map_count);
        if (sub <= CO_PDO_MAP_MAX) return rd_u32(buf, cap, len, p->map[sub - 1]);
        return CO_ABORT_NO_SUBINDEX;
    }
    if (index >= 0x1800u && index < 0x1800u + CO_TPDO_COUNT) {          /* TPDO comm */
        const co_tpdo_t *p = &node->tpdo[index - 0x1800u];
        switch (sub) {
        case 0: return rd_u8(buf, cap, len, 5);
        case 1: return rd_u32(buf, cap, len, p->cob_id);
        case 2: return rd_u8(buf, cap, len, p->trans_type);
        case 3: return rd_u16(buf, cap, len, p->inhibit_100us);
        case 4: return rd_u8(buf, cap, len, 0);                /* reserved */
        case 5: return rd_u16(buf, cap, len, p->event_ms);
        default: return CO_ABORT_NO_SUBINDEX;
        }
    }
    if (index >= 0x1A00u && index < 0x1A00u + CO_TPDO_COUNT) {          /* TPDO map */
        const co_tpdo_t *p = &node->tpdo[index - 0x1A00u];
        if (sub == 0) return rd_u8(buf, cap, len, p->map_count);
        if (sub <= CO_PDO_MAP_MAX) return rd_u32(buf, cap, len, p->map[sub - 1]);
        return CO_ABORT_NO_SUBINDEX;
    }
    return CO_ABORT_NO_OBJECT;
}

/* forward */
static uint32_t od_read(co_node_t *node, uint16_t index, uint8_t sub,
                        uint8_t *buf, size_t cap, size_t *len);
static uint32_t map_validate(co_node_t *node, const uint32_t *map, uint8_t count,
                             bool bForTx);

static uint32_t od_internal_write(co_node_t *node, uint16_t index, uint8_t sub,
                                  const uint8_t *buf, size_t len)
{
    if (index == 0x1017u) {
        if (sub) return CO_ABORT_NO_SUBINDEX;
        if (len != 2) return CO_ABORT_TYPE_MISMATCH;
        node->cfg.heartbeat_ms = co_get_u16(buf);
        node->hb_last_ms = node->now_ms;
        return 0;
    }
    if (index == 0x1000u || index == 0x1001u || index == 0x1005u ||
        index == 0x1008u || index == 0x1009u || index == 0x100Au ||
        index == 0x1014u || index == 0x1018u || index == 0x1200u)
        return CO_ABORT_READ_ONLY;

    if (index >= 0x1400u && index < 0x1400u + CO_RPDO_COUNT) {
        co_rpdo_t *p = &node->rpdo[index - 0x1400u];
        switch (sub) {
        case 0: return CO_ABORT_READ_ONLY;
        case 1: {
            if (len != 4) return CO_ABORT_TYPE_MISMATCH;
            uint32_t v = co_get_u32(buf);
            bool bOldValid = !(p->cob_id & COB_ID_INVALID), bNewValid = !(v & COB_ID_INVALID);
            if (bOldValid && bNewValid && ((v ^ p->cob_id) & 0x7FFu))
                return CO_ABORT_PARAM_INCOMPAT;     /* change ID only while invalid */
            if ((v & 0x7FFu) == 0) return CO_ABORT_VALUE_RANGE;
            p->cob_id = v & (COB_ID_INVALID | 0x7FFu);
            p->pending = false;
            return 0;
        }
        case 2: if (len != 1) return CO_ABORT_TYPE_MISMATCH;
                if (buf[0] > 240 && buf[0] < 254) return CO_ABORT_VALUE_RANGE;
                p->trans_type = buf[0]; p->pending = false; return 0;
        default: return CO_ABORT_NO_SUBINDEX;
        }
    }
    if (index >= 0x1600u && index < 0x1600u + CO_RPDO_COUNT) {
        co_rpdo_t *p = &node->rpdo[index - 0x1600u];
        if (sub == 0) {
            if (len != 1) return CO_ABORT_TYPE_MISMATCH;
            if (buf[0] > CO_PDO_MAP_MAX) return CO_ABORT_VALUE_RANGE;
            uint32_t abort = map_validate(node, p->map, buf[0], false);
            if (abort) return abort;
            p->map_count = buf[0];
            return 0;
        }
        if (sub > CO_PDO_MAP_MAX) return CO_ABORT_NO_SUBINDEX;
        if (len != 4) return CO_ABORT_TYPE_MISMATCH;
        if (p->map_count != 0) return CO_ABORT_UNSUPPORTED;   /* disable first (sub0 = 0) */
        p->map[sub - 1] = co_get_u32(buf);
        return 0;
    }
    if (index >= 0x1800u && index < 0x1800u + CO_TPDO_COUNT) {
        co_tpdo_t *p = &node->tpdo[index - 0x1800u];
        switch (sub) {
        case 0: return CO_ABORT_READ_ONLY;
        case 1: {
            if (len != 4) return CO_ABORT_TYPE_MISMATCH;
            uint32_t v = co_get_u32(buf);
            bool bOldValid = !(p->cob_id & COB_ID_INVALID), bNewValid = !(v & COB_ID_INVALID);
            if (bOldValid && bNewValid && ((v ^ p->cob_id) & 0x7FFu))
                return CO_ABORT_PARAM_INCOMPAT;
            if ((v & 0x7FFu) == 0) return CO_ABORT_VALUE_RANGE;
            p->cob_id = v & (COB_ID_INVALID | 0x7FFu);
            p->have_last = false;
            return 0;
        }
        case 2: if (len != 1) return CO_ABORT_TYPE_MISMATCH;
                if (buf[0] > 240 && buf[0] < 252) return CO_ABORT_VALUE_RANGE;
                p->trans_type = buf[0]; p->sync_count = 0; return 0;
        case 3: if (len != 2) return CO_ABORT_TYPE_MISMATCH;
                p->inhibit_100us = co_get_u16(buf); return 0;
        case 4: return CO_ABORT_READ_ONLY;
        case 5: if (len != 2) return CO_ABORT_TYPE_MISMATCH;
                p->event_ms = co_get_u16(buf); return 0;
        default: return CO_ABORT_NO_SUBINDEX;
        }
    }
    if (index >= 0x1A00u && index < 0x1A00u + CO_TPDO_COUNT) {
        co_tpdo_t *p = &node->tpdo[index - 0x1A00u];
        if (sub == 0) {
            if (len != 1) return CO_ABORT_TYPE_MISMATCH;
            if (buf[0] > CO_PDO_MAP_MAX) return CO_ABORT_VALUE_RANGE;
            uint32_t abort = map_validate(node, p->map, buf[0], true);
            if (abort) return abort;
            p->map_count = buf[0];
            p->have_last = false;
            return 0;
        }
        if (sub > CO_PDO_MAP_MAX) return CO_ABORT_NO_SUBINDEX;
        if (len != 4) return CO_ABORT_TYPE_MISMATCH;
        if (p->map_count != 0) return CO_ABORT_UNSUPPORTED;
        p->map[sub - 1] = co_get_u32(buf);
        return 0;
    }
    return CO_ABORT_NO_OBJECT;
}

/* ---------------------------------------------------------------------------
 * 3. OD dispatch
 * ------------------------------------------------------------------------- */
static uint32_t od_read(co_node_t *node, uint16_t index, uint8_t sub,
                        uint8_t *buf, size_t cap, size_t *len)
{
    *len = 0;
    if (is_internal_index(index))
        return od_internal_read(node, index, sub, buf, cap, len);
    if (!node->cfg.od_read) return CO_ABORT_NO_OBJECT;
    return node->cfg.od_read(node->cfg.arg, index, sub, buf, cap, len);
}

static uint32_t od_write(co_node_t *node, uint16_t index, uint8_t sub,
                         const uint8_t *buf, size_t len)
{
    if (is_internal_index(index))
        return od_internal_write(node, index, sub, buf, len);
    if (!node->cfg.od_write) return CO_ABORT_NO_OBJECT;
    return node->cfg.od_write(node->cfg.arg, index, sub, buf, len);
}

/* ---------------------------------------------------------------------------
 * 4. PDO engine
 * ------------------------------------------------------------------------- */
static uint32_t map_validate(co_node_t *node, const uint32_t *map, uint8_t count,
                             bool bForTx)
{
    unsigned bits_total = 0;
    for (uint8_t i = 0; i < count; i++) {
        uint16_t index = (uint16_t)(map[i] >> 16);
        uint8_t  sub   = (uint8_t)(map[i] >> 8);
        uint8_t  bits  = (uint8_t)map[i];
        if (bits != 8 && bits != 16 && bits != 32 && bits != 64)
            return CO_ABORT_PDO_MAP;
        bits_total += bits;
        if (bits_total > 64) return CO_ABORT_PDO_LENGTH;
        /* The object must exist and (for TPDOs) be readable with exactly
         * the mapped length; RPDO targets are only checked for existence. */
        uint8_t tmp[CO_SDO_BUF_SIZE];
        size_t  len = 0;
        uint32_t abort = od_read(node, index, sub, tmp, sizeof tmp, &len);
        if (abort) return CO_ABORT_PDO_MAP;
        if (bForTx && len != (size_t)bits / 8u) return CO_ABORT_PDO_MAP;
        if (!bForTx && index < 0x2000u) return CO_ABORT_PDO_MAP;   /* no comm objects in RPDOs */
    }
    return 0;
}

/* Build TPDO n into data[]; returns the byte length, or -1 if a mapped
 * object cannot be read (PDO is then not sent). */
static int tpdo_build(co_node_t *node, const co_tpdo_t *p, uint8_t *data)
{
    unsigned pos = 0;
    for (uint8_t i = 0; i < p->map_count; i++) {
        uint16_t index = (uint16_t)(p->map[i] >> 16);
        uint8_t  sub   = (uint8_t)(p->map[i] >> 8);
        unsigned n     = (uint8_t)p->map[i] / 8u;
        uint8_t  tmp[CO_SDO_BUF_SIZE];
        size_t   len = 0;
        if (od_read(node, index, sub, tmp, sizeof tmp, &len) || len != n || pos + n > 8)
            return -1;
        memcpy(data + pos, tmp, n);
        pos += n;
    }
    return (int)pos;
}

static void tpdo_send(co_node_t *node, co_tpdo_t *p)
{
    uint8_t data[8];
    int n = tpdo_build(node, p, data);
    if (n < 0) return;
    if (tx(node, (uint16_t)(p->cob_id & 0x7FFu), data, (uint8_t)n)) {
        node->stats.pdo_tx++;
        p->last_tx_ms = node->now_ms;
        p->have_last  = true;
        p->last_len   = (uint8_t)n;
        memcpy(p->last_data, data, (size_t)n);
        p->sync_count = 0;
    }
}

static bool tpdo_enabled(const co_tpdo_t *p)
{
    return !(p->cob_id & COB_ID_INVALID) && p->map_count > 0;
}

static bool tpdo_changed(co_node_t *node, co_tpdo_t *p)
{
    uint8_t data[8];
    int n = tpdo_build(node, p, data);
    if (n < 0) return false;
    if (!p->have_last || p->last_len != (uint8_t)n) return true;
    return memcmp(p->last_data, data, (size_t)n) != 0;
}

/* Event-driven TPDOs (254/255): inhibit + on-change + event timer. */
static void tpdo_tick(co_node_t *node)
{
    if (node->nmt != CO_NMT_OPERATIONAL) return;
    for (uint8_t i = 0; i < CO_TPDO_COUNT; i++) {
        co_tpdo_t *p = &node->tpdo[i];
        if (!tpdo_enabled(p) || p->trans_type < 254) continue;
        uint32_t since = node->now_ms - p->last_tx_ms;
        uint32_t inhibit_ms = ((uint32_t)p->inhibit_100us + 9u) / 10u;
        if (p->have_last && since < inhibit_ms) continue;
        bool bTimer = p->event_ms && (!p->have_last || since >= p->event_ms);
        if (bTimer || tpdo_changed(node, p))
            tpdo_send(node, p);
    }
}

static void rpdo_apply(co_node_t *node, co_rpdo_t *p, const uint8_t *data, uint8_t len)
{
    /* CiA 301 §7.2.2.1.2: a PDO shorter than its mapping is not processed
     * at all (no partial writes) and reported as EMCY 0x8210. */
    unsigned need = 0;
    for (uint8_t i = 0; i < p->map_count; i++) need += (uint8_t)p->map[i] / 8u;
    if (need > len) {
        static const uint8_t mfr[5] = {0};
        co_emcy(node, 0x8210u, CO_ERR_GENERIC, mfr);
        return;
    }
    unsigned pos = 0;
    for (uint8_t i = 0; i < p->map_count; i++) {
        uint16_t index = (uint16_t)(p->map[i] >> 16);
        uint8_t  sub   = (uint8_t)(p->map[i] >> 8);
        unsigned n     = (uint8_t)p->map[i] / 8u;
        (void)od_write(node, index, sub, data + pos, n);   /* errors: no SDO context to report */
        pos += n;
    }
    node->stats.pdo_rx++;
}

static void sync_rx(co_node_t *node)
{
    if (node->nmt != CO_NMT_OPERATIONAL) return;
    for (uint8_t i = 0; i < CO_TPDO_COUNT; i++) {
        co_tpdo_t *p = &node->tpdo[i];
        if (!tpdo_enabled(p) || p->trans_type > 240) continue;
        if (p->trans_type == 0) {                     /* acyclic: only if changed */
            if (tpdo_changed(node, p)) tpdo_send(node, p);
        } else if (++p->sync_count >= p->trans_type) {
            tpdo_send(node, p);                       /* resets sync_count */
        }
    }
    for (uint8_t i = 0; i < CO_RPDO_COUNT; i++) {
        co_rpdo_t *p = &node->rpdo[i];
        if (p->pending) { p->pending = false; rpdo_apply(node, p, p->data, p->len); }
    }
}

static bool pdo_rx(co_node_t *node, const co_frame_t *f)
{
    /* RTR on a TPDO COB-ID: answer with the current data (types 252/253
     * and — tolerant — the event-driven ones). */
    if (f->rtr) {
        for (uint8_t i = 0; i < CO_TPDO_COUNT; i++) {
            co_tpdo_t *p = &node->tpdo[i];
            if (tpdo_enabled(p) && (p->cob_id & 0x7FFu) == f->id) {
                if (node->nmt == CO_NMT_OPERATIONAL) tpdo_send(node, p);
                return true;
            }
        }
        return false;
    }
    for (uint8_t i = 0; i < CO_RPDO_COUNT; i++) {
        co_rpdo_t *p = &node->rpdo[i];
        if ((p->cob_id & COB_ID_INVALID) || p->map_count == 0 ||
            (p->cob_id & 0x7FFu) != f->id) continue;
        if (node->nmt != CO_NMT_OPERATIONAL) return true;
        if (p->trans_type <= 240) {                   /* synchronous: hold until SYNC */
            p->pending = true; p->len = f->dlc; memcpy(p->data, f->data, 8);
        } else {
            rpdo_apply(node, p, f->data, f->dlc);
        }
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * 5. SDO server
 * ------------------------------------------------------------------------- */
static void sdo_reply(co_node_t *node, const uint8_t d[8])
{
    tx(node, (uint16_t)(COB_SDO_TX_BASE + node->cfg.node_id), d, 8);
}

static void sdo_abort(co_node_t *node, uint16_t index, uint8_t sub, uint32_t code)
{
    uint8_t d[8] = {0x80, 0, 0, 0, 0, 0, 0, 0};
    co_put_u16(&d[1], index); d[3] = sub; co_put_u32(&d[4], code);
    sdo_reply(node, d);
    node->stats.sdo_aborts++;
    node->sdo.state = SDO_IDLE;
}

static void sdo_upload_initiate(co_node_t *node, uint16_t index, uint8_t sub)
{
    size_t len = 0;
    uint32_t abort = od_read(node, index, sub, node->sdo.buf, sizeof node->sdo.buf, &len);
    if (abort) { sdo_abort(node, index, sub, abort); return; }

    uint8_t d[8] = {0};
    co_put_u16(&d[1], index); d[3] = sub;
    if (len <= 4) {                                   /* expedited */
        d[0] = (uint8_t)(0x43u | ((4u - len) << 2));  /* scs=2, e=1, s=1, n */
        memcpy(&d[4], node->sdo.buf, len);
        node->sdo.state = SDO_IDLE;
    } else {                                          /* segmented, size indicated */
        d[0] = 0x41;
        co_put_u32(&d[4], (uint32_t)len);
        node->sdo.state  = SDO_UPLOAD_SEG;
        node->sdo.index  = index; node->sdo.sub = sub;
        node->sdo.len    = len;   node->sdo.pos = 0;
        node->sdo.toggle = 0;
        node->sdo.last_ms = node->now_ms;
    }
    sdo_reply(node, d);
}

static void sdo_upload_segment(co_node_t *node, uint8_t cmd)
{
    if (node->sdo.state != SDO_UPLOAD_SEG) {
        sdo_abort(node, 0, 0, CO_ABORT_CMD); return;
    }
    uint8_t toggle = (cmd >> 4) & 1u;
    if (toggle != node->sdo.toggle) {
        sdo_abort(node, node->sdo.index, node->sdo.sub, CO_ABORT_TOGGLE); return;
    }
    size_t remain = node->sdo.len - node->sdo.pos;
    size_t n = remain > 7 ? 7 : remain;
    bool bLast = (n == remain);
    uint8_t d[8] = {0};
    d[0] = (uint8_t)((toggle << 4) | ((7u - n) << 1) | (bLast ? 1u : 0u));
    memcpy(&d[1], node->sdo.buf + node->sdo.pos, n);
    node->sdo.pos += n;
    node->sdo.toggle ^= 1u;
    node->sdo.last_ms = node->now_ms;
    if (bLast) node->sdo.state = SDO_IDLE;
    sdo_reply(node, d);
}

static void sdo_download_initiate(co_node_t *node, uint8_t cmd, uint16_t index,
                                  uint8_t sub, const uint8_t *payload)
{
    bool e = (cmd & 0x02u) != 0, s = (cmd & 0x01u) != 0;
    if (e) {                                          /* expedited: 1..4 bytes */
        size_t len = s ? 4u - ((cmd >> 2) & 3u) : 4u;
        uint32_t abort = od_write(node, index, sub, payload, len);
        if (abort) { sdo_abort(node, index, sub, abort); return; }
        uint8_t d[8] = {0x60, 0, 0, 0, 0, 0, 0, 0};
        co_put_u16(&d[1], index); d[3] = sub;
        node->sdo.state = SDO_IDLE;
        sdo_reply(node, d);
        return;
    }
    size_t expected = s ? co_get_u32(payload) : 0;
    if (expected > sizeof node->sdo.buf) {
        sdo_abort(node, index, sub, CO_ABORT_OUT_OF_MEMORY); return;
    }
    node->sdo.state  = SDO_DOWNLOAD_SEG;
    node->sdo.index  = index; node->sdo.sub = sub;
    node->sdo.len    = expected; node->sdo.pos = 0;
    node->sdo.toggle = 0;
    node->sdo.last_ms = node->now_ms;
    uint8_t d[8] = {0x60, 0, 0, 0, 0, 0, 0, 0};
    co_put_u16(&d[1], index); d[3] = sub;
    sdo_reply(node, d);
}

static void sdo_download_segment(co_node_t *node, uint8_t cmd, const uint8_t *payload)
{
    if (node->sdo.state != SDO_DOWNLOAD_SEG) {
        sdo_abort(node, 0, 0, CO_ABORT_CMD); return;
    }
    uint8_t toggle = (cmd >> 4) & 1u;
    if (toggle != node->sdo.toggle) {
        sdo_abort(node, node->sdo.index, node->sdo.sub, CO_ABORT_TOGGLE); return;
    }
    size_t n = 7u - ((cmd >> 1) & 7u);
    bool bLast = (cmd & 1u) != 0;
    if (node->sdo.pos + n > sizeof node->sdo.buf) {
        sdo_abort(node, node->sdo.index, node->sdo.sub, CO_ABORT_OUT_OF_MEMORY); return;
    }
    memcpy(node->sdo.buf + node->sdo.pos, payload, n);
    node->sdo.pos += n;
    node->sdo.last_ms = node->now_ms;
    uint8_t d[8] = {0};
    d[0] = (uint8_t)(0x20u | (toggle << 4));
    node->sdo.toggle ^= 1u;
    if (bLast) {
        if (node->sdo.len && node->sdo.pos != node->sdo.len) {
            sdo_abort(node, node->sdo.index, node->sdo.sub, CO_ABORT_TYPE_MISMATCH); return;
        }
        uint32_t abort = od_write(node, node->sdo.index, node->sdo.sub,
                                  node->sdo.buf, node->sdo.pos);
        node->sdo.state = SDO_IDLE;
        if (abort) { sdo_abort(node, node->sdo.index, node->sdo.sub, abort); return; }
    }
    sdo_reply(node, d);
}

static void sdo_rx(co_node_t *node, const co_frame_t *f)
{
    if (f->dlc < 8 || f->rtr) return;                 /* malformed: ignore  */
    node->stats.sdo_requests++;
    uint8_t  cmd   = f->data[0];
    uint8_t  ccs   = cmd >> 5;
    uint16_t index = co_get_u16(&f->data[1]);
    uint8_t  sub   = f->data[3];
    switch (ccs) {
    case 0: sdo_download_segment(node, cmd, &f->data[1]); break;
    case 1: sdo_download_initiate(node, cmd, index, sub, &f->data[4]); break;
    case 2: sdo_upload_initiate(node, index, sub); break;
    case 3: sdo_upload_segment(node, cmd); break;
    case 4: node->sdo.state = SDO_IDLE; break;        /* client abort       */
    default: sdo_abort(node, index, sub, CO_ABORT_CMD); break;   /* block transfer etc. */
    }
}

/* ---------------------------------------------------------------------------
 * 6. NMT / heartbeat / EMCY
 * ------------------------------------------------------------------------- */
static void node_reset(co_node_t *node, bool bResetNode)
{
    node->sdo.state = SDO_IDLE;
    for (uint8_t i = 0; i < CO_TPDO_COUNT; i++) {
        co_tpdo_t *p = &node->tpdo[i];
        memset(p, 0, sizeof *p);
        p->cob_id = COB_ID_INVALID | (uint32_t)(COB_TPDO1_BASE + 0x100u * i + node->cfg.node_id);
        p->trans_type = 255;
    }
    for (uint8_t i = 0; i < CO_RPDO_COUNT; i++) {
        co_rpdo_t *p = &node->rpdo[i];
        memset(p, 0, sizeof *p);
        p->cob_id = COB_ID_INVALID | (uint32_t)(COB_RPDO1_BASE + 0x100u * i + node->cfg.node_id);
        p->trans_type = 255;
    }
    if (bResetNode) node->error_register = 0;
    node->nmt = CO_NMT_INITIALISING;
    if (node->cfg.on_reset) node->cfg.on_reset(node->cfg.arg, bResetNode);
    node->bootup_pending = true;
    node->hb_last_ms = node->now_ms;
    nmt_set(node, CO_NMT_PREOPERATIONAL);
}

static void nmt_rx(co_node_t *node, const co_frame_t *f)
{
    if (f->dlc < 2) return;
    uint8_t cmd = f->data[0], target = f->data[1];
    if (target != 0 && target != node->cfg.node_id) return;
    switch (cmd) {
    case NMT_CMD_START:      nmt_set(node, CO_NMT_OPERATIONAL);
                             for (uint8_t i = 0; i < CO_TPDO_COUNT; i++) node->tpdo[i].have_last = false;
                             break;
    case NMT_CMD_STOP:       nmt_set(node, CO_NMT_STOPPED); node->sdo.state = SDO_IDLE; break;
    case NMT_CMD_PREOP:      nmt_set(node, CO_NMT_PREOPERATIONAL); break;
    case NMT_CMD_RESET_NODE: node_reset(node, true);  break;
    case NMT_CMD_RESET_COMM: node_reset(node, false); break;
    default: break;
    }
}

static void heartbeat_tick(co_node_t *node)
{
    if (node->bootup_pending) {
        uint8_t d = 0x00;                             /* boot-up            */
        if (tx(node, (uint16_t)(COB_HB_BASE + node->cfg.node_id), &d, 1)) {
            node->bootup_pending = false;
            node->hb_last_ms = node->now_ms;
        }
        return;
    }
    if (node->cfg.heartbeat_ms == 0) return;
    if (node->now_ms - node->hb_last_ms >= node->cfg.heartbeat_ms) {
        uint8_t d = (uint8_t)node->nmt;
        tx(node, (uint16_t)(COB_HB_BASE + node->cfg.node_id), &d, 1);
        node->hb_last_ms = node->now_ms;
    }
}

void co_emcy(co_node_t *node, uint16_t code, uint8_t err_bits, const uint8_t mfr[5])
{
    if (code == CO_EMCY_NO_ERROR) node->error_register &= (uint8_t)~err_bits;
    else                          node->error_register |= err_bits;
    if (node->nmt == CO_NMT_STOPPED || node->nmt == CO_NMT_INITIALISING) return;
    uint8_t d[8] = {0};
    co_put_u16(&d[0], code);
    d[2] = node->error_register;
    if (mfr) memcpy(&d[3], mfr, 5);
    if (tx(node, (uint16_t)(COB_EMCY_BASE + node->cfg.node_id), d, 8))
        node->stats.emcy_tx++;
    node->last_emcy = code;
}

/* ---------------------------------------------------------------------------
 * 7. Public API
 * ------------------------------------------------------------------------- */
void co_init(co_node_t *node, const co_config_t *cfg, uint32_t now_ms)
{
    memset(node, 0, sizeof *node);
    node->cfg = *cfg;
    if (node->cfg.node_id == 0 || node->cfg.node_id > 127) node->cfg.node_id = 127;
    node->now_ms = now_ms;
    /* Same path as an NMT "reset node", but without the owner callback
     * (the owner is in the middle of its own init). */
    co_reset_fn saved = node->cfg.on_reset;
    node->cfg.on_reset = 0;
    node_reset(node, true);
    node->cfg.on_reset = saved;
}

bool co_tpdo_configure(co_node_t *node, uint8_t n, uint16_t cob_id,
                       uint8_t trans_type, uint16_t inhibit_ms,
                       uint16_t event_ms, const uint32_t *map, uint8_t count)
{
    if (n >= CO_TPDO_COUNT || count > CO_PDO_MAP_MAX) return false;
    if (map_validate(node, map, count, true) != 0) return false;
    co_tpdo_t *p = &node->tpdo[n];
    p->cob_id = cob_id ? (uint32_t)(cob_id & 0x7FFu)
                       : (uint32_t)(COB_TPDO1_BASE + 0x100u * n + node->cfg.node_id);
    p->trans_type    = trans_type;
    p->inhibit_100us = (uint16_t)(inhibit_ms * 10u);
    p->event_ms      = event_ms;
    p->map_count     = count;
    if (count) memcpy(p->map, map, count * sizeof map[0]);
    p->have_last = false; p->sync_count = 0;
    return true;
}

bool co_rpdo_configure(co_node_t *node, uint8_t n, uint16_t cob_id,
                       uint8_t trans_type, const uint32_t *map, uint8_t count)
{
    if (n >= CO_RPDO_COUNT || count > CO_PDO_MAP_MAX) return false;
    if (map_validate(node, map, count, false) != 0) return false;
    co_rpdo_t *p = &node->rpdo[n];
    p->cob_id = cob_id ? (uint32_t)(cob_id & 0x7FFu)
                       : (uint32_t)(COB_RPDO1_BASE + 0x100u * n + node->cfg.node_id);
    p->trans_type = trans_type;
    p->map_count  = count;
    if (count) memcpy(p->map, map, count * sizeof map[0]);
    p->pending = false;
    return true;
}

static void master_note(co_node_t *node)
{
    node->master_seen    = true;
    node->last_master_ms = node->now_ms;
}

void co_rx(co_node_t *node, const co_frame_t *f, uint32_t now_ms)
{
    node->now_ms = now_ms;
    node->stats.rx_frames++;
    if (f->id == COB_NMT) {
        if (f->dlc >= 2 && (f->data[1] == 0 || f->data[1] == node->cfg.node_id)) master_note(node);
        nmt_rx(node, f);
        return;
    }
    if (node->nmt == CO_NMT_INITIALISING) return;
    if (f->id == COB_SYNC && !f->rtr) { master_note(node); sync_rx(node); return; }
    if (node->nmt == CO_NMT_STOPPED) return;          /* only NMT + heartbeat */
    if (f->id == (uint16_t)(COB_SDO_RX_BASE + node->cfg.node_id)) {
        master_note(node); sdo_rx(node, f); return;
    }
    if (pdo_rx(node, f)) master_note(node);
}

bool co_master_active(const co_node_t *node, uint32_t now_ms, uint32_t timeout_ms)
{
    return node->master_seen && (now_ms - node->last_master_ms) <= timeout_ms;
}

void co_tick(co_node_t *node, uint32_t now_ms)
{
    node->now_ms = now_ms;
    heartbeat_tick(node);
    if (node->sdo.state != SDO_IDLE &&
        now_ms - node->sdo.last_ms > CO_SDO_TIMEOUT_MS) {
        sdo_abort(node, node->sdo.index, node->sdo.sub, CO_ABORT_TIMEOUT);
    }
    tpdo_tick(node);
}

void co_tpdo_request(co_node_t *node, uint8_t n)
{
    if (n < CO_TPDO_COUNT) node->tpdo[n].have_last = false;
}

co_nmt_state_t co_nmt_state(const co_node_t *node) { return node->nmt; }

void co_heartbeat_set(co_node_t *node, uint16_t ms)
{
    node->cfg.heartbeat_ms = ms;
    node->hb_last_ms = node->now_ms;
}
