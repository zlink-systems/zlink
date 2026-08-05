# Config 12 Node E2E feature map

공통 Config 12의 16개 exact scenario를 selector와 독립 role process에 연결했다.
현재 working tree에서 각 scenario를 개별 실행한 결과와 role endpoint evidence가 확인됐다.
이 표의 `process PASS`는 Config 12 targeted evidence이며, common 374개 exact inventory와
Config 1-14 aggregate Framework gate의 완료를 뜻하지 않는다.
현재 `run_e2e.sh ALL`은 CH07C에서 caller native process segfault로 중단됐으므로 aggregate PASS로
표시하지 않는다.

| ID | 상태 | 후속 조건 |
|---|---|---|
| CH-E2E-01 | process PASS | `run_e2e.sh CH01`; 양방향 RouteMesh request와 role evidence |
| CH-E2E-02 | process PASS | `run_e2e.sh CH02`; nested request와 downstream evidence |
| CH-E2E-03 | process PASS | `run_e2e.sh CH03`; Spot callback·timer downstream request evidence |
| CH-E2E-04A | process PASS | `run_e2e.sh CH04A`; ClientServer weight selector evidence |
| CH-E2E-04B | process PASS | `run_e2e.sh CH04B`; draining server admission evidence |
| CH-E2E-04C | process PASS | `run_e2e.sh CH04C`; new lifecycle restart evidence |
| CH-E2E-05 | process PASS | `run_e2e.sh CH05`; one-way send evidence |
| CH-E2E-06 | process PASS | `run_e2e.sh CH06`; duplicate egress startup failure |
| CH-E2E-07A | process PASS | `run_e2e.sh CH07A`; missing channel `NotFound` evidence |
| CH-E2E-07B | process PASS | `run_e2e.sh CH07B`; local Server role remote selection |
| CH-E2E-07C | process PASS | `run_e2e.sh CH07C`; known target retained as `NotConnected`, request `Unavailable` |
| CH-E2E-08 | process PASS | `run_e2e.sh CH08`; handler object egress evidence |
| CH-E2E-09 | process PASS | `run_e2e.sh CH09`; port 0 advertised endpoint와 split role status |
| CH-E2E-10 | process PASS | `run_e2e.sh CH10`; ClientServer recovery evidence |
| CH-E2E-11 | process PASS | `run_e2e.sh CH11`; ChannelName-only route selection |
| CH-E2E-12 | process PASS | `run_e2e.sh CH12`; handler-originated one-way evidence |

`process PASS`는 각 selector의 fresh process 결과다. common exact inventory의 누락, 다른
Config의 blocker, aggregate·coverage·CI 결과를 승격하지 않으며, 해당 조건은 ND-E2E-IMP-001과
ND-E2E-IMP-002에서 별도로 판정한다.
