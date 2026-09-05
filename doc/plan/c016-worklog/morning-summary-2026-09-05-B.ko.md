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
- Single: before 07:16 → 수신 경로 pass 1(library no-go; **C++ 러너가 메시지마다 `PERF_PART_COUNT`를 getenv로 읽던 버그** 수정 `9cb8a3a11b`) → 08:20 재짝지음: PAIR 86~96%, PUBSUB 91~113%, DD 63~96%, DR 91~97%, RR 74~98% — `inproc`(DD 63.1, RR 73.8, PAIR 86.3, PUBSUB 91.1)과 tcp PAIR 90.0·DD 92.6, ws/wss PAIR 94 등만 미달, 나머지 통과. REQREP 40.6~46.4% 미달(pass 예정). one-way latency는 큐 깊이라 판정 제외(D-B91).

## 3. 열려 있던 항목 — 사용자 지시(08:20 "결정할 것 없음, 진행")로 D-B91에 확정

D-B83 Core latency 잔여는 원인별 수정 트랙으로, D-B89 러너 수정 유지, D-B90 PUBSUB 러너 parity 수정 진행, one-way latency는 큐 깊이라 판정 제외(처리량으로 판정), DD 완화 90% 전 transport 적용.

## 3-1. A 후속 2건 접수 (`handoff-B-followups-D086-D087.ko.md`, 08:25 KST job 시작)

- D-086 tcp same-RID handover admission 지연: Core job `b-core-d086`(sol high, 90분).
- D-087 Java 네이티브 라이브러리 임시 디렉터리 누수: Java job `b-java-d087`(sol high, 60분).

## 4. 남은 것 (계획서 §7.4 순서)

- C++: single 수신 경로 pass 1(진행 중) → after 측정 → Sol pass 2 → single REQREP pass(43%) → 언어 gate → **.NET** → Java → Node → Go → Rust → Python. 언어당 반나절(§6.4) 규모라 오늘 낮 동안 .NET부터.
- Core: D-B83 결정 대기.

## 5. 진행 중 job

- `b-cpp-single-recv-pass1` 완료(07:35, 위 반영). `b-core-d086`, `b-java-d087` 진행 중(08:25~).

## 6. 오전 추가 (08:20~11:00)
- 사용자 지시로 결정 항목 5개 감독 판단으로 확정(D-B91), A 후속 6건 전부 완료·push(인계 문서 §8), bindings parity 작업(D-B98~D-B103), Core 수정 3건(`7ffb8e55d9`, `1c69086a4a`, `0c39ed2e52`; hotpath_gate 실행 중).
- 남은 사용자 결정(spec gap): inproc peer close CLOSED 이벤트, 즉시 disconnect→connect overlap, request가 쓴 connection_id 공개.
- 이후: hotpath_gate 결과 기록 → C++/.NET/C monitor mask 통일 → bindings 성능 계획 재개(.NET).
