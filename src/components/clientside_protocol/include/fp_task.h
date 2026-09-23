/*
 * Copyright (c) 2026 Melowsyne Unipessoal Lda. All rights reserved.
 *
 * This source code is proprietary. No license is granted to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of
 * this software without prior written permission from the copyright holder.
 *
 * For licensing inquiries, contact: info@melowsyne.com
 */

/* fp_task — FreeRTOS communication task: wires up fp_ws + fp_session and
 * drives periodic heartbeat/dp_report. ESP-IDF-specific.
 * The public entry point is fountain_proto_start() (see fountain_proto.h). */
#pragma once
#include "fountain_proto.h"
