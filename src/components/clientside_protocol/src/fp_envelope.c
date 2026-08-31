/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "fp_envelope.h"

cJSON *fp_envelope_build(fp_msg_type_t type, cJSON *body,
                         const char *serial, const char *msg_id,
                         const char *in_reply_to, int64_t ts) {
    const fp_msg_meta_t *meta = fp_msg_meta(type);
    if (!meta) { cJSON_Delete(body); return NULL; }

    cJSON *msg = cJSON_CreateObject();
    if (!msg) { cJSON_Delete(body); return NULL; }
    cJSON_AddNumberToObject(msg, "v", meta->wire);
    cJSON_AddStringToObject(msg, "type", meta->name);
    cJSON_AddNumberToObject(msg, "ts", (double)ts);
    if (serial)      cJSON_AddStringToObject(msg, "serial", serial);
    if (msg_id)      cJSON_AddStringToObject(msg, "msg_id", msg_id);
    if (in_reply_to) cJSON_AddStringToObject(msg, "in_reply_to", in_reply_to);

    /* Move the body fields into the message (relocate). */
    if (body) {
        cJSON *child = body->child;
        while (child) {
            cJSON *next = child->next;
            cJSON_DetachItemViaPointer(body, child);
            cJSON_AddItemToObject(msg, child->string, child);
            child = next;
        }
        cJSON_Delete(body);
    }
    return msg;
}

fp_msg_type_t fp_envelope_type(const cJSON *msg) {
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(msg, "type");
    if (!cJSON_IsString(t)) return FP_MSG_COUNT;
    return fp_msg_type_from_name(t->valuestring);
}
