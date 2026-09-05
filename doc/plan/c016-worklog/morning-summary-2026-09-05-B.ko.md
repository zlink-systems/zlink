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

## 7. 저녁 요약 (09-05 12:00~22:20 KST) — 내일 아침용

### 완료·push
- A 후속 6건 전부(§8 인계 문서), spec gap 확정 4건(D-B109/112/116/119), bindings parity 8개(spec+구현+테스트).
- Core 수정 6건: admission pair ID `7ffb8e55d9`, monitor identity `1c69086a4a`, inproc progress `0c39ed2e52`, ws/tls identity·REJECT close `349040d3e6`, **64 KiB REQUEST 토큰 busy loop `a40cb46335`**(모든 binding 64K REQREP 회복). hotpath_gate 3회 PASS.
- 성능 pass: C++ 2, .NET 3, Java 3, Node 3, Go 2, Rust 1, Python 1 — 전부 gate green·push.

### 언어별 최신 판정(quiet paired 3-run, Core `a40cb46335`, tcp multi 4 pattern: DD / DR / RR / PUBSUB, %)
| 언어 | before | 최신 | 목표 | 남은 원인 |
|---|---|---|---|---|
| C++ | 75/52/61/93 | 90.8 통과 / 57.4 / 68.4 / 93.2 (tcp) | 95(90 완화)/85/85/95 | REQREP pending 구조(계약 유지 후보 소진) |
| .NET | 44.7/51.6/53.6/44.8 | 59.6 / 58.3 / 67.0 / 61.3 | 85/70/70/85 | submit 락·요청당 Task·runtime pump(pass 3에서 pump 대안 2개는 64K 붕괴로 기각) |
| Java | 50.8/15.1/24.0/81.0 | 80.9 / 59.4 / 58.7 / 80.9 | 90/70/70/90 | REQREP FFM 39회 vs C 17회, DD 64K |
| Node | 27.6/18.7/17.9/28.6 | 35.9 / 24.3 / 24.8 / 30.2 | 60/60/60/60 | REQREP ~50 ms 고정 지연(client completion 대기), one-way JS 객체 비용 |
| Go | 31.5/2.9/2.8/47.3 | 53.2 / 19.0 / 21.8 / 54.6 | 65/53/53/65 | REQREP 러너 = socket당 1 in-flight(D-B127), SEND 무할당 잔여 |
| Rust | 63.0/65.9/68.2/83.2 | 64.7 / 66.1 / 69.1 / 93.8 | 95/85/85/95 | DD 작은 크기 wrapper 이동·shared RwLock, REQREP pass 2 |
| Python | 6.6/14.2/15.4/26.2 | 9.2 / 15.2 / 16.5 / 28.8 | 60/60/60/60 | GIL 아래 메시지당 Python 79 함수 — C 확장으로 hot path 이전(pass 2) |

### 사용자 결정 요청(아침)
1. **D-B127** Single REQREP 정책 gap 2건: (a) binding 러너의 "백프레셔까지 포화 제출"을 공개 poller `POLLOUT` level로 정의(권고) — 검증 job 필요; (b) C single 러너의 별도 latency 단계를 정책(같은 구간)에 맞춤 → single 기준값 재측정. Multi REQREP도 같은 문제(Go/Java 러너는 socket당 1 in-flight).
2. 목표 미달 언어의 다음 단계: 각 언어 pass 2(리뷰·구조) 계속 vs 여기서 `보류` 확정. 권고: Node 1d·Python 2·.NET 구조(submit 무락)·Rust 2는 효과가 예상되므로 계속, C++/Java는 보류 확정.

### 미결 소소
- Go 1b의 `bindings/go/perf/tests/test_reqrep_control.py`(러너 제어 테스트) worktree 제거 시 누락.
- Python 패키지 payload 디렉터리에 stale 0.15.1 심볼릭 링크가 있어 perf 러너가 실패했음 → 정리(ignored 산출물).
- Node PUBSUB·C++ Single one-way 표는 갱신 완료; C++ Single REQREP 행은 "러너 모델 차이 참고값".
