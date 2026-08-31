# DOKU — Fountainer Firmware Reference Documentation

Maintained, presentation-ready project documentation. The firmware version is
whatever [`../version.txt`](../version.txt) says — documents note their own
last-review date where it matters.

## Reference

| Document | Contents |
|---|---|
| [Protocol_Reference.md](Protocol_Reference.md) | Fountain v2.2 on the wire: envelope, full message bodies, session lifecycle with sequence diagram, HMAC auth details, integration surface. |
| [Datapoints.md](Datapoints.md) | The X-macro datapoint engine, report triggers, atomic writes and constraints, complete catalog reference (107 points with NVS ids). |
| [Robustness.md](Robustness.md) | Watchdog cascade, link scoring and Wi-Fi backoff, the network trial-reboot state machine, power management, structured logging. |
| [Development.md](Development.md) | Toolchain, fresh-checkout setup, build scripts, versioning and the stale-build trap, release flow, host tests, conventions. |
| [Hardware.md](Hardware.md) | Pinout, pressure-sensor path and runtime calibration, relay, AM2302, flash layout. |

## Project documents

| Document | Contents |
|---|---|
| [Project_Overview.md](Project_Overview.md) | Overall overview: purpose (well-pump regulation), architecture diagrams, feature walkthrough, build/deploy workflow, Tiny-LLM outlook. |
| [Certificates.md](Certificates.md) | PKI structure (CA/server/device/OTA signing), how certificates reach firmware and server, step-by-step issuing and key rotation. |
| [Hardening_Tests_2026-07-10.md](Hardening_Tests_2026-07-10.md) | Fault-injection test log on the real device: Wi-Fi loss, power loss during/after update, tampered image — including the session-limbo bug found and fixed. |

## Further sources in the repository

- **Host-test runners:** `../test/host/run.sh` (pump state machine + link
  scorer) and `../src/components/clientside_protocol/test/host/run_host_tests.sh`
  (golden auth vector + session path; builds cJSON + mbedTLS ABI-consistently
  from the toolchain).
