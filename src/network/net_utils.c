/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

#include "net_utils.h"
#include "debug.h"

#include <string.h>
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#define TAG "net_util"

bool net_util_ip4_from_str(const char *pstrLiteral, esp_ip4_addr_t *pstOut)
{
    if (!pstrLiteral || !pstrLiteral[0] || !pstOut) return false;
    return esp_netif_str_to_ip4(pstrLiteral, pstOut) == ESP_OK;
}

bool net_util_resolve_ip4(const char *pstrHostOrIp, esp_ip4_addr_t *pstOut)
{
    if (!pstrHostOrIp || !pstrHostOrIp[0] || !pstOut) return false;

    /* Fast path: dotted-quad literal (works without any network). */
    if (net_util_ip4_from_str(pstrHostOrIp, pstOut)) return true;

    /* DNS name -> lwIP resolver (requires connectivity + configured DNS). */
    const struct addrinfo stHints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *pstRes = NULL;
    int slErr = getaddrinfo(pstrHostOrIp, NULL, &stHints, &pstRes);
    if (slErr != 0 || !pstRes) {
        logging(LOG_TARGET_AUTO, DBG_LVL_LOW, TAG,
                "DNS resolve failed for '%s' (err=%d)", pstrHostOrIp, slErr);
        return false;
    }
    const struct sockaddr_in *pstSa = (const struct sockaddr_in *)pstRes->ai_addr;
    pstOut->addr = pstSa->sin_addr.s_addr;
    freeaddrinfo(pstRes);

    logging(LOG_TARGET_AUTO, DBG_LVL_MEDIUM, TAG, "resolved '%s' -> " IPSTR,
            pstrHostOrIp, IP2STR(pstOut));
    return true;
}
