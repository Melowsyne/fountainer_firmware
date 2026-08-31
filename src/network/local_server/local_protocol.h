/*
 * Copyright (c) 2026 Sascha G. Eurich, Melowsyne UNIPESSOAL LDA
 * SPDX-License-Identifier: MIT
 *
 * local_protocol — Fountain processing of the local sessions (AP5).
 * Called by the local_server worker; direct reference (NO weak symbols:
 * in static archives the object would otherwise never be linked).
 */
#pragma once

#include "local_session.h"

void local_protocol_on_open(local_session_t *s);
void local_protocol_on_frame(local_session_t *s, const char *json);
void local_protocol_on_close(local_session_t *s);
