[한국어](scenarios.ko.md)

# Core Usage Scenarios

> **What this chapter answers** — a lookup table that maps a common requirement
> directly to a socket pattern and its core API. The exact contract for each
> pattern is owned by that socket's spec.

| Requirement | Socket pattern | Core API |
|---|---|---|
| One-to-one communication between threads | PAIR | `zlink_send_part`, `zlink_recv_part` |
| Topic-based one-way distribution | PUB/SUB | `zlink_publish_part`, `zlink_subscribe_part` |
| Async routed request/reply | DEALER/ROUTER | `zlink_dealer_request_part`, `zlink_router_reply_part` |
| Sending to multiple peers by routing id | ROUTER | `zlink_send_part_rid`, `zlink_router_recv_part` |
| Integrating with an external TCP-family client | STREAM | `zlink_stream_packet_handler` |
| Waiting on unified readiness | poller | `zlink_poller_add`, `zlink_poller_wait` |
| Observing connection state | socket monitor | `zlink_socket_monitor_open`, `zlink_socket_monitor_recv` |
| Periodic-task notification | generic timer | `zlink_timer_start`, `zlink_timer_recv` |

Application topology and stateful-object scenarios are covered in the
per-language Framework guides and E2E documents.
