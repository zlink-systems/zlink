# Core 1.24.0 release gate 판정

최종 판정: **FAIL**. Runs-5 재판정에서 cell 165개와 aggregate 121개가 남아 아래 48개 그룹이 실패했다. 상세 비율과 각 실패 항목은 [report.md](./report.md)에 있다.

| Suite | 패턴 | 전송 |
|---|---|---|
| multi | DEALER_DEALER | tcp |
| multi | DEALER_DEALER | tls |
| multi | DEALER_DEALER | ws |
| multi | DEALER_DEALER | wss |
| multi | DEALER_ROUTER_REQREP | tcp |
| multi | DEALER_ROUTER_REQREP | tls |
| multi | DEALER_ROUTER_REQREP | ws |
| multi | DEALER_ROUTER_REQREP | wss |
| multi | DEALER_ROUTER_SENDSEND | tcp |
| multi | DEALER_ROUTER_SENDSEND | tls |
| multi | DEALER_ROUTER_SENDSEND | ws |
| multi | DEALER_ROUTER_SENDSEND | wss |
| multi | PUBSUB | tcp |
| multi | PUBSUB | tls |
| multi | PUBSUB | ws |
| multi | PUBSUB | wss |
| multi | ROUTER_ROUTER_REQREP | tcp |
| multi | ROUTER_ROUTER_REQREP | tls |
| multi | ROUTER_ROUTER_REQREP | ws |
| multi | ROUTER_ROUTER_REQREP | wss |
| multi | ROUTER_ROUTER_SENDSEND | tcp |
| multi | ROUTER_ROUTER_SENDSEND | tls |
| multi | ROUTER_ROUTER_SENDSEND | ws |
| multi | ROUTER_ROUTER_SENDSEND | wss |
| multi | STREAM | tls |
| multi | STREAM | wss |
| single | DEALER_DEALER | inproc |
| single | DEALER_DEALER | ipc |
| single | DEALER_DEALER | ws |
| single | DEALER_DEALER | wss |
| single | DEALER_ROUTER | inproc |
| single | DEALER_ROUTER | ws |
| single | DEALER_ROUTER | wss |
| single | DEALER_ROUTER_REQREP | inproc |
| single | DEALER_ROUTER_REQREP | ws |
| single | DEALER_ROUTER_REQREP | wss |
| single | PAIR | inproc |
| single | PAIR | wss |
| single | PUBSUB | ipc |
| single | PUBSUB | tcp |
| single | PUBSUB | ws |
| single | ROUTER_ROUTER | inproc |
| single | ROUTER_ROUTER | ipc |
| single | ROUTER_ROUTER | ws |
| single | ROUTER_ROUTER | wss |
| single | ROUTER_ROUTER_REQREP | inproc |
| single | ROUTER_ROUTER_REQREP | ipc |
| single | ROUTER_ROUTER_REQREP | wss |
