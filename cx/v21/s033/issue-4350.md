## 이 단계: L0 구축과 교차 시험

- 네 언어에서 실제 Framework route mesh 송수신 경로를 binding perf 모양으로 벗깁니다. 벤치 서버와 클라이언트를 모두 벗기고, 기존 bench와 wire는 그대로 동작해야 합니다.
- L0와 현재 main(대조군)에서 교차 시험을 합니다: raw 서버+raw 클라이언트, Framework 서버+raw 클라이언트, Framework 서버+Framework 클라이언트. 이것으로 source 쪽 손실과 target 쪽 손실을 나눕니다. #56에서는 client 37%p, server 19%p였습니다.
- **완료:** 네 언어의 L0 처리량이 binding의 0.95 이상이고, 교차 시험 표가 있습니다. L0가 binding보다 크게 낮으면 벗기지 못한 비용이 남은 것이므로, 그 위치를 먼저 찾습니다.

## 공통 방법 (확정, #56·#1639)

- Framework 성능은 binding 사다리로 맞춥니다. 현재 경로에서 비용을 조금씩 깎는 방식은 쓰지 않습니다(실장님 결정 2026-09-10 #56, 2026-11-12 재확인).
- L0은 Framework 메시징 경로를 binding perf와 같은 모양으로 벗긴 상태입니다. 수신 owner가 블로킹으로 기다리고, 빌 때까지 받고, 받은 파트를 그대로 응답에 얹어 제출합니다. 그 위에 기능을 L1→L7 순서로 하나씩 되돌리며 단계마다 처리량을 binding과 나란히 잽니다. 이 이슈는 그중 한 단계를 네 언어에서 함께 다룹니다.
- **예산:** 단계마다 직전 단계 대비 처리량 손실은 기본 7% 이하입니다(0.93^7 ≈ 0.60, 목표 Framework ÷ binding ≥ 0.6). 예산을 넘은 언어는 이 단계의 기능을 binding 수준 비용으로 다시 만듭니다. 스펙이 정한 동작은 바꾸지 않으며, 스펙을 바꿔야만 예산에 들어오면 멈추고 결정을 요청합니다.
- **측정:** kairos 측정 서버, source CPU 0–7·target CPU 8–15, warmup 30초, 활성 5초, 3회 중앙값, 측정 lock exclusive. 셀은 #4190 조건의 send-saturation·request-serial·request-backpressure × 1K·4K입니다. 기준선은 https://zlink-docs.zlink.systems/perf/2026-11-12-4190-warmup30/ 입니다.
- **규칙:** 사다리 코드는 실험 branch에만 두고 머지하지 않습니다. 제품 코드에 기능을 끄는 플래그를 넣지 않습니다. 다시 만든 기능만 이 이슈를 참조하는 PR로 냅니다. 단계마다 켜고 끈 코드 위치(`file:line`)와 단계별 처리량·손실·예산 판정 표를 이 이슈와 문서 사이트 성능 메뉴에 남깁니다.
- 담당은 kairos입니다. 언어별 담당을 두지 않습니다.

## 언어별 진행

- [ ] C++
- [ ] .NET
- [ ] Java·Kotlin
- [ ] Node

