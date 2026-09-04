# 아침 요약 — 머신 B, 2026-09-05 (밤사이 자율 진행, 05:00~07:30 KST 갱신)

## 1. 완료·push (main)

| 항목 | 커밋 | 결과 |
|---|---|---|
| Core REQUEST 계약 B(D-B85) + 스펙 | `7d8205a028`, `ea934d0e97` | hotpath_gate PASS(D-B86) |
| bindings 8개 REQUEST 포팅 | dotnet `6b4c60eb33`(병합 `2099bb045a`), java `a06260f507`, node `b145f86501`, cpp `e0860723bc`, python `78eed9ce96`, rust `9728a0e081`, go `7afa27e72b`, c `ba615d3137`(+C perf 러너 `0f9c329764`) | 각 gate green |
| Core 후속 | SUB session UAF `29add0ac81`, 단일 파트 REPLY 64 KiB `5e26e72806`, 토큰 경로 리팩토링 `900ea8319e` | ctest green, hotpath ±0.04% |
| Core 0.17.0 vs 0.15.1 판정 | D-B87/D-B88 (plan-b §2) | 처리량 전 셀 −5% 이내, (pattern,transport) 합계 하락 없음 |
| C++ hot-path pass 1·2 | `86b897abf7`, `e6dd88fbc6` | Multi tcp DD 90.8% `통과`(완화 90%), REQREP 57.4/68.4% `보류` |
| C 러너 ws/wss REQREP 4 KiB 붕괴 | `21746768ca` (D-B89) | 러너 제출 턴 문제로 확정·수정, ws 4096 7.9k→76k ops/s |
| 계획서·log 갱신 | `57e262e71b`, `4c65ba0484`, `58eb484525`, `b03681422d`, `c34ef7d496` | §9.1 C++ Multi 4 transport + Single 7 pattern 표 채움 |
| 인계 문서 최종 | `3480ee5d78` | REQUEST 해시·perf 정책 복원 완료 반영 |

## 2. C++ 판정 현황 (계획서 §9.1)

- Multi `tcp`: DD `통과(90.8%)`, DR/RR REQREP `보류(57.4/68.4%)`, PUBSUB `보류(81.5%)`(자체·Sol 두 pass 모두 no-go; subscriber wrapper 5~6%만 binding 귀속).
- Multi `tls/ws/wss`: DD `보류(79.3/85.3/91.1%)`(3-run), REQREP `보류`(53.1~72.3%, §7.5: pattern pass 두 번 완료·후보 소진), PUBSUB `통과(100.2~104.8%)`.
- Single(before, 07:16): one-way 82~109%, routed one-way 84~96%(`inproc`만 57~75%), REQREP 40.6~46.4%. **one-way latency가 C 대비 수십~수백 배** — 정의는 같고 C++ 수신 경로가 병목이라 큐가 HWM까지 차는 현상. 수신 경로 pass 1 job 진행 중(07:17~, 결과는 §5).

## 3. 사용자 결정 필요

1. **D-B83 1·2** Core latency 잔여(DD 작은 크기 평균 latency 1.6~3.5x, REQREP +50%) — Core 추가 수정 여부.
2. **D-B89** C 러너 ws/wss REQREP byte-quantum 제출 턴 유지 여부(붕괴 제거 대신 ws 4K/8K 안정 처리량 40~50% 하락). 대안 (b) client별 제출 즉시 진행, (c) timeout 상향.
3. **D-B90** C++ Multi PUBSUB 러너 parity 수정(서버 auto-HWM 재계산 시점을 C에 맞춤, SUB filter `""`) — 러너 변경이라 확인 뒤 진행.
4. DD/one-way **latency metric**: multi DD와 single one-way 모두 "큐 깊이"가 되어 binding 비교에 쓸 수 없음(양 러너 bimodal). 정책(§2.2 latency 목표)을 admission→수신 시간 등으로 재정의할지.
5. tcp의 DD 완화 목표 90%를 tls/ws/wss에도 적용할지(적용 시 `wss` DD `통과`).

## 4. 남은 것 (계획서 §7.4 순서)

- C++: single 수신 경로 pass 1(진행 중) → after 측정 → Sol pass 2 → single REQREP pass(43%) → 언어 gate → **.NET** → Java → Node → Go → Rust → Python. 언어당 반나절(§6.4) 규모라 오늘 낮 동안 .NET부터.
- Core: D-B83 결정 대기.

## 5. 진행 중 job

- `b-cpp-single-recv-pass1`(sol high, 75분 상한, worktree `~/project/zlink-wt-cpp-single`) — 결과는 이 파일 갱신 또는 계획서 §9.1.1에 반영.
