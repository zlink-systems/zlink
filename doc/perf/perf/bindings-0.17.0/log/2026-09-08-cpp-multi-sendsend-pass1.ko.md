# C++ Multi TCP SENDSEND pass 1 — 2026-09-08

## 판정 범위

독자는 이 pass의 채택·완화 목표 적용 여부를 판단하는 감독자다. 대상은
`MULTI_DEALER_ROUTER_SENDSEND`(DR)와 `MULTI_ROUTER_ROUTER_SENDSEND`(RR)의 TCP다.
목표 throughput은 C 대비 95%이며, before는 사용자 지정 `p8ss5r` 5-run이다.
Library 수정 후보를 채택하지 않았다. 아래 재측정은 **변경 없는 source의 확인**이며
최적화 after로 해석하지 않는다. 확인 aggregate는 DR/RR **92.25%/92.25%**, 기능·대표 회귀
gate는 통과다. 95% 기본 목표는 미달이며 90% 완화 목표 적용은 감독자가 판단한다.

## 고정 조건과 보존 자료

- Branch `main`. 시작 시 tracked 변경 없음. Framework의 untracked
  `languages/cpp/bench/with-grpc/log/{smoke,smoke2}/`,
  `languages/node/bench/with-grpc/log/smoke/`를 보존한다.
- `ZLINK_CORE_SOURCE=release`,
  `ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.1`.
- Core library: 위 prefix의 `lib/libzlink.so.0.17.1`, SHA-256
  `a3e00fd269b2a1c8d66371ac7ae7efd3f6dd25e6ab842484b847352258ea39a2`.
  Core 재빌드·교체 없음. C++ Release, 기존 benchmark binary를 사용한다.
- 공식 측정은 TCP, clients 100, I/O threads 4/4, balanced auto-HWM, 2-part,
  sizes 64/256/1024/4096/65536, duration 5 s, runs 5다.
- 모든 진단·공식 측정 직전에 `bash scripts/perf/wait-for-idle-perf.sh`와 `uptime`을
  실행하고 1분 load가 5를 넘으면 기다린다. Perf는 직렬로 실행하며 빌드·테스트와 겹치지 않는다.
- 원자료·진단 harness·분석 도구·기능 gate 로그:
  `/tmp/zlink-cpp-sendsend-pass1/`. `pair.py`는 기존 공개 benchmark binary를 구동하고,
  `cg.py`는 callgrind의 함수별 self/inclusive Ir와 call edge를 집계한다.
  `profile-summary.json`에 정규화 원자료를 보존한다. Runner source와 Core는 수정하지 않는다.

필독 가이드 §4, decisions D-B121~D-B130·D-BP17~D-BP21, DD pass 1 및 REQREP pass 1·2의
기각 근거를 확인했다. 과거 DD의 기각을 SENDSEND 전체에 그대로 적용하지 않고 실제 호출을 비교했다.

## 1. DD 대비 routed 경로 비용 분리

### 실제 workload와 비교 단위

**SENDSEND는 SEND API로 양방향 DATA echo를 만드는 workload다.** DR client는 일반
`dealer.send()`로 제출하고 ROUTER server가 target RID를 지정해 echo를 보낸다. RR은
client도 `router.send(target_rid)`를 사용한다. 따라서 DD→DR client의 전체 Ir 차이를
곧바로 RID 송신 비용으로 부르면 안 된다. DD client에는 echo 수신이 없고 DR client에는 있다.

- DR client: `bindings/cpp/perf/multi/src/perf_dealer_router_client.cpp:196`.
- Routed server: `bindings/cpp/perf/multi/common/perf_multi_routed_relay.hpp:61`.
- RID 저장: `bindings/cpp/src/Runtime/Sockets/router.cpp:22`,
  `bindings/cpp/src/Runtime/Messaging/operation_state.hpp:131`.
- Native 제출: `bindings/cpp/src/Runtime/Messaging/operation_submit.hpp:95`.

첫 비교의 진단 조건은 모두 **TCP, 64 B, clients 100, duration 5 s, 2-part**다. 먼저 DD·DR·RR 각각
C/C++ client만 `valgrind --tool=callgrind --cache-sim=no --collect-jumps=no`로 실행했다.
Server의 routed send는 DR 양쪽 process를 callgrind로 실행한 별도 쌍에서 확인했다.
실행은 예를 들어 `python3 /tmp/zlink-cpp-sendsend-pass1/pair.py cpp cpp dealer_router 64 100 5 client`이며,
server 비교에서는 마지막 인자를 `both`로 쓴다. DD는 `dealer_dealer`, RR은 `router_router`다.
원 profile 이름은 `<server언어>_<client언어>_<pattern>_64_100_5_<client|both>.<client|server>.cg`다.
아래 profile의 모든 process 쌍은 exit 0이며 진단 JSON은 `status: complete`다.

분모는 client/server 각각의 `(zlink_send_part calls + zlink_send_part_rid calls) / 2`다.
따라서 **native 2-part 환산 제출 시도당 Ir**이며, 거절·재시도와 DD stop record를 포함한다.
DD의 single-part stop record는 100건이다. C++ DD는 stop을 포함한 async 시작
193,727건·재시도 35건이며 native 2-part 환산 분모는
193,727 + 35 − 100/2 = **193,712건**이다. 실제 2-part payload 시도는 193,662건이다.
C DD도 환산 분모 281,881건 중 stop 환산 50건이 포함된다. 따라서 이 분모를
완료된 업무 메시지 수와 엄밀히 같다고 하지 않는다. DR/RR 64 B client에는 SEND waiter
등록·재시도가 없으며 async 호출 수와 분모가 일치한다.

Callgrind는 application과 I/O thread 진행 비율을 바꾼다. 특히 DD server의 실제 시간
active 구간과 느린 client의 송신 구간이 어긋나므로 server RESULT×5를 분모로 쓰지 않는다.
Profile의 RESULT throughput·latency는 공식 성능 판정에 사용하지 않는다.
처음 DD 진단은 harness에 `CLIENT_READY → START` 처리가 없어 대기했다. 해당 실행을
중단하고 기존 runner의 server/client START 절차를 반영했으며, 중단 자료는 아래 표에서 제외했다.

### Client 전체 및 application 구간

| 항목, Ir/제출 시도 | C DD | C DR | DR−DD, C | C++ DD | C++ DR | DR−DD, C++ |
|---|---:|---:|---:|---:|---:|---:|
| 전체 process | 6,418 | 26,791 | +20,373 | 8,257 | 31,757 | +23,500 |
| application active 함수 inclusive | 4,671 | 15,362 | +10,691 | 6,194 | 17,094 | +10,900 |
| native `zlink_send_part` inclusive | 4,029 | 4,765 | +736 | 3,962 | 4,583 | +621 |
| native receive inclusive | — | 6,694 | +6,694 | — | 6,338 | +6,338 |
| native `zlink_poller_wait` inclusive | 3 | 2,447 | +2,444 | 6 | 1,799 | +1,793 |

Client native 제출 시도 수: C DD **281,881**, C DR **38,700**, C++ DD **193,712**,
C++ DR **30,800**. Application 함수는 C의 `run_single_size_case`/`run_echo_window_round_robin`,
C++의 각 `run_phase [clone .actor]`다. Inclusive 항목은 서로 포함되므로 합산하지 않는다.

전체 process의 차분의 차는 약 **3,127 Ir**지만 여기에는 setup·Core I/O가 들어간다.
Application 구간의 차분의 차는 약 **210 Ir**다. 이 값 역시 runner·poll 빈도 차이를 포함하며
순수 RID 비용은 아니다. 실제 공통 SEND 경계를 아래처럼 다시 분리해야 한다.

| C++ 함수, Ir/제출 시도 | DD client | DR client | RR client |
|---|---:|---:|---:|
| builder (`dealer_socket_t::send` 또는 `router_socket_t::send`), inclusive | 89 | 89 | 221 |
| `submit_raw_send_state` inclusive − native send inclusive | 702 | 702 | 707 |
| `async()` inclusive − `submit_raw_send_state` inclusive | 270 | 271 | 271 |

**DR의 송신 binding 비용은 DD와 같다.** Routed builder의 추가 비용은 약 **132 Ir**,
submit wrapper 차이는 약 **5 Ir**다. RR client의 native routed send는 C **5,627 Ir**,
C++ **5,362 Ir**다. DR→RR의 native 차이는 C +861, C++ +779 Ir이므로 native 비용 증가를
C++의 RID wrapper에 귀속하지 않는다. Target lookup·route별 admission은 고정 Core 안에 있다.

### 실제 ROUTER server

양쪽 process를 callgrind로 실행한 DR 쌍이다. C/C++ server의 native 제출 시도는
각각 38,600/31,500건이다.

| Server, Ir/제출 시도 | C | C++ |
|---|---:|---:|
| 전체 process | 22,542 | 24,178 |
| application relay inclusive | 14,462 | 15,767 |
| native routed send inclusive | 5,278 | 5,278 |
| native router receive inclusive | 4,960 | 4,967 |
| native poll inclusive | 1,910 | 2,131 |
| C++ routed builder inclusive | — | 221 |
| C++ submit wrapper − native send | — | 711 |
| C++ async terminal − submit wrapper | — | 271 |

Native routed send·receive는 두 언어가 거의 같다. C++의 추가 builder 132 Ir는
server application의 **0.84%**, 전체 process의 **0.55%**다. 이는 명령 비중이며
실제 시간 개선의 상한을 증명하지 않는다. 다만 target별 map·allocation·중복 route lookup 같은
큰 binding 전용 비용이 있다는 가설을 지지하지 않는다. 64 B profile을 64 KiB의 재시도율이나
wall-clock 병목으로 일반화하지 않는다.

### RR 1024 B 확인

Before 비율이 가장 낮은 RR 1024 B에서도 동일한 비용인지 마지막으로 확인했다.
TCP·clients 100·duration 5 s·2-part, client만 callgrind이며 C/C++ 모두 complete다.
시작 1분 load는 C 2.82, C++ 1.26이었다.

| RR 1024 B, Ir/제출 시도 | C | C++ |
|---|---:|---:|
| 제출 시도 수 | 36,600 | 29,964 |
| 전체 process | 26,770 | 31,816 |
| native routed send inclusive | 5,592 | 5,409 |
| routed builder inclusive | — | 221 |
| submit wrapper − native send | — | 707 |
| async terminal − submit wrapper | — | 271 |

C++의 async 호출·native 2-part 시도는 모두 29,964건이며 SEND waiter 등록·재시도가 없다.
Builder·submit wrapper·async terminal의 비용은 64 B와 같다. 이 표본에서도 throughput 격차를
RID별 대기자 자료구조나 반복 재제출의 큰 추가 비용으로 설명할 근거는 없다.
Profile은 위 이름 규칙에서 size를 `1024`로 바꾼 두 파일이다.

## 2. 후보와 계약 판단

| 검토 방향 | 채택/기각과 근거 |
|---|---|
| RID를 native operation storage로 직접 변환해 thread-local ring 경유 복사 제거 | 미구현·미채택. `routing_id_access.hpp:57`의 ring→`operation_state.hpp:134` 복사는 줄일 수 있는 국소 비용이다. 그러나 RID 유무를 포함한 builder 추가 비용 전체가 132 Ir/건이고 제거 가능한 것은 그 일부다. 목표 격차를 유의미하게 줄인다는 근거가 없다. 이를 계약상 제거 불가능한 비용이라고 주장하지 않는다. |
| RID snapshot 자체를 빌린 pointer로 대체 | 기각. Builder와 거절된 async operation은 caller RID 객체보다 오래 유지될 수 있다. Target value를 operation이 소유해야 exact-target 재제출을 보존한다. 현재 snapshot은 inline native struct이며 별도 heap 할당이 없다. |
| Native route lookup 결과·선택한 pipe를 binding에서 cache | 기각. Route 교체·credit·admission의 소유자는 Core다. 공개 API 밖의 pipe를 보관하거나 RID별 연결 상태를 binding에서 복제할 수 없다. |
| Routed receive metadata 전달의 값 복사 축소 | 미구현·미채택. `commit_receive_metadata` self 103 Ir/건(server application의 0.65%)이다. 실제 caller의 RID storage는 유지해야 한다. 이 전달 함수 전체를 없앤다고 가정해도 작은 명령 비중이며 routed send의 큰 원인이 아니다. |
| 성공 경로의 반복 errno 조회 축소 | 미구현·미채택. 성공 errno를 조회할 필요 자체는 줄일 수 있지만 함수 호출당 약 14 Ir이며 RID 전용 비용이 아니다. 이 작은 정리만으로 현재 pass의 채택 기준을 충족한다는 근거가 없다. |

기존 no-go는 재구현하지 않았다. SEND의 immediate result는 인스턴스별 소비 상태를 보존하며,
singleton/public result pool은 허용되지 않는다. Native borrowed part는 실패 때도 소비되는
Core part와 재제출용 C++ 원본을 분리한다. Native move로 이 비용을 없애면 원본 보존이 깨진다.
공개 scheduler ABI·coroutine frame/entry pool·runner 변경·in-flight 상한·재제출 순서 변경도
제외했다. `measurement_part_count()`의 기존 static cache는 건드리지 않았다.

계약 근거:

- `bindings/doc/spec/README.ko.md:1336-1353`: SEND OK/ID 0 즉시 terminal,
  BACKPRESSURED 입력 보관, socket-local context/token 대조, 독립 staging copy, 별도 send gate 금지.
- `core/doc/spec/core/socket/README.ko.md:972-1008`: 거절 시 part 소비, payload-free token,
  NO_DATA까지 completion drain 후 같은 record 재제출, ROUTER target은 RID 하나이며
  다른 RID credit으로 해당 token을 깨우지 않는다.
- `bindings/doc/spec/async-coroutine-policy.ko.md:34-37`: operation 생성 시 target capture.
- `bindings/doc/spec/README.ko.md:942-956`: caller-provided receive storage와 RID 저장소 재사용.

판정은 **이 pass에서 채택 가능한 유의미한 성능 후보 없음**이다. 계약이 요구하는 부분은
message·target 소유권, operation별 결과의 공개 동작, Core admission·retry 규칙이다.
RID의 불필요한 임시 복사까지 계약상 필수라고 하거나, 가능한 모든 미래 최적화가 없다고
주장하지 않는다. 새 상태·timer·pool을 추가해 수치를 만들지 않는다.

## 3. 5-run 확인 측정

Library 수정 없이 같은 source를 재측정했다. 공식 runner가 각 size의 5-run **중앙값**을
RESULT로 기록하며, aggregate는 size별 C 대비 throughput 비율의 산술평균이다.
Before는 지정된 `p8ss5r`를 그대로 썼다. C baseline을 다시 선택하지 않았다.
After 시작 load(1/5/15분)는 **0.19/1.07/2.18**이었다.

| Pattern | Size | C ops/s | C++ before ops/s | C++ 확인 ops/s | Before/C | 확인/C | Before 대비 |
|---|---:|---:|---:|---:|---:|---:|---:|
| DR | 64 | 495,809.4 | 451,500.2 | 458,600.8 | 91.06% | 92.50% | +1.57% |
| DR | 256 | 452,137.6 | 428,099.6 | 416,636.0 | 94.68% | 92.15% | -2.68% |
| DR | 1024 | 450,293.8 | 407,060.0 | 410,209.2 | 90.40% | 91.10% | +0.77% |
| DR | 4096 | 405,658.8 | 366,690.2 | 366,536.4 | 90.39% | 90.36% | -0.04% |
| DR | 65536 | 74,166.2 | 70,695.8 | 70,559.4 | 95.32% | 95.14% | -0.19% |
| **DR aggregate** | — | — | — | — | **92.37%** | **92.25%** | **-0.13%p** |
| RR | 64 | 489,424.2 | 432,938.2 | 437,073.0 | 88.46% | 89.30% | +0.96% |
| RR | 256 | 443,228.0 | 402,187.6 | 399,207.0 | 90.74% | 90.07% | -0.74% |
| RR | 1024 | 452,185.4 | 391,606.6 | 396,001.4 | 86.60% | 87.58% | +1.12% |
| RR | 4096 | 392,575.8 | 365,739.2 | 360,259.0 | 93.16% | 91.77% | -1.50% |
| RR | 65536 | 78,588.4 | 81,179.4 | 80,566.4 | 103.30% | 102.52% | -0.76% |
| **RR aggregate** | — | — | — | — | **92.45%** | **92.25%** | **-0.21%p** |

두 pattern 모두 95% 목표에 미달한다. Code diff가 없고 aggregate 변화도 −0.13/−0.21%p라
개선 효과가 아니다. 사용자가 지시한 대로 대상 SENDSEND의 이번 성능 판정에는 throughput만
사용했다. 원 report에는 latency도 보존돼 있다.

Report는 모두 `status: complete`다. Before는 pattern별 success 5이며,
이번 확인 report는 **success 10**, expected/actual RESULT **50/50**이다.

- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_232939_p8ss5r.txt`
- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_233439_p8ss5r.txt`
- `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_233213_p8ss5r.txt`
- `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_233704_p8ss5r.txt`
- `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_234733_sendsend1-unchanged-5r.txt`

실행 명령(환경 변수는 위 고정값):

```bash
bash scripts/perf/wait-for-idle-perf.sh
uptime
bash bindings/cpp/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern MULTI_DEALER_ROUTER_SENDSEND,MULTI_ROUTER_ROUTER_SENDSEND \
  --transports tcp --msg-sizes 64,256,1024,4096,65536 \
  --duration 5 --runs 5 --results-tag sendsend1-unchanged-5r
```


## 4. 회귀 gate

기능 gate: 고정 Core를 연결한 기존 C++ Release build를 확인한 뒤
`ctest --test-dir bindings/cpp/build --output-on-failure -E '^perf_cpp_'` **27/27 통과**.
Contract 19, sample-smoke 7, stress 1이다. 별도 unit label은 없으며 단위 동작 검증은
contract suite에 포함된다. `build.log`, `tests.log`에 보존했다.

대표 셀은 DD·PUBSUB TCP 64/1024 B, clients 100, duration 5 s다.
Before는 사용자 지정 5-run `p6b5run`, 최초 확인은 탐색용 runs 1이다.
시작 load는 **3.27/3.08/2.78**이었다. `status: complete`, success 4, RESULT 20/20.

| Pattern | Size | Before ops/s | 1-run ops/s | throughput 변화 | Before latency(ms) | 1-run latency(ms) | latency 변화 |
|---|---:|---:|---:|---:|---:|---:|---:|
| DD | 64 | 1,391,177.8 | 1,398,705.6 | +0.54% | 0.063168 | 0.069678 | +10.31% |
| DD | 1024 | 1,252,076.0 | 1,229,552.4 | -1.80% | 0.532587 | 0.564324 | +5.96% |
| PUBSUB | 64 | 2,126,793.0 | 2,166,041.8 | +1.85% | 1340.505749 | 1365.519895 | +1.87% |
| PUBSUB | 1024 | 2,302,388.2 | 2,384,736.4 | +3.58% | 895.604816 | 880.243579 | -1.72% |

1-run gate는 **수치상 미통과**다. Throughput −5% 초과 하락은 없지만 DD 64 B latency가
+10.31%로 +10% 경계를 0.31%p 넘었다. Library·runner·Core가 같은 상태이므로 코드 변경에
기인한 회귀로 귀속하지 않는다. D-BP21의 1-run 변동과 §7.2 경계 판정 조건에 따라
**결과에 관계없이 한 번의 5-run 확인**을 추가했다. 유리한 값이 나올 때까지 반복하지 않는다.

- `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_231855_p6b5run.txt`
- `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_232356_p6b5run.txt`
- `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_235255_sendsend1-reg-1r.txt`


### 경계 5-run 확인

한 번의 5-run 확인으로 **최종 대표 회귀 gate 통과**다. 시작 load는
**0.98/2.43/2.58**, report는 `status: complete`, success 4, RESULT 20/20이다.
비교값은 before와 확인 모두 5-run 중앙값이다.

| Pattern | Size | Before ops/s | 5-run ops/s | throughput 변화 | Before latency(ms) | 5-run latency(ms) | latency 변화 |
|---|---:|---:|---:|---:|---:|---:|---:|
| DD | 64 | 1,391,177.8 | 1,401,105.6 | +0.71% | 0.063168 | 0.061618 | -2.45% |
| DD | 1024 | 1,252,076.0 | 1,223,334.6 | -2.30% | 0.532587 | 0.552584 | +3.75% |
| PUBSUB | 64 | 2,126,793.0 | 2,132,062.6 | +0.25% | 1340.505749 | 1356.869664 | +1.22% |
| PUBSUB | 1024 | 2,302,388.2 | 2,327,182.4 | +1.08% | 895.604816 | 941.133536 | +5.08% |

Throughput 최대 하락 **−2.30%**, latency 최대 증가 **+5.08%**로 모두 gate 안이다.
1-run에서 걸렸던 DD 64 B latency는 5-run에서 **−2.45%**다. Source·binary가 동일하고
5-run으로 경계 초과가 재현되지 않았으므로 첫 +10.31%는 1-run 변동으로 판단한다.
추가 재측정은 하지 않았다.

- `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_235433_sendsend1-reg-5r.txt`

```bash
bash scripts/perf/wait-for-idle-perf.sh
uptime
bash bindings/cpp/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern MULTI_DEALER_DEALER,MULTI_PUBSUB \
  --transports tcp --msg-sizes 64,1024 --duration 5 --runs 5 \
  --results-tag sendsend1-reg-5r
```

## 5. 변경 확인

Library·공개 헤더·runner diff는 모두 0줄이다. 이 기록 외의 tracked 파일을 수정하지
않았으며 commit·push하지 않았다. Runtime 규칙 수는 전/후 동일하다.
Core SHA-256은 시작 값과 일치한다. 시작 이후 다른 작업의 문서 commit이 추가됐지만
before commit `ca924fd43e`부터 확인한 HEAD `5e206715fe`까지 C++ binding·C/C++ runner diff는 없다.

- 소유 계층: Core는 RID별 route/admission/WRITABLE, binding은 target·message 소유권과 언어 terminal.
- Spec: binding README Submit 결과 투영, async-coroutine-policy §2, Core socket Part send.
- 교차언어: C/C++의 native routed send·receive 비용은 거의 같고 C++의 추가는 언어 소유권/결과와 wrapper다.
- 변경 분류: runtime 변경 없음. 검토한 국소 복사 축소는 B 후보이나 미구현·미채택,
  Core route cache와 ownership 생략은 C(우회)로 기각.
- 수정 전/후 규칙 수: 동일(코드 diff 0).
