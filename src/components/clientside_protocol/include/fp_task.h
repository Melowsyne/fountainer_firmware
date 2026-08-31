/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 */

/* fp_task — FreeRTOS communication task: wires up fp_ws + fp_session and
 * drives periodic heartbeat/dp_report. ESP-IDF-specific.
 * The public entry point is fountain_proto_start() (see fountain_proto.h). */
#pragma once
#include "fountain_proto.h"
