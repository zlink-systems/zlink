
<!-- zlink-nav:start -->
[← Core Glossary](glossary.en.md)
<!-- zlink-nav:end -->

# Core Usage Scenarios

> **What this chapter answers** — a lookup table that maps a common requirement
> directly to a socket pattern and its core API. The exact contract for each
> pattern is owned by that socket's spec.

| Requirement | Socket pattern | Core API |
|---|---|---|
| One-to-one communication between threads | PAIR | `zlink_send`, `zlink_recv` |
| Topic-based one-way distribution | PUB/SUB | `zlink_publish`, `zlink_subscribe` |
| Async routed request/reply | DEALER/ROUTER | `zlink_request`, `zlink_completion_recv`, `zlink_reply` |
| Sending to multiple peers by routing id | ROUTER | `zlink_send_rid`, `zlink_router_recv` |
| Integrating with an external TCP-family client | STREAM | `zlink_stream_recv_packet` |
| Waiting on unified readiness | poller | `zlink_poller_add`, `zlink_poller_wait` |
| Observing connection state | socket monitor | `zlink_socket_monitor_open`, `zlink_socket_monitor_recv` |
| Periodic-task notification | generic timer | `zlink_timer_start`, `zlink_timer_recv` |

Application topology and stateful-object scenarios are covered in the
per-language Framework guides and E2E documents.
