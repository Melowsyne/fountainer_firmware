/* Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved. */
/* Host test for the CANopen slave core (src/components/canopen/src/co_core.c).
 * PURE: frames go in via co_rx(), time via co_tick(), the object dictionary
 * and the CAN "driver" are fakes below — no mocks, plain gcc. Verifies the
 * CiA 301 wire format byte-exactly (SDO command specifiers, toggle bits,
 * abort codes, heartbeat/boot-up encoding, PDO packing, EMCY layout). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "co_core.h"

static int s_checks;
#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        s_checks++;                                                       \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL(%d): %s (%s:%d)\n", s_checks, msg,      \
                    __FILE__, __LINE__);                                  \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

/* ---- fake CAN driver: captures every transmitted frame ---------------- */
#define CAP_MAX 64
static co_frame_t s_cap[CAP_MAX];
static int s_ncap;
static bool s_send_fail;

static bool fake_send(void *arg, const co_frame_t *f)
{
    (void)arg;
    if (s_send_fail) return false;
    if (s_ncap < CAP_MAX) s_cap[s_ncap++] = *f;
    return true;
}
static void cap_reset(void) { s_ncap = 0; }
static const co_frame_t *last(void) { return s_ncap ? &s_cap[s_ncap - 1] : NULL; }

/* ---- fake application OD ---------------------------------------------- */
static uint8_t  s_u8  = 0x11;
static uint16_t s_u16 = 0x2233;
static uint32_t s_u32 = 0x44556677;
static float    s_f32 = 3.25f;
static char     s_str[32] = "Fountainer-Prototype";   /* 20 chars */
static uint64_t s_u64 = 0x0102030405060708ull;
static uint8_t  s_wo  = 0;

static uint32_t fake_od_read(void *arg, uint16_t index, uint8_t sub,
                             uint8_t *buf, size_t cap, size_t *len)
{
    (void)arg;
    if (sub != 0) return CO_ABORT_NO_SUBINDEX;
    switch (index) {
    case 0x2000: if (cap < 1) return CO_ABORT_OUT_OF_MEMORY; buf[0] = s_u8; *len = 1; return 0;
    case 0x2001: co_put_u16(buf, s_u16); *len = 2; return 0;
    case 0x2002: co_put_u32(buf, s_u32); *len = 4; return 0;
    case 0x2003: memcpy(buf, &s_f32, 4); *len = 4; return 0;
    case 0x2004: *len = strlen(s_str); if (cap < *len) return CO_ABORT_OUT_OF_MEMORY;
                 memcpy(buf, s_str, *len); return 0;
    case 0x2005: memcpy(buf, &s_u64, 8); *len = 8; return 0;
    case 0x2006: return CO_ABORT_WRITE_ONLY;
    default: return CO_ABORT_NO_OBJECT;
    }
}

static uint32_t fake_od_write(void *arg, uint16_t index, uint8_t sub,
                              const uint8_t *buf, size_t len)
{
    (void)arg;
    if (sub != 0) return CO_ABORT_NO_SUBINDEX;
    switch (index) {
    case 0x2000: if (len != 1) return CO_ABORT_TYPE_MISMATCH; s_u8 = buf[0]; return 0;
    case 0x2001: if (len != 2) return CO_ABORT_TYPE_MISMATCH; s_u16 = co_get_u16(buf); return 0;
    case 0x2002: return CO_ABORT_READ_ONLY;
    case 0x2003: if (len != 4) return CO_ABORT_TYPE_MISMATCH; memcpy(&s_f32, buf, 4); return 0;
    case 0x2004: if (len >= sizeof s_str) return CO_ABORT_TYPE_TOO_HIGH;
                 memcpy(s_str, buf, len); s_str[len] = 0; return 0;
    case 0x2006: if (len != 1) return CO_ABORT_TYPE_MISMATCH; s_wo = buf[0]; return 0;
    default: return CO_ABORT_NO_OBJECT;
    }
}

static int s_resets, s_reset_node;
static void fake_reset(void *arg, bool bNode) { (void)arg; s_resets++; if (bNode) s_reset_node++; }

#define NODE_ID 3
static co_node_t s_node;
static uint32_t  s_now;

static void node_setup(uint16_t hb_ms)
{
    co_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.node_id      = NODE_ID;
    cfg.heartbeat_ms = hb_ms;
    cfg.device_type  = 0x000F0191u;
    cfg.vendor_id    = 0x12345678u;
    cfg.product_code = 0x464E5400u;
    cfg.revision     = 0x00040028u;
    cfg.serial       = 0x00000003u;
    cfg.device_name  = "Fountainer";
    cfg.hw_version   = "HW2.0";
    cfg.sw_version   = "4.40.0";
    cfg.send = fake_send; cfg.od_read = fake_od_read; cfg.od_write = fake_od_write;
    cfg.on_reset = fake_reset;
    s_now = 1000;
    cap_reset();
    co_init(&s_node, &cfg, s_now);
}

static void tick(uint32_t dt) { s_now += dt; co_tick(&s_node, s_now); }

static void rx(uint16_t id, const uint8_t *d, uint8_t dlc)
{
    co_frame_t f; memset(&f, 0, sizeof f);
    f.id = id; f.dlc = dlc; if (d) memcpy(f.data, d, dlc);
    co_rx(&s_node, &f, s_now);
}
static void rx8(uint16_t id, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3,
                uint8_t b4, uint8_t b5, uint8_t b6, uint8_t b7)
{
    uint8_t d[8] = {b0, b1, b2, b3, b4, b5, b6, b7};
    rx(id, d, 8);
}
static void nmt(uint8_t cmd) { uint8_t d[2] = {cmd, NODE_ID}; rx(0x000, d, 2); }

/* SDO helpers (client side) */
#define SDO_TX (0x580 + NODE_ID)
#define SDO_RX (0x600 + NODE_ID)
static void sdo_upload_req(uint16_t idx, uint8_t sub)
{
    rx8(SDO_RX, 0x40, (uint8_t)idx, (uint8_t)(idx >> 8), sub, 0, 0, 0, 0);
}
static void sdo_download_exp(uint16_t idx, uint8_t sub, const uint8_t *v, uint8_t n)
{
    uint8_t d[8] = {(uint8_t)(0x23 | ((4 - n) << 2)), (uint8_t)idx, (uint8_t)(idx >> 8), sub, 0, 0, 0, 0};
    memcpy(&d[4], v, n);
    rx(SDO_RX, d, 8);
}
static uint32_t last_abort(void)
{
    const co_frame_t *f = last();
    CHECK(f && f->id == SDO_TX && f->data[0] == 0x80, "expected SDO abort");
    return co_get_u32(&f->data[4]);
}

/* ------------------------------------------------------------------------ */
static void test_bootup_and_heartbeat(void)
{
    node_setup(1000);
    CHECK(co_nmt_state(&s_node) == CO_NMT_PREOPERATIONAL, "pre-op after init");
    CHECK(s_ncap == 0, "nothing sent before the first tick");
    tick(0);
    CHECK(s_ncap == 1 && last()->id == 0x700 + NODE_ID && last()->dlc == 1 &&
          last()->data[0] == 0x00, "boot-up message 0x700+id / 0x00");
    tick(999);
    CHECK(s_ncap == 1, "no heartbeat before the period elapsed");
    tick(1);
    CHECK(s_ncap == 2 && last()->id == 0x700 + NODE_ID && last()->data[0] == 0x7F,
          "heartbeat pre-operational = 0x7F");
    nmt(0x01);
    tick(1000);
    CHECK(last()->data[0] == 0x05, "heartbeat operational = 0x05");
    nmt(0x02);
    tick(1000);
    CHECK(last()->data[0] == 0x04, "heartbeat stopped = 0x04");

    /* boot-up is retried while the driver refuses to send */
    node_setup(0);
    s_send_fail = true; tick(0); tick(10);
    CHECK(s_ncap == 0, "nothing captured while tx fails");
    s_send_fail = false; tick(10);
    CHECK(s_ncap == 1 && last()->data[0] == 0x00, "boot-up sent once tx works");
    tick(5000);
    CHECK(s_ncap == 1, "heartbeat 0 = producer off");
}

static void test_sdo_expedited(void)
{
    node_setup(0); tick(0); cap_reset();
    s_u8 = 0xAB; s_u16 = 0xBEEF; s_u32 = 0xDEADBEEF;

    sdo_upload_req(0x2000, 0);
    CHECK(s_ncap == 1 && last()->id == SDO_TX, "upload response on 0x580+id");
    CHECK(last()->data[0] == 0x4F && last()->data[1] == 0x00 && last()->data[2] == 0x20 &&
          last()->data[3] == 0 && last()->data[4] == 0xAB, "u8 expedited: scs 0x4F");
    sdo_upload_req(0x2001, 0);
    CHECK(last()->data[0] == 0x4B && co_get_u16(&last()->data[4]) == 0xBEEF, "u16 expedited: 0x4B");
    sdo_upload_req(0x2002, 0);
    CHECK(last()->data[0] == 0x43 && co_get_u32(&last()->data[4]) == 0xDEADBEEF, "u32 expedited: 0x43");
    sdo_upload_req(0x2003, 0);
    { float v; memcpy(&v, &last()->data[4], 4); CHECK(last()->data[0] == 0x43 && v == 3.25f, "f32 raw IEEE bytes"); }

    /* communication profile */
    sdo_upload_req(0x1000, 0);
    CHECK(co_get_u32(&last()->data[4]) == 0x000F0191u, "0x1000 device type");
    sdo_upload_req(0x1018, 0);
    CHECK(last()->data[0] == 0x4F && last()->data[4] == 4, "0x1018 sub0 = 4");
    sdo_upload_req(0x1018, 4);
    CHECK(co_get_u32(&last()->data[4]) == 3, "0x1018 sub4 serial");
    sdo_upload_req(0x1200, 2);
    CHECK(co_get_u32(&last()->data[4]) == 0x580 + NODE_ID, "0x1200 sub2 = SDO tx cob");
    sdo_upload_req(0x1014, 0);
    CHECK(co_get_u32(&last()->data[4]) == 0x80 + NODE_ID, "0x1014 EMCY cob");

    /* expedited download */
    uint8_t v16[2] = {0x34, 0x12};
    sdo_download_exp(0x2001, 0, v16, 2);
    CHECK(last()->data[0] == 0x60 && last()->data[1] == 0x01 && last()->data[2] == 0x20,
          "download response 0x60 + index echo");
    CHECK(s_u16 == 0x1234, "u16 written");
    uint8_t v8 = 7;
    sdo_download_exp(0x2000, 0, &v8, 1);
    CHECK(s_u8 == 7, "u8 written");
    sdo_download_exp(0x2006, 0, &v8, 1);
    CHECK(s_wo == 7 && last()->data[0] == 0x60, "write-only object writable");

    /* aborts */
    sdo_download_exp(0x2002, 0, v16, 2);
    CHECK(last_abort() == CO_ABORT_READ_ONLY, "write RO -> 0x06010002");
    sdo_upload_req(0x2006, 0);
    CHECK(last_abort() == CO_ABORT_WRITE_ONLY, "read WO -> 0x06010001");
    sdo_upload_req(0x2999, 0);
    CHECK(last_abort() == CO_ABORT_NO_OBJECT, "unknown index -> 0x06020000");
    sdo_upload_req(0x1018, 9);
    CHECK(last_abort() == CO_ABORT_NO_SUBINDEX, "unknown sub -> 0x06090011");
    sdo_download_exp(0x2000, 0, v16, 2);
    CHECK(last_abort() == CO_ABORT_TYPE_MISMATCH, "wrong length -> 0x06070010");
    sdo_download_exp(0x1000, 0, v16, 2);
    CHECK(last_abort() == CO_ABORT_READ_ONLY, "0x1000 is read-only");
    rx8(SDO_RX, 0xC0, 0x00, 0x20, 0, 0, 0, 0, 0);          /* block upload */
    CHECK(last_abort() == CO_ABORT_CMD, "block transfer -> 0x05040001");
    rx8(SDO_RX, 0x60, 0, 0, 0, 0, 0, 0, 0);                /* upload segment w/o transfer */
    CHECK(last_abort() == CO_ABORT_CMD, "segment without initiate -> 0x05040001");

    /* heartbeat time writable via 0x1017 */
    uint8_t hb[2] = {0xF4, 0x01};                          /* 500 ms */
    sdo_download_exp(0x1017, 0, hb, 2);
    CHECK(last()->data[0] == 0x60, "0x1017 write ok");
    cap_reset(); tick(499); CHECK(s_ncap == 0, "no hb yet"); tick(1);
    CHECK(s_ncap == 1 && last()->id == 0x700 + NODE_ID, "heartbeat now every 500 ms");
    CHECK(s_node.stats.sdo_aborts == 8, "abort counter");
}

static void test_sdo_segmented(void)
{
    node_setup(0); tick(0); cap_reset();
    strcpy(s_str, "Fountainer-Prototype");                 /* 20 bytes */

    sdo_upload_req(0x2004, 0);
    CHECK(last()->data[0] == 0x41 && co_get_u32(&last()->data[4]) == 20,
          "segmented initiate: 0x41 + size");
    rx8(SDO_RX, 0x60, 0, 0, 0, 0, 0, 0, 0);                /* toggle 0 */
    CHECK(last()->data[0] == 0x00 && memcmp(&last()->data[1], "Fountai", 7) == 0, "segment 1 (t=0, 7 bytes)");
    rx8(SDO_RX, 0x70, 0, 0, 0, 0, 0, 0, 0);                /* toggle 1 */
    CHECK(last()->data[0] == 0x10 && memcmp(&last()->data[1], "ner-Pro", 7) == 0, "segment 2 (t=1)");
    rx8(SDO_RX, 0x60, 0, 0, 0, 0, 0, 0, 0);                /* toggle 0, last: 6 bytes */
    CHECK(last()->data[0] == (0x00 | (1 << 1) | 1) && memcmp(&last()->data[1], "totype", 6) == 0,
          "segment 3: n=1 unused, c=1");
    CHECK(s_node.sdo.state == 0, "transfer finished");

    /* toggle error */
    sdo_upload_req(0x2004, 0);
    rx8(SDO_RX, 0x70, 0, 0, 0, 0, 0, 0, 0);                /* wrong: must start with 0 */
    CHECK(last_abort() == CO_ABORT_TOGGLE, "toggle mismatch -> 0x05030000");

    /* segmented download of 10 bytes: "HelloWorld" */
    rx8(SDO_RX, 0x21, 0x04, 0x20, 0, 10, 0, 0, 0);         /* initiate, size = 10 */
    CHECK(last()->data[0] == 0x60, "download initiate ack");
    rx8(SDO_RX, 0x00, 'H', 'e', 'l', 'l', 'o', 'W', 'o');   /* t=0, 7 bytes, not last */
    CHECK(last()->data[0] == 0x20, "segment ack t=0");
    rx8(SDO_RX, 0x10 | (4 << 1) | 1, 'r', 'l', 'd', 0, 0, 0, 0);   /* t=1, n=4 unused, last */
    CHECK(last()->data[0] == 0x30, "segment ack t=1");
    CHECK(strcmp(s_str, "HelloWorld") == 0, "string written via segmented download");

    /* size mismatch -> abort */
    rx8(SDO_RX, 0x21, 0x04, 0x20, 0, 3, 0, 0, 0);
    rx8(SDO_RX, 0x00 | (6 << 1) | 1, 'X', 0, 0, 0, 0, 0, 0);   /* 1 byte, last */
    CHECK(last_abort() == CO_ABORT_TYPE_MISMATCH, "declared 3, got 1 -> mismatch");
    CHECK(strcmp(s_str, "HelloWorld") == 0, "OD untouched after abort");

    /* oversize -> out of memory */
    rx8(SDO_RX, 0x21, 0x04, 0x20, 0, 0xFF, 0xFF, 0, 0);
    CHECK(last_abort() == CO_ABORT_OUT_OF_MEMORY, "65535 bytes -> 0x05040005");

    /* timeout of a stalled transfer */
    sdo_upload_req(0x2004, 0);
    cap_reset(); tick(900); CHECK(s_ncap == 0, "still waiting");
    tick(200);
    CHECK(last_abort() == CO_ABORT_TIMEOUT, "stalled transfer aborted after 1 s");

    /* long string through the 128-byte buffer limit is rejected cleanly */
    for (int i = 0; i < 31; i++) s_str[i] = 'a';
    s_str[31] = 0;
    sdo_upload_req(0x2004, 0);
    CHECK(last()->data[0] == 0x41 && co_get_u32(&last()->data[4]) == 31, "31-byte upload initiate");
}

static void test_nmt(void)
{
    node_setup(0); tick(0); cap_reset();
    CHECK(s_resets == 0, "co_init does not call on_reset");
    nmt(0x01); CHECK(co_nmt_state(&s_node) == CO_NMT_OPERATIONAL, "start");
    nmt(0x80); CHECK(co_nmt_state(&s_node) == CO_NMT_PREOPERATIONAL, "pre-op");
    nmt(0x02); CHECK(co_nmt_state(&s_node) == CO_NMT_STOPPED, "stop");
    sdo_upload_req(0x2000, 0);
    CHECK(s_ncap == 0, "SDO ignored while stopped");
    { uint8_t d[2] = {0x01, 0x00}; rx(0x000, d, 2); }        /* broadcast start */
    CHECK(co_nmt_state(&s_node) == CO_NMT_OPERATIONAL, "broadcast node id 0 accepted");
    { uint8_t d[2] = {0x02, NODE_ID + 1}; rx(0x000, d, 2); }
    CHECK(co_nmt_state(&s_node) == CO_NMT_OPERATIONAL, "other node id ignored");

    uint32_t map[1] = {0x20000008u};
    CHECK(co_tpdo_configure(&s_node, 0, 0, 255, 0, 0, map, 1), "tpdo cfg");
    s_node.error_register = 0x81;
    nmt(0x82);                                                /* reset comm */
    CHECK(s_resets == 1 && s_reset_node == 0, "on_reset(comm) called");
    CHECK(co_nmt_state(&s_node) == CO_NMT_PREOPERATIONAL, "pre-op after reset comm");
    CHECK(s_node.tpdo[0].map_count == 0 && (s_node.tpdo[0].cob_id & 0x80000000u), "PDOs back to disabled");
    CHECK(s_node.error_register == 0x81, "error register kept on reset comm");
    cap_reset(); tick(0);
    CHECK(s_ncap == 1 && last()->id == 0x700 + NODE_ID && last()->data[0] == 0, "boot-up after reset");
    nmt(0x81);
    CHECK(s_resets == 2 && s_reset_node == 1 && s_node.error_register == 0, "reset node clears error register");
}

static void test_tpdo_event_driven(void)
{
    node_setup(0); tick(0); cap_reset();
    uint32_t map[3] = {0x20000008u, 0x20010010u, 0x20020020u};
    CHECK(co_tpdo_configure(&s_node, 0, 0, 255, 100, 1000, map, 3), "TPDO1 u8+u16+u32");
    uint32_t bad[1] = {0x20000010u};                          /* u8 mapped as 16 bit */
    CHECK(!co_tpdo_configure(&s_node, 1, 0, 255, 0, 0, bad, 1), "length mismatch rejected");
    uint32_t big[3] = {0x20050040u, 0x20000008u, 0};
    CHECK(!co_tpdo_configure(&s_node, 1, 0, 255, 0, 0, big, 2), "> 64 bit rejected");

    s_u8 = 1; s_u16 = 0x0202; s_u32 = 0x03030303;
    tick(10); CHECK(s_ncap == 0, "no PDO in pre-operational");
    nmt(0x01);
    tick(10);
    CHECK(s_ncap == 1 && last()->id == 0x180 + NODE_ID && last()->dlc == 7, "first TPDO1 right after start");
    CHECK(last()->data[0] == 1 && co_get_u16(&last()->data[1]) == 0x0202 &&
          co_get_u32(&last()->data[3]) == 0x03030303, "packed little-endian in mapping order");
    tick(10); tick(10); CHECK(s_ncap == 1, "unchanged -> silent");
    s_u8 = 2; tick(10);
    CHECK(s_ncap == 1, "change inside inhibit time held back");
    for (int i = 0; i < 8; i++) tick(10);                     /* 100 ms since tx */
    CHECK(s_ncap == 2 && last()->data[0] == 2, "sent after inhibit elapsed");
    {
        uint32_t t_tx = s_node.tpdo[0].last_tx_ms;
        while (s_now + 10 - t_tx < 1000) tick(10);
        CHECK(s_ncap == 2, "990 ms after the last tx: timer not yet");
        tick(10);
        CHECK(s_ncap == 3 && s_now - t_tx == 1000, "event timer 1000 ms fires without change");
    }
    /* RTR */
    { co_frame_t f; memset(&f, 0, sizeof f); f.id = 0x180 + NODE_ID; f.rtr = true; co_rx(&s_node, &f, s_now); }
    CHECK(s_ncap == 4 && !last()->rtr && last()->dlc == 7, "RTR answered with data");
    /* explicit request */
    co_tpdo_request(&s_node, 0); tick(10);
    CHECK(s_ncap == 5, "co_tpdo_request forces a transmission");
    CHECK(s_node.stats.pdo_tx == 5, "pdo_tx stat");
    nmt(0x80); s_u8 = 9; for (int i = 0; i < 20; i++) tick(10);
    CHECK(s_ncap == 5, "no PDO after pre-op");
}

static void test_tpdo_sdo_reconfig(void)
{
    node_setup(0); tick(0); cap_reset();
    uint8_t zero = 0, one = 1, two = 2;
    uint8_t m1[4], m2[4];
    co_put_u32(m1, 0x20030020u);                              /* f32 */
    co_put_u32(m2, 0x20000008u);                              /* u8  */
    sdo_download_exp(0x1A01, 1, m1, 4);
    CHECK(last()->data[0] == 0x60, "map entry writable while count = 0");
    sdo_download_exp(0x1A01, 2, m2, 4);
    sdo_download_exp(0x1A01, 0, &two, 1);
    CHECK(last()->data[0] == 0x60 && s_node.tpdo[1].map_count == 2, "count = 2 accepted");
    sdo_download_exp(0x1A01, 1, m2, 4);
    CHECK(last_abort() == CO_ABORT_UNSUPPORTED, "entry write while mapped -> 0x06010000");
    sdo_download_exp(0x1A01, 0, &zero, 1);
    uint8_t m3[4]; co_put_u32(m3, 0x29990008u);
    sdo_download_exp(0x1A01, 1, m3, 4);
    sdo_download_exp(0x1A01, 0, &one, 1);
    CHECK(last_abort() == CO_ABORT_PDO_MAP, "unknown object in map -> 0x06040041");
    uint8_t m4[4]; co_put_u32(m4, 0x20050040u);
    sdo_download_exp(0x1A01, 1, m4, 4);
    co_put_u32(m4, 0x20000008u);
    sdo_download_exp(0x1A01, 2, m4, 4);
    sdo_download_exp(0x1A01, 0, &two, 1);
    CHECK(last_abort() == CO_ABORT_PDO_LENGTH, "72 bit -> 0x06040042");
    /* restore a valid 2-entry mapping, enable and configure comm params */
    sdo_download_exp(0x1A01, 1, m1, 4);
    sdo_download_exp(0x1A01, 2, m2, 4);
    sdo_download_exp(0x1A01, 0, &two, 1);
    uint8_t cob[4]; co_put_u32(cob, 0x280 + NODE_ID);        /* valid bit cleared */
    sdo_download_exp(0x1801, 1, cob, 4);
    CHECK(last()->data[0] == 0x60 && !(s_node.tpdo[1].cob_id & 0x80000000u), "COB-ID enabled");
    co_put_u32(cob, 0x285);
    sdo_download_exp(0x1801, 1, cob, 4);
    CHECK(last_abort() == CO_ABORT_PARAM_INCOMPAT, "ID change while valid rejected");
    uint8_t tt = 254; sdo_download_exp(0x1801, 2, &tt, 1);
    uint8_t ev[2] = {0xC8, 0x00}; sdo_download_exp(0x1801, 5, ev, 2);   /* 200 ms */
    sdo_upload_req(0x1801, 5); CHECK(co_get_u16(&last()->data[4]) == 200, "event timer readback");
    sdo_upload_req(0x1A01, 0); CHECK(last()->data[4] == 2, "map count readback");
    sdo_upload_req(0x1A01, 1); CHECK(co_get_u32(&last()->data[4]) == 0x20030020u, "map entry readback");
    nmt(0x01); cap_reset(); s_f32 = 1.5f; tick(10);
    CHECK(s_ncap == 1 && last()->id == 0x280 + NODE_ID && last()->dlc == 5, "TPDO2 f32+u8 = 5 bytes");
    { float v; memcpy(&v, last()->data, 4); CHECK(v == 1.5f && last()->data[4] == s_u8, "TPDO2 content"); }
}

static void test_sync_pdos(void)
{
    node_setup(0); tick(0); cap_reset();
    uint32_t map[1] = {0x20010010u};
    CHECK(co_tpdo_configure(&s_node, 2, 0, 2, 0, 0, map, 1), "TPDO3 every 2nd SYNC");
    uint32_t map0[1] = {0x20000008u};
    CHECK(co_tpdo_configure(&s_node, 3, 0, 0, 0, 0, map0, 1), "TPDO4 acyclic sync");
    nmt(0x01); cap_reset();
    for (int i = 0; i < 5; i++) tick(10);
    CHECK(s_ncap == 0, "sync PDOs never fire on the timer");
    rx(0x080, NULL, 0);
    CHECK(s_ncap == 1 && last()->id == 0x480 + NODE_ID, "acyclic TPDO4 on first SYNC (initial)");
    rx(0x080, NULL, 0);
    CHECK(s_ncap == 2 && last()->id == 0x380 + NODE_ID && last()->dlc == 2, "TPDO3 on 2nd SYNC");
    rx(0x080, NULL, 0); rx(0x080, NULL, 0);
    CHECK(s_ncap == 3 && last()->id == 0x380 + NODE_ID, "TPDO3 again on 4th, TPDO4 silent (unchanged)");
    s_u8 ^= 0xFF; rx(0x080, NULL, 0);
    CHECK(s_ncap == 4 && last()->id == 0x480 + NODE_ID, "TPDO4 on SYNC after a change");

    /* RPDO: immediate (255) and synchronous (1) */
    uint32_t rmap[2] = {0x20000008u, 0x20010010u};
    CHECK(co_rpdo_configure(&s_node, 0, 0, 255, rmap, 2), "RPDO1 u8+u16");
    uint32_t rmap2[1] = {0x20030020u};
    CHECK(co_rpdo_configure(&s_node, 1, 0, 1, rmap2, 1), "RPDO2 sync f32");
    uint32_t badr[1] = {0x10170010u};
    CHECK(!co_rpdo_configure(&s_node, 1, 0, 1, badr, 1), "comm object not RPDO-mappable");
    cap_reset();
    { uint8_t d[3] = {0x55, 0xAD, 0xDE}; rx(0x200 + NODE_ID, d, 3); }
    CHECK(s_u8 == 0x55 && s_u16 == 0xDEAD, "RPDO1 applied immediately");
    { float v = -2.5f; uint8_t d[4]; memcpy(d, &v, 4); rx(0x300 + NODE_ID, d, 4); }
    CHECK(s_f32 != -2.5f, "sync RPDO held until SYNC");
    rx(0x080, NULL, 0);
    CHECK(s_f32 == -2.5f, "sync RPDO applied on SYNC");
    cap_reset();
    { uint8_t d[2] = {0x01, 0x02}; rx(0x200 + NODE_ID, d, 2); }  /* too short */
    CHECK(s_ncap == 1 && last()->id == 0x80 + NODE_ID && co_get_u16(last()->data) == 0x8210,
          "short RPDO -> EMCY 0x8210");
    CHECK(s_u8 == 0x55, "short RPDO not applied");
    nmt(0x80);
    { uint8_t d[3] = {0x66, 0, 0}; rx(0x200 + NODE_ID, d, 3); }
    CHECK(s_u8 == 0x55, "RPDO ignored in pre-op");
    CHECK(s_node.stats.pdo_rx == 2, "pdo_rx stat");
}

static void test_emcy(void)
{
    node_setup(0); tick(0); cap_reset();
    uint8_t mfr[5] = {4, 0, 0, 0, 0};
    co_emcy(&s_node, 0x1000, CO_ERR_GENERIC | CO_ERR_DEVICE_SPECIFIC, mfr);
    CHECK(s_ncap == 1 && last()->id == 0x80 + NODE_ID && last()->dlc == 8, "EMCY frame");
    CHECK(co_get_u16(last()->data) == 0x1000 && last()->data[2] == 0x81 && last()->data[3] == 4,
          "EMCY: code, error register, manufacturer bytes");
    sdo_upload_req(0x1001, 0);
    CHECK(last()->data[0] == 0x4F && last()->data[4] == 0x81, "0x1001 mirrors the error register");
    co_emcy(&s_node, 0x0000, CO_ERR_GENERIC | CO_ERR_DEVICE_SPECIFIC, NULL);
    CHECK(last()->data[2] == 0x00 && co_get_u16(last()->data) == 0, "error reset clears bits");
    nmt(0x02); cap_reset();
    co_emcy(&s_node, 0x1000, CO_ERR_GENERIC, NULL);
    CHECK(s_ncap == 0 && s_node.error_register == 1, "no EMCY while stopped, register still set");
    CHECK(s_node.stats.emcy_tx == 2, "emcy stat");
}

static void test_master_presence(void)
{
    node_setup(0); tick(0);
    CHECK(!co_master_active(&s_node, s_now, 5000), "no master before any frame");
    rx8(0x123, 0, 0, 0, 0, 0, 0, 0, 0);                        /* foreign frame */
    CHECK(!co_master_active(&s_node, s_now, 5000), "foreign frame is not a master");
    { uint8_t d[2] = {0x01, NODE_ID + 1}; rx(0x000, d, 2); }   /* NMT for another node */
    CHECK(!co_master_active(&s_node, s_now, 5000), "NMT for another node ignored");
    sdo_upload_req(0x1000, 0);
    CHECK(co_master_active(&s_node, s_now, 5000), "SDO request marks the master active");
    CHECK(s_node.last_master_ms == s_now, "timestamp of the last master frame");
    tick(4999); CHECK(co_master_active(&s_node, s_now, 5000), "still active at 4999 ms");
    tick(2);    CHECK(!co_master_active(&s_node, s_now, 5000), "lost after the timeout");
    nmt(0x01);  CHECK(co_master_active(&s_node, s_now, 5000), "NMT start refreshes");
    tick(6000); rx(0x080, NULL, 0);
    CHECK(co_master_active(&s_node, s_now, 5000), "SYNC refreshes");
    tick(6000);
    { uint8_t d[2] = {0x01, 0x02}; rx(0x200 + NODE_ID, d, 2); }   /* RPDO1 not configured */
    CHECK(!co_master_active(&s_node, s_now, 5000), "unmapped RPDO id does not count");
}

static void test_stats_and_misc(void)
{
    node_setup(0); tick(0);
    CHECK(s_node.stats.tx_frames == 1 && s_node.stats.rx_frames == 0, "tx stat");
    rx8(0x123, 0, 0, 0, 0, 0, 0, 0, 0);                        /* foreign frame */
    CHECK(s_node.stats.rx_frames == 1 && s_ncap == 1, "foreign frame counted, ignored");
    rx8(SDO_RX, 0x40, 0, 0x20, 0, 0, 0, 0, 0);
    CHECK(s_node.stats.sdo_requests == 1, "sdo stat");
    { co_frame_t f; memset(&f, 0, sizeof f); f.id = SDO_RX; f.dlc = 4; co_rx(&s_node, &f, s_now); }
    CHECK(s_node.stats.sdo_requests == 1, "short SDO frame ignored");
    co_config_t cfg = s_node.cfg; cfg.node_id = 0;
    co_init(&s_node, &cfg, 0); cap_reset();
    CHECK(s_node.cfg.node_id == 127, "node id 0 falls back to 127");
    sdo_upload_req(0x1008, 0);
    CHECK(s_ncap == 0, "SDO for node 3 ignored by node 127");
    co_heartbeat_set(&s_node, 250); cap_reset(); tick(0); cap_reset(); tick(250);
    CHECK(s_ncap == 1 && last()->id == 0x700 + 127, "heartbeat on node 127");
}

int main(void)
{
    test_bootup_and_heartbeat();
    test_sdo_expedited();
    test_sdo_segmented();
    test_nmt();
    test_tpdo_event_driven();
    test_tpdo_sdo_reconfig();
    test_sync_pdos();
    test_emcy();
    test_master_presence();
    test_stats_and_misc();
    printf("test_co_core: OK (%d checks)\n", s_checks);
    return 0;
}
