/*
 * Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
 *
 * This source code is proprietary. No license is granted to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of
 * this software without prior written permission from the copyright holder.
 *
 * For licensing inquiries, contact: info@melowsyne.com
 */

#pragma once
#include <stdbool.h>
#include "esp_netif.h"

/* =============================================================
 * net_utils — string address helpers.
 * Converts "IP or domain" strings (as stored in the Network_*
 * datapoints) into the binary IPv4 notation used by esp_netif.
 * ============================================================= */

/* Parse a dotted-quad IPv4 literal ("SERVER_IP"). No DNS, no network
 * required — safe to use before the WLAN is up (static IP config). */
bool net_util_ip4_from_str(const char *pstrLiteral, esp_ip4_addr_t *pstOut);

/* Resolve an IPv4 literal OR a DNS name ("server1.server.com").
 * Literals succeed offline; DNS names require an established network
 * connection (lwIP getaddrinfo -> the router's DNS). */
bool net_util_resolve_ip4(const char *pstrHostOrIp, esp_ip4_addr_t *pstOut);
