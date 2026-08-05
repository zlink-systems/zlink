# C++ Stream Connector Guide

This is the official user guide for the C++ STREAM client connector family.

## Table Of Contents

| Document | Content |
|------|------|
| [01 — Overview](01-overview.en.md) | Product family composition, deployment unit, supported engines |
| [02 — Getting Started](02-getting-started.en.md) | Installation, CMake integration, first connection and request |
| [03 — Connector Options](03-connector-options.en.md) | Endpoint, timeout, heartbeat, reconnect, dispatch mode |
| [04 — Sending Packets](04-sending.en.md) | send, request, metadata, compression, codec |
| [05 — Receiving Packets](05-receiving.en.md) | on(), dispatch(), wait_for(), event callbacks |
| [06 — Connection Lifecycle](06-lifecycle.en.md) | connect, close, state transitions, reconnect, heartbeat |
| [07 — Error Handling](07-error-handling.en.md) | result_t, error_code_t, per-error handling |
| [08 — E2E Client](08-e2e-client.en.md) | async(), co_await, task_t, perf scenarios |
| [09 — Engine Adapters](09-engine-adapters.en.md) | Using the Unreal, Godot, Axmol wrappers |
| [10 — Packaging](10-packaging.en.md) | vcpkg, Conan, CMake find_package, build features |
| [11 — Performance Testing](11-performance.en.md) | Running smoke/scale, interpreting reports |

## Quick Reference

**General C++ client**: [Getting Started](02-getting-started.en.md) → [Sending Packets](04-sending.en.md) → [Receiving Packets](05-receiving.en.md)

**Server e2e/perf scenario**: [E2E Client](08-e2e-client.en.md) → [Performance Testing](11-performance.en.md)

**Engine integration**: [Engine Adapters](09-engine-adapters.en.md) → [Connection Lifecycle](06-lifecycle.en.md)

**Deployment/build**: [Packaging](10-packaging.en.md)
