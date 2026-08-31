/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

/* fp_envelope — body (generated) <-> complete wire message.
 * Pure C + cJSON (host-compilable, no ESP-IDF dependency). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "fountain_msgs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Builds a complete message: envelope (v from meta, type, ts, optional
 * serial/msg_id/in_reply_to) + the fields from `body`. `body` is CONSUMED
 * (its children are moved over, body itself deleted). Omit NULL strings.
 * Return value is the new message (caller frees it via cJSON). */
cJSON *fp_envelope_build(fp_msg_type_t type, cJSON *body,
                         const char *serial, const char *msg_id,
                         const char *in_reply_to, int64_t ts);

/* Returns the message type of a received message (FP_MSG_COUNT = unknown). */
fp_msg_type_t fp_envelope_type(const cJSON *msg);

#ifdef __cplusplus
}
#endif
