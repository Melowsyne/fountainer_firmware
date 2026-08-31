/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

/* Flash tier of the logging component (Logging_v1.md, USB milestone):
 *
 *   logstore partition (128 K, data/0x40) = 2 slots x 64 K.
 *   Each boot claims the older/acked slot (erase + header with a monotonic
 *   generation counter); the other slot holds the PREVIOUS boot's records
 *   until the server fetched and acknowledged them (log_read_prev +
 *   log_ack_prev — the ack clears a header flag without erasing).
 *
 *   Persisted: records with level <= Log_Flash_Level (default WARN) plus
 *   the boot/watchdog-diagnosis records regardless of level. The emit path
 *   only enqueues (non-blocking); a low-priority task appends record+CRC32
 *   to the slot. Full slot / full queue -> drop counter, never blocking.
 *
 *   RUNTIME-DETECTED: without the partition every entry point is a no-op
 *   (previous_boot_available = false) — the same firmware runs before and
 *   after the USB-only partition-table extension. */

#include "logging.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

static const char *TAG = "log_flash";

#define LOGF_MAGIC        0x4C4F4753u          /* "LOGS"                    */
#define LOGF_SLOT_SIZE    0x10000u             /* 64 K                      */
#define LOGF_DATA_OFF     64u                  /* records start here        */
#define LOGF_QUEUE_LEN    32
#define LOGF_TASK_STACK   3072
#define LOGF_TASK_PRIO    2

typedef struct {
    uint32_t ulMagic;
    uint32_t ulBootId;
    uint32_t ulGeneration;    /* monotonic across boots (slot choice)       */
    uint32_t ulAcked;         /* 0xFFFFFFFF = pending, 0 = server acked     */
} logf_hdr_t;

typedef struct {
    log_record_t stRec;
    uint32_t     ulCrc;       /* crc32 over stRec                           */
} logf_entry_t;

static const esp_partition_t *s_pstPart;   /* NULL = tier not present      */
static QueueHandle_t s_hQueue;
static uint32_t s_ulWriteOff;              /* next entry offset (own slot) */
static uint32_t s_ulOwnSlotOff;
static uint32_t s_ulPrevSlotOff;
static uint32_t s_ulPrevBootId;
static bool     s_bPrevAvailable;
static volatile uint8_t s_ucFlashLevel = LOG_LEVEL_WARN;
static uint32_t s_ulDroppedFlash;

void logging_set_flash_level(log_level_t eLevel)
{
    if (eLevel > LOG_LEVEL_TRACE) eLevel = LOG_LEVEL_TRACE;
    s_ucFlashLevel = (uint8_t)eLevel;
}

uint32_t logging_flash_dropped_get(void) { return s_ulDroppedFlash; }

static bool hdr_read(uint32_t ulSlotOff, logf_hdr_t *pstHdr)
{
    return esp_partition_read(s_pstPart, ulSlotOff, pstHdr,
                              sizeof(*pstHdr)) == ESP_OK;
}

/* Append task: the only flash writer. */
static void logf_task(void *pvArg)
{
    (void)pvArg;
    logf_entry_t stEntry;
    for (;;) {
        if (xQueueReceive(s_hQueue, &stEntry.stRec, portMAX_DELAY) != pdTRUE)
            continue;
        if (s_ulWriteOff + sizeof(stEntry) > LOGF_SLOT_SIZE) {
            s_ulDroppedFlash++;            /* slot full: RAM ring continues */
            continue;
        }
        stEntry.ulCrc = esp_rom_crc32_le(0, (const uint8_t *)&stEntry.stRec,
                                         sizeof(stEntry.stRec));
        if (esp_partition_write(s_pstPart, s_ulOwnSlotOff + s_ulWriteOff,
                                &stEntry, sizeof(stEntry)) == ESP_OK)
            s_ulWriteOff += sizeof(stEntry);
        else
            s_ulDroppedFlash++;
    }
}

/* Called by logging_emit for every record that passed the runtime gate:
 * persist WARN/ERROR (per Log_Flash_Level) plus boot/wd-diagnosis records.
 * Non-blocking — the control paths must never wait on flash. */
void logging_flash_submit(const log_record_t *pstRec)
{
    if (!s_pstPart || !s_hQueue) return;
    bool bWant = pstRec->ucLevel <= s_ucFlashLevel ||
                 pstRec->usEventId == LOG_EVT_BOOT ||
                 pstRec->usEventId == LOG_EVT_WD_BOOT_DIAG;
    if (!bWant) return;
    if (xQueueSend(s_hQueue, pstRec, 0) != pdTRUE)
        s_ulDroppedFlash++;
}

void logging_flash_init(void)
{
    s_pstPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40,
                                         "logstore");
    if (!s_pstPart) {
        ESP_LOGI(TAG, "no logstore partition — flash tier inactive");
        return;
    }

    log_stats_t stStats;
    logging_stats_get(&stStats);           /* current boot_id */

    /* Slot choice: prefer an invalid or acked slot; otherwise overwrite the
     * OLDER generation (its boot was already fetchable last time). */
    logf_hdr_t astHdr[2];
    bool abValid[2];
    for (int i = 0; i < 2; i++) {
        abValid[i] = hdr_read(i * LOGF_SLOT_SIZE, &astHdr[i]) &&
                     astHdr[i].ulMagic == LOGF_MAGIC;
    }
    int slOwn;
    if      (!abValid[0]) slOwn = 0;
    else if (!abValid[1]) slOwn = 1;
    else if (astHdr[0].ulAcked == 0 && astHdr[1].ulAcked != 0) slOwn = 0;
    else if (astHdr[1].ulAcked == 0 && astHdr[0].ulAcked != 0) slOwn = 1;
    else slOwn = (astHdr[0].ulGeneration <= astHdr[1].ulGeneration) ? 0 : 1;

    int slPrev = 1 - slOwn;
    s_ulOwnSlotOff  = (uint32_t)slOwn  * LOGF_SLOT_SIZE;
    s_ulPrevSlotOff = (uint32_t)slPrev * LOGF_SLOT_SIZE;
    s_bPrevAvailable = abValid[slPrev] && astHdr[slPrev].ulAcked != 0 &&
                       astHdr[slPrev].ulBootId != stStats.ulBootId;
    s_ulPrevBootId   = abValid[slPrev] ? astHdr[slPrev].ulBootId : 0;

    uint32_t ulGen = 1;
    if (abValid[0] && astHdr[0].ulGeneration >= ulGen) ulGen = astHdr[0].ulGeneration + 1;
    if (abValid[1] && astHdr[1].ulGeneration >= ulGen) ulGen = astHdr[1].ulGeneration + 1;

    if (esp_partition_erase_range(s_pstPart, s_ulOwnSlotOff,
                                  LOGF_SLOT_SIZE) != ESP_OK) {
        ESP_LOGE(TAG, "slot erase failed — flash tier inactive");
        s_pstPart = NULL;
        return;
    }
    logf_hdr_t stOwn = { .ulMagic = LOGF_MAGIC, .ulBootId = stStats.ulBootId,
                         .ulGeneration = ulGen, .ulAcked = 0xFFFFFFFFu };
    if (esp_partition_write(s_pstPart, s_ulOwnSlotOff, &stOwn,
                            sizeof(stOwn)) != ESP_OK) {
        ESP_LOGE(TAG, "slot header write failed — flash tier inactive");
        s_pstPart = NULL;
        return;
    }
    s_ulWriteOff = LOGF_DATA_OFF;

    s_hQueue = xQueueCreate(LOGF_QUEUE_LEN, sizeof(log_record_t));
    if (!s_hQueue || xTaskCreate(logf_task, "log_flash", LOGF_TASK_STACK,
                                 NULL, LOGF_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "queue/task setup failed — flash tier inactive");
        s_pstPart = NULL;
        return;
    }
    ESP_LOGI(TAG, "active: slot %d gen %u; previous boot %s (boot_id %u)",
             slOwn, (unsigned)ulGen,
             s_bPrevAvailable ? "AVAILABLE" : "none",
             (unsigned)s_ulPrevBootId);
}

/* ---- previous-boot access (real implementations replace the stubs) ------ */

bool logging_previous_boot_available(uint32_t *pulBootId)
{
    if (pulBootId) *pulBootId = s_bPrevAvailable ? s_ulPrevBootId : 0;
    return s_bPrevAvailable;
}

size_t logging_read_previous_boot(log_level_t eMinLevel,
                                  log_record_t *pstOut, size_t szMax)
{
    if (!s_bPrevAvailable || !s_pstPart || !pstOut || !szMax) return 0;
    if (eMinLevel == LOG_LEVEL_OFF) eMinLevel = LOG_LEVEL_TRACE;

    size_t szN = 0;
    for (uint32_t ulOff = LOGF_DATA_OFF;
         ulOff + sizeof(logf_entry_t) <= LOGF_SLOT_SIZE && szN < szMax;
         ulOff += sizeof(logf_entry_t)) {
        logf_entry_t stEntry;
        if (esp_partition_read(s_pstPart, s_ulPrevSlotOff + ulOff, &stEntry,
                               sizeof(stEntry)) != ESP_OK)
            break;
        if (stEntry.ulCrc != esp_rom_crc32_le(0, (const uint8_t *)&stEntry.stRec,
                                              sizeof(stEntry.stRec)))
            break;                          /* end of valid data (or damage) */
        if (stEntry.stRec.ucLevel > (uint8_t)eMinLevel) continue;
        pstOut[szN++] = stEntry.stRec;
    }
    return szN;
}

bool logging_ack_previous_boot(uint32_t ulBootId)
{
    if (!s_bPrevAvailable || !s_pstPart) return false;
    if (ulBootId != 0 && ulBootId != s_ulPrevBootId) return false;

    /* NOR flash: the ack flag flips 1 -> 0 without an erase. */
    logf_hdr_t stHdr;
    if (!hdr_read(s_ulPrevSlotOff, &stHdr)) return false;
    stHdr.ulAcked = 0;
    if (esp_partition_write(s_pstPart, s_ulPrevSlotOff, &stHdr,
                            sizeof(stHdr)) != ESP_OK)
        return false;
    s_bPrevAvailable = false;
    ESP_LOGI(TAG, "previous boot %u acked — slot reclaimable",
             (unsigned)s_ulPrevBootId);
    return true;
}
