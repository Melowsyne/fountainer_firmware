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

/* =============================================================
 * MAIN module — the key/composition module (Modularisation.md §3).
 *
 * Responsibilities:
 *   - initialize all modules in dependency order (app_main)
 *   - wire the core hooks (dependency injection): power_mgmt
 *     providers, debug sinks, the system pre-reboot hook
 *   - start the worker tasks (device: task_measure, network:
 *     task_com) and SUPERVISE them via the monitor task, which
 *     also gathers the System_* datapoints and drives power_mgmt
 *
 * Module hierarchy: main -> {network, device} -> core -> ESP-IDF.
 * Patterns in use: Observer (data_store), State (fountain),
 * Command (command), Facade (task_com), Producer/Consumer (fp TX
 * queue), Strategy/Dependency-Injection (core hooks, this module).
 *
 * No public API — the entry point is ESP-IDF's app_main().
 * ============================================================= */
