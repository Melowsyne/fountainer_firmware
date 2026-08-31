/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "fp_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/base64.h"
#include "mbedtls/md.h"

/* ----- dynamic string buffer (portable; no open_memstream) -------- */
typedef struct { char *p; size_t len, cap; } sb_t;

static bool sb_init(sb_t *s) {
    s->cap = 128; s->len = 0;
    s->p = malloc(s->cap);
    if (!s->p) return false;
    s->p[0] = '\0';
    return true;
}
static bool sb_putn(sb_t *s, const char *d, size_t n) {
    if (s->len + n + 1 > s->cap) {
        size_t nc = s->cap * 2;
        while (nc < s->len + n + 1) nc *= 2;
        char *np = realloc(s->p, nc);
        if (!np) return false;
        s->p = np; s->cap = nc;
    }
    memcpy(s->p + s->len, d, n);
    s->len += n; s->p[s->len] = '\0';
    return true;
}
static bool sb_putc(sb_t *s, char c) { return sb_putn(s, &c, 1); }
static bool sb_puts(sb_t *s, const char *z) { return sb_putn(s, z, strlen(z)); }

/* ----- mbedTLS helpers --------------------------------------------------- */
static void md_sha256(const uint8_t *in, size_t n, uint8_t out[32]) {
    const mbedtls_md_info_t *mi = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md(mi, in, n, out);
}
static void md_hmac(const uint8_t *key, size_t kl,
                    const uint8_t *in, size_t n, uint8_t out[32]) {
    const mbedtls_md_info_t *mi = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(mi, key, kl, in, n, out);
}

char *fp_auth_base64_encode(const uint8_t *data, size_t n) {
    size_t olen = 0;
    mbedtls_base64_encode(NULL, 0, &olen, data, n);   /* required size incl. NUL */
    char *out = malloc(olen ? olen : 1);
    if (!out) return NULL;
    if (mbedtls_base64_encode((unsigned char *)out, olen, &olen, data, n) != 0) {
        free(out); return NULL;
    }
    out[olen] = '\0';
    return out;
}

int fp_auth_hex_decode(const char *hex, uint8_t *out, size_t outcap) {
    size_t n = strlen(hex);
    if (n % 2 != 0 || n / 2 > outcap) return -1;
    for (size_t i = 0; i < n; i += 2) {
        unsigned v;
        if (sscanf(hex + i, "%2x", &v) != 1) return -1;
        out[i / 2] = (uint8_t)v;
    }
    return (int)(n / 2);
}

void fp_auth_sha256_hex(const char *data, size_t len, char out[65]) {
    uint8_t d[32];
    md_sha256((const uint8_t *)data, len, d);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", d[i]);
    out[64] = '\0';
}

/* ----- canonical serialization (JCS-like) ------------------------------ */
static void emit_string(sb_t *s, const char *z) {
    sb_putc(s, '"');
    for (const unsigned char *p = (const unsigned char *)z; *p; p++) {
        switch (*p) {
            case '"':  sb_puts(s, "\\\""); break;
            case '\\': sb_puts(s, "\\\\"); break;
            case '\b': sb_puts(s, "\\b");  break;
            case '\f': sb_puts(s, "\\f");  break;
            case '\n': sb_puts(s, "\\n");  break;
            case '\r': sb_puts(s, "\\r");  break;
            case '\t': sb_puts(s, "\\t");  break;
            default:
                if (*p < 0x20) { char b[8]; sprintf(b, "\\u%04x", *p); sb_puts(s, b); }
                else sb_putc(s, (char)*p);
        }
    }
    sb_putc(s, '"');
}

static void emit_number(sb_t *s, const cJSON *n) {
    char b[32];
    double d = n->valuedouble;
    if (d == (double)(long long)d && d >= -9.0e15 && d <= 9.0e15) {
        snprintf(b, sizeof b, "%lld", (long long)d);     /* integer short (JCS) */
    } else {
        /* JCS (RFC 8785) requires the SHORTEST round-trip representation —
         * this is what Python's json.dumps emits on the server side. A
         * fixed %.17g would print 34.47 as "34.469999999999999" and the
         * MACs would diverge (observed live as auth_failed on float
         * dp_writes). Probe 15..17 significant digits, take the first
         * that round-trips. */
        for (int prec = 15; prec <= 17; prec++) {
            snprintf(b, sizeof b, "%.*g", prec, d);
            if (strtod(b, NULL) == d) break;
        }
    }
    sb_puts(s, b);
}

static int cmp_name(const void *a, const void *b) {
    return strcmp((*(const cJSON *const *)a)->string,
                  (*(const cJSON *const *)b)->string);
}

static void canon_write(sb_t *s, const cJSON *n) {
    if (cJSON_IsObject(n)) {
        int cnt = cJSON_GetArraySize(n);
        const cJSON **kids = calloc(cnt > 0 ? cnt : 1, sizeof(*kids));
        int i = 0;
        for (const cJSON *c = n->child; c; c = c->next) kids[i++] = c;
        qsort(kids, cnt, sizeof(*kids), cmp_name);
        sb_putc(s, '{');
        for (i = 0; i < cnt; i++) {
            if (i) sb_putc(s, ',');
            emit_string(s, kids[i]->string);
            sb_putc(s, ':');
            canon_write(s, kids[i]);
        }
        sb_putc(s, '}');
        free(kids);
    } else if (cJSON_IsArray(n)) {
        sb_putc(s, '[');
        int i = 0;
        for (const cJSON *c = n->child; c; c = c->next, i++) {
            if (i) sb_putc(s, ',');
            canon_write(s, c);
        }
        sb_putc(s, ']');
    } else if (cJSON_IsString(n)) {
        emit_string(s, n->valuestring);
    } else if (cJSON_IsNumber(n)) {
        emit_number(s, n);
    } else if (cJSON_IsTrue(n)) {
        sb_puts(s, "true");
    } else if (cJSON_IsFalse(n)) {
        sb_puts(s, "false");
    } else {
        sb_puts(s, "null");
    }
}

char *fp_auth_canon_build(const cJSON *node) {
    sb_t s;
    if (!sb_init(&s)) return NULL;
    canon_write(&s, node);
    return s.p;
}

static const char *ENVELOPE[] = {"v", "type", "serial", "ts",
                                 "msg_id", "in_reply_to", "auth"};

void fp_auth_body_hash_build(const cJSON *msg, char out[65]) {
    cJSON *body = cJSON_Duplicate(msg, 1);
    for (size_t i = 0; i < sizeof(ENVELOPE) / sizeof(ENVELOPE[0]); i++)
        cJSON_DeleteItemFromObject(body, ENVELOPE[i]);
    char *canon = fp_auth_canon_build(body);
    fp_auth_sha256_hex(canon, strlen(canon), out);
    free(canon);
    cJSON_Delete(body);
}

/* ----- MAC input + Sign/Verify ---------------------------------------- */
static char *field_str(const cJSON *msg, const char *key) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(msg, key);
    if (!it) return strdup("");
    if (cJSON_IsString(it)) return strdup(it->valuestring);
    if (cJSON_IsNumber(it)) {
        char b[32];
        snprintf(b, sizeof b, "%lld", (long long)it->valuedouble);
        return strdup(b);
    }
    return strdup("");
}

static char *compute_mac(const uint8_t *key, size_t keylen,
                         const char *v, const char *type, const char *direction,
                         const char *device_id, const char *serial, const char *ts,
                         const char *msg_id, const char *in_reply_to,
                         const char *kid, const char *seq,
                         const char *server_nonce, const char *client_nonce,
                         const char *body_hash) {
    const char *fields[13] = {v, type, direction, device_id, serial, ts, msg_id,
                              in_reply_to, kid, seq, server_nonce, client_nonce,
                              body_hash};
    sb_t s;
    if (!sb_init(&s)) return NULL;
    for (int i = 0; i < 13; i++) {
        if (i) sb_putc(&s, (char)0x1F);
        sb_puts(&s, fields[i] ? fields[i] : "");
    }
    uint8_t mac[32];
    md_hmac(key, keylen, (const uint8_t *)s.p, s.len, mac);
    free(s.p);
    return fp_auth_base64_encode(mac, 16);   /* first 128 bits */
}

bool fp_auth_message_sign(cJSON *msg, const uint8_t *key, size_t keylen,
                          const char *kid, long seq, const char *direction,
                          const char *device_id, const char *server_nonce,
                          const char *client_nonce) {
    char bhash[65];
    fp_auth_body_hash_build(msg, bhash);
    char *vs = field_str(msg, "v"), *type = field_str(msg, "type");
    char *serial = field_str(msg, "serial"), *ts = field_str(msg, "ts");
    char *msg_id = field_str(msg, "msg_id"), *irt = field_str(msg, "in_reply_to");
    char seqs[24];
    snprintf(seqs, sizeof seqs, "%ld", seq);

    char *mac = compute_mac(key, keylen, vs, type, direction, device_id, serial,
                            ts, msg_id, irt, kid, seqs, server_nonce,
                            client_nonce ? client_nonce : "", bhash);
    bool ok = false;
    if (mac) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "kid", kid);
        cJSON_AddNumberToObject(a, "seq", (double)seq);
        cJSON_AddStringToObject(a, "mac", mac);
        cJSON_DeleteItemFromObject(msg, "auth");
        cJSON_AddItemToObject(msg, "auth", a);
        ok = true;
    }
    free(mac); free(vs); free(type); free(serial);
    free(ts); free(msg_id); free(irt);
    return ok;
}

int fp_auth_message_verify(const cJSON *msg, const uint8_t *key, size_t keylen,
                           const char *expected_kid, const char *direction,
                           const char *device_id, const char *server_nonce,
                           const char *client_nonce, const char **reason) {
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(msg, "auth");
    if (!cJSON_IsObject(a)) { *reason = "missing_auth"; return 0; }
    const cJSON *kid = cJSON_GetObjectItemCaseSensitive(a, "kid");
    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(a, "seq");
    const cJSON *mac = cJSON_GetObjectItemCaseSensitive(a, "mac");
    if (!cJSON_IsString(kid) || strcmp(kid->valuestring, expected_kid) != 0) {
        *reason = "kid_mismatch"; return 0;
    }
    if (!cJSON_IsNumber(seq)) { *reason = "bad_seq"; return 0; }
    if (!cJSON_IsString(mac)) { *reason = "bad_mac"; return 0; }

    char bhash[65];
    fp_auth_body_hash_build(msg, bhash);
    char *vs = field_str(msg, "v"), *type = field_str(msg, "type");
    char *serial = field_str(msg, "serial"), *ts = field_str(msg, "ts");
    char *msg_id = field_str(msg, "msg_id"), *irt = field_str(msg, "in_reply_to");
    char seqs[24];
    snprintf(seqs, sizeof seqs, "%lld", (long long)seq->valuedouble);

    char *want = compute_mac(key, keylen, vs, type, direction, device_id, serial,
                             ts, msg_id, irt, kid->valuestring, seqs, server_nonce,
                             client_nonce ? client_nonce : "", bhash);
    int ok = (want && strcmp(want, mac->valuestring) == 0);
    if (!ok) *reason = "mac_mismatch";
    free(want); free(vs); free(type); free(serial);
    free(ts); free(msg_id); free(irt);
    return ok;
}

bool fp_replay_check(fp_replay_t *r, long seq) {
    if (seq <= r->last) return false;
    r->last = seq;
    return true;
}
