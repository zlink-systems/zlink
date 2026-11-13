# #4162 Core gate latency 진단

## 결과

**대표 inproc 셀에서는 벤치의 수신 계측 비용이 queue와 latency를 바꾸는 것을 확인했습니다. 현재 증거는 벤치 측정 재현성 결함을 지지하며, Core 계약 위반은 입증하지 못했습니다. 모든 multi tail의 원인이 같다고 판정하지는 않습니다.**

- 같은 Core·벤치 바이너리로 ROUTER_ROUTER/inproc/1024B를 20회 실행했습니다. low(mean <0.2ms) **8회**, high(≥0.9ms) **8회**, 중간 **4회**입니다. mean 범위는 **0.067293–1.158352ms**입니다. 관측 가능한 low 실행의 평균 queue 근사치는 **153–345건**, high는 **1,813–2,166건**입니다. `raw/runs.tsv`, `raw/final-analysis.json`에 실행별 수치가 있습니다.
- **mode를 가르는 직접적인 관계는 연속 송신 속도와 통계를 포함한 수신 처리 속도의 균형입니다.** 송신은 제한 없이 제출하고(`single/common/perf_single_one_way.hpp:175`), 수신은 다음 recv 전에 통계를 갱신합니다(`:310`). 통계 저장소가 차기 전에는 순차 append, 찬 뒤에는 RNG·나눗셈·임의 위치 교체를 수행합니다(`single/common/perf_single_latency.hpp:43`, `:47`, `:49`). 교체 확률은 코드상 대략 `cap/count`로 감소하므로 계측 비용 자체가 실행 중 변합니다. 수신이 뒤처지면 HWM까지 queue가 누적되고, 따라잡으면 queue가 작아집니다. `평균 queue/수신율`과 보고 mean의 상관계수는 **0.9988**입니다(`raw/final-analysis.json`). 자연 실행에서 이 균형을 바꾼 개별 CPU·cache 요인까지 분리한 것은 아닙니다.
- **변수 강제:** CPU를 receiver=8/sender=10으로, ASLR·주요 스택 주소도 같게 고정했습니다. percentile cap **100만→0→100만**에서 mean은 **0.353921→0.004325→0.094889ms**, 평균 queue 근사치는 **684→5→176건**이었습니다. cap 0에서도 전체 count·sum은 계속 집계하므로 mean의 추정식이 달라진 결과가 아닙니다(`single/common/perf_single_latency.hpp:38`, `:60`). p95/p99는 cap 0의 fallback이므로 비교 근거로 사용하지 않았습니다. 근거: `raw/cap-{A,B,A2}-01.{log,json,proc.jsonl}`, `raw/final-analysis.json`.
- cap을 **200만**으로 강제한 한 실행에서는 시간 구간별 mean이 **0.024ms(0.2–0.8초) → 1.640ms(1.5–2.5초) → 0.144ms(3–4.8초)**였습니다. 같은 실행 안에서도 high 구간이 생겼다가 해소됐습니다. 수신 계측 비용의 전이와 queue 형성이 연결된다는 근거입니다(`raw/cap2M-01.proc.jsonl`, `raw/final-analysis.json`).
- **warm-up 잔여 queue가 영구히 mode를 고정한다는 설명은 성립하지 않은 실행을 확인했습니다.** 외부 ptrace로 receiver만 20ms 정지하자 queue 근사치가 **3,556건**까지 찼고, 재개 후 2.0–2.9초에는 **6.6건·0.00397ms**로 내려갔습니다. inproc 벤치에는 별도 warm-up phase도 없습니다(`single/src/perf_router_router.cpp:242`, `:516`; `single/common/perf_single_one_way.hpp:278`). 근거: `raw/queue-cycle-01.json`, `raw/final-analysis.json`. 이 강제 실행의 전체 RESULT도 보존했으며 정상 gate 값으로 섞지 않았습니다.

**multi 원자료의 구분**

| 셀·가설 | 분리된 증거와 판정 |
|---|---|
| PUBSUB tcp 256B | 15회 중 high 8회는 mean **285.769–595.183ms**, 최저 subscriber 수신율/목표율 **0.8147–0.8657**입니다. low 6회는 **2.243–3.112ms**, 비율 **0.99937–0.99985**입니다. high 8회 모두 1ms 초과 송신 일정 지연은 0건입니다. 수신 측 처리량 부족을 지지합니다(`raw/prior-stats.json`). |
| multi 계측·pacing | 목표율은 통계 수집 전 active 수신율의 80%입니다(`multi/common/perf_multi_measurement.hpp:19`). DD receiver는 active에서 통계를 끄고 latency 구간에서 켭니다(`multi/src/perf_multi_dealer_dealer_server.cpp:230`, `:246`). REQREP도 paced 구간에서만 sampler를 실행합니다(`multi/common/perf_multi_socket_reqrep.hpp:234`). 따라서 80%가 **계측을 포함한 수신 능력**보다 낮다는 보장은 없습니다. PUBSUB의 동일 원인 확정에는 별도 강제 실험이 필요합니다. |
| DD tls/ws 64B | 큰 tail 일부에 송신 일정 지연·catch-up 부하가 동반됩니다. 그러나 지연 비율과 p95가 일대일 대응하지 않으므로 이것만으로 전부 설명하지 않습니다(`raw/prior-summary.txt`; scheduler `multi/common/perf_multi_measurement.hpp:44`, `:52`). |
| ROUTER_ROUTER_REQREP tls 64B | 큰 tail은 송신 일정 지연이 0인 실행에서도 발생했습니다. Core·환경·completion 진행 중 어느 원인인지 미분리입니다(`raw/prior-summary.txt`). 이 경로의 최대 poll 대기는 50ms입니다(`multi/common/perf_multi_socket_reqrep.hpp:654`); timer 원인으로 확정한 것은 아닙니다. |
| Nagle·#3541 | inproc은 직접 pipe pair를 연결합니다(`core/src/runtime/sockets/common/socket_base_endpoint.cpp:272`, `:345`). 따라서 이번 대표 셀은 Nagle과 transport I/O thread round-robin의 영향을 받지 않습니다. transport 선택은 `core/src/runtime/core/ctx_io_thread_registry.cpp:91`이 소유하며, multi의 세션별 I/O 배치는 이번에 계측하지 않았습니다. TCP 기본 NODELAY=1은 `core/src/runtime/core/options.cpp:116`, 적용은 `core/src/runtime/transports/tcp/tcp.cpp:29`입니다. |
| 주파수·타이머·스택 배치 | 자연 실행의 CPU·주파수·주소를 기록했으나 두 mode를 분리하는 단일 값은 찾지 못했습니다. 고정 배치의 cap 실험에서도 latency가 변했습니다(`raw/final-analysis.json`, 각 `.proc.jsonl`). 환경은 `tsc`, monotonic 해상도 1ns를 보고합니다(`raw/environment.json`). 이는 개별 scheduling 지연까지 배제하는 증거는 아닙니다. |

**분류:** 벤치 계측이 측정 대상의 queue를 바꾸는 기존 결함은 **B 후보**입니다. 다만 새로운 latency 부하·phase를 정하는 설계는 **D**입니다. single 정책은 포화 active의 동일 메시지 집합을 요구하고(`doc/perf/PERF_SINGLE_TEST_POLICY.md:40`, `:138`), multi 정책도 동시 집계라고 서술하지만(`doc/perf/PERF_MULTI_TEST_POLICY.md:29`, `:714`) 실제 C reference는 별도 paced 구간을 사용합니다. 감독자가 측정 계약을 먼저 확정해야 합니다. HWM 축소·대기 시간 증가를 원인 수정으로 제안하지 않습니다.

## 판정 위치(전/후 수)

수정은 없으므로 **실제 위치 수는 전후 동일**합니다. 아래 권고 수는 소유 정리 목표이며 구현·승인된 설계가 아닙니다. 범위는 이번 C benchmark 경로입니다. Core·Framework runtime과 언어별 binding은 변경하지 않았습니다.

| 결정 | 소유 조항 | 지금 판정하는 위치 전부 | 단일 수정 소유자·위치 수 |
|---|---|---|---|
| 제출 부하율 | `doc/perf/PERF_SINGLE_TEST_POLICY.md:138`, `doc/perf/PERF_POLICY.md:1294`; multi 정책 충돌은 위 D | single 연속 제출 `single/common/perf_single_one_way.hpp:175`; multi 80% `multi/common/perf_multi_measurement.hpp:19` (DD 적용 `multi/src/perf_multi_dealer_dealer_client.cpp:556`, PUBSUB `multi/src/perf_multi_pubsub_server.cpp:248`, REQREP `multi/common/perf_multi_socket_reqrep.hpp:717`) | C bench 공통 측정 정책. 실제 **2→2**. 계약 확정 후 현재 multi 공통 소유자를 재사용하는 **2→1**을 검토합니다. |
| percentile 저장·교체 | `doc/perf/PERF_POLICY.md:106`, `:120` | `single/common/perf_single_latency.hpp:36`; `multi/common/perf_multi_metrics.hpp:147` | C bench 통계 모듈. 실제 **2→2**, 중복 알고리즘 소유는 **2→1** 권고. 공통화만으로 latency가 해결된다고 판정하지 않습니다. |
| 유효 수신 건수 | `doc/perf/PERF_SINGLE_TEST_POLICY.md:398`, `doc/perf/PERF_POLICY.md:106` | `single/common/perf_single_one_way.hpp:310`; `single/common/perf_single_latency.hpp:39` | 기존 stats builder의 count. 실제 **2→2**, 중복 상태 제거 **2→1** 권고. |
| byte admission | `core/doc/spec/core/systems/06-auto-hwm.ko.md:453`, `:476` | `core/src/runtime/core/pipe_write.cpp:1236`, `:1249`, `:1257`; 공통 술어 `core/src/runtime/core/pipe.hpp:1112` | Core pipe 유지. 일반·예외·경쟁 재확인 호출 **3→3**, 공통 술어 소유 **1→1**. 벤치에서 다시 판정하지 않습니다. |
| dequeue credit·writer wake | 같은 문서 `:457` | `core/src/runtime/core/pipe_receive.cpp:756`, `:807`, `:815`; 대기 등록 `core/src/runtime/core/pipe_write.cpp:146`; LWM 계산 `core/src/runtime/core/pipe_transport.cpp:465` | Core pipe 유지. 최종 writer wake 판정 **1→1**. |
| transport I/O thread 선택 | `core/doc/spec/core/01-context.ko.md:89` | `core/src/runtime/core/ctx_io_thread_registry.cpp:91` | Core context 유지 **1→1**. inproc data 경로에는 미적용. |

`single/`·`multi/`는 `bindings/c/perf/` 아래입니다. 관련 Core 파일은 후보 `0005a4d6c0`와 현재 main이 동일합니다(`raw/source-checks.json`).

설계 대안은 두 가지입니다. **현 계약을 유지하며 수신 계측 비용을 줄이는 B 수정**은 기존 통계 소유자에서 다루되, 이것이 5% A/A 재현성을 보장하는지는 별도 검증해야 합니다. **계측을 포함한 처리 능력으로 latency 부하를 정하는 공통 측정 계약**은 재현성 문제를 직접 다루지만 D를 먼저 해소해야 합니다. 현 증거로 Core admission·wake 변경은 추천하지 않습니다.

## 검증(명령·로그 경로·핵심 수치)

- 대상: detached `76a4d7850664c2cb32e8d35837dac45d9d2a4057`. Core 후보 라이브러리는 `/root/jobs/fx/cx/v21/s033/core124/prefix-1.24/lib/libzlink.so.1.23.0`, SHA-256 **`2e0aa58e221f0cbe4755d440b20f9c71dee67f692b2fee554d44a913f8323d64`**입니다. Core 자체는 다시 빌드하지 않았습니다. 실제 로딩 경로는 `raw/ldd.txt`입니다.
- 벤치 SHA-256은 **`ba819c1f7f59ed74e168e3d72d4fd17474a7cc27493fdb0ad06f1501a84998a7`**이며 **27회 모두 동일**합니다. 원래 20회, 관측 보완 자연 실행 2회, cap A/B/A 3회, queue 강제 1회, cap 200만 1회이며 **27/27 정상 종료**했습니다(`raw/final-analysis.json`).
- 빌드: `bash /root/jobs/fx/cx/v21/s033/i4162/build.sh` → `raw/build-exact.log`. 지정 `cpp-build-slot.sh`, `make -j3`, 25GiB address-space 상한을 사용했습니다. 바이너리와 소스 사본은 `bin/`, `source/`에 보존했습니다.
- 자연 실행: `bash /root/jobs/fx/cx/v21/s033/i4162/run-natural.sh`. 측정 명령은 `flock -o /root/jobs/fx/cx/measure.lock taskset -c 0 python3 .../observe.py --runs 20 --label natural`입니다. 각 benchmark는 CPU 8–15, inproc/1024B, 5초, balanced, 2-part 조건입니다. `raw/natural-driver.log`, `raw/natural-01`부터 `raw/natural-20`까지 보존했습니다.
- 강제 실행: `run-controls.sh`, `run-queue-cycle.sh`; cap 200만은 같은 lock 아래 `observe.py --runs 1 --label cap2M --placement separate --aslr-off --sample-cap 2000000 --pad 0`입니다. 로그는 `raw/control-driver.log`, `raw/queue-cycle-driver.log`, `raw/cap2M-driver.log`입니다. cap 0의 환경 문자열 길이는 padding으로 보정했고 주요 스택 주소가 실제로 같았음을 확인했습니다(`raw/final-analysis.json`).
- 외부 관측은 `/proc`를 읽었습니다. **queue 근사치 = 송신 payload sequence − 유효 수신 count**이며, 진행 중 send와 비동시 읽기 오차를 포함합니다. 실제 pipe depth와 구분합니다. 원래 20회 중 **12·18번은 상태 주소 식별 실패**로 queue 관측이 없었습니다. 두 결과를 버리지 않고 기록했으며, 주소 선택을 보완한 외부 관측기로 2회를 추가해 자연 실행의 유효 queue 관측 **20회**를 확보했습니다. count 일관성 위반은 0건입니다(`raw/final-analysis.json`).
- 스레드 합계는 각 `.before.json`·`.after.json`의 **TID 합집합**으로 재검산했습니다. 전체 실행별 값은 `raw/runs.tsv`, 분석 명령은 `analyze_prior.py`, `analyze_observed.py`, `final_analysis.py`입니다. 기존 A/A 실패 **single 105/840, multi 129/560**도 원문과 대조했습니다(`raw/prior-stats.json`). 전체 gate·E2E·Framework 검사는 실행하지 않았습니다.

## 리팩토링 점검

리팩토링 점검: 성능 2건, POSDDD 1건, 불필요 코드 1건 발견. **고친 항목 0건**. 넘긴 항목: 매 메시지 송신 통계 atomic 갱신(`single/common/perf_single_one_way.hpp:188`), 수신 경로의 가변 reservoir 비용(`single/common/perf_single_latency.hpp:43`), 수신 건수 중복 상태(위 표), 호출되지 않는 readiness probe 경로(`single/src/perf_router_router.cpp:353`, `:389`). 송신 atomic과 죽은 경로를 이번 mode의 확정 원인으로 판정하지 않았습니다. 추가 상태·flag·queue·lock은 0개입니다.

## 남은 문제

- 자연 실행에서 수신·송신의 상대 처리 속도를 바꾼 세부 CPU/cache/scheduling 요인과 multi REQREP tls tail은 미분리입니다. 특정 Core 수정이나 #4135가 mode 발생 확률을 바꾸는지는 이번 A/A 실험으로 판정하지 않습니다.
- PUBSUB·DD의 계측 비용 강제 실험과 세션별 I/O 배치 관측은 미실행입니다. 대표 inproc에서 입증한 계측 효과를 모든 transport의 단일 원인으로 확대하지 않습니다.
- 감독자는 single·multi 측정 계약의 불일치와 latency 부하 정의를 먼저 결정해야 합니다. sample cap 0은 **원인 분리용**이며 릴리스 설정 변경안이 아닙니다.
- 제품·벤치·스펙 소스 변경, git 네트워크 작업, GitHub·다른 AI·sub-agent 사용은 없습니다. 이 작업의 빌드 디렉터리를 제거했고, worktree와 모든 원자료를 보존했습니다. 최종 git status·diff는 비어 있고 실행 프로세스도 남지 않았습니다(`raw/final-integrity.json`).
