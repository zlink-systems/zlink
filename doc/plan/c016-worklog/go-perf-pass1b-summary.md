# Go REQREP runner · binding pass 1b

**구현·기능 gate 완료, 성능 최종 판정은 미완료다.** 공식 after는 **19/20 complete cases, 1 failure, partial/exit 1**이며 DD 64B가 종료 기한에 실패한다. REQREP의 pass 1 after 대비 처리량 평균은 DR **6.23x**, RR **6.43x**다. 지정 C 역사값 대비는 **37.8% / 40.2%**로 목표 53% 미달이다. 러너 효과는 아래에서 binding 효과와 분리했다. SEND 왕복 Go heap 비용은 **1216→848 B/op**, **20→16 allocs/op**로 줄었으나 전체 SEND 무할당은 미달이다.

최종 REQUEST 계수는 **1,108,800 attempts, 2 failures, 2 timeouts (0.000180%)**다. 모두 RR 65536B의 timeout이며 해당 cell은 **2/61,176 (0.003269%)**다. Timeout budget은 기존 200ms를 유지했다. Raw reply 오류·panic·fatal admission 실패는 최종 REQREP 로그에 없다.

## 변경 범위

- 작업 트리의 detached 상태와 pass 1 미커밋 변경을 보존했다. checkout/reset/stash/commit/push 및 Core build/clean 실행 없음.
- runner: `bindings/go/perf/multi/perf_multi_socket_reqrep.go`, `perf_multi_main.go`, `bindings/go/perf/run_benchmarks_multi.sh`. 소켓당 goroutine 하나가 blocking `Submit(ctx)`를 반복한다. active deadline 이전의 완료 왕복만 집계하고 이미 제출한 terminal은 join한다. timeout은 실패와 함께 별도 계수한다. 서버는 public POLLIN poller → recv drain → reply token direct reply 경로를 사용한다. C와 같은 100ms auxiliary poll, STOP/QUIT/EOF 종료를 사용한다. client는 RESULT와 CLIENT_DONE을 출력한 뒤 socket을 유지하고, shell이 server를 종료한 뒤 보낸 STOP에서만 socket을 닫는다. RESULT가 있더라도 server/client exit 실패나 강제 종료는 case 실패다. C에 없는 ROUTER SEND 준비 probe와 fallback 응답을 제거했다.
- binding: `bindings/go/internal/native/operations.go`, `send_retry.go`, `dealer_router_request.go`. 기존 builder inline buffer에 두 part를 담고, retry wrapper는 한 backing allocation에 모은다. RID 없는 SEND/REQUEST는 native RID storage를 만들지 않는다. multipart copy/cleanup과 socket completion owner는 기존 경로 하나를 사용한다.
- 회귀·계측: `bindings/go/internal/native/send_retry_storage_test.go`, `send_multipart_bench_test.go`, `bindings/go/perf/multi/perf_multi_socket_reqrep_test.go`, `bindings/go/perf/tests/test_reqrep_control.py`.

## 계약과 대안

- 소유 계층: binding의 builder·retry storage; 러너의 동시 실행과 측정 창. Core의 admission·WRITABLE·timeout 정책은 변경하지 않았다.
- spec: `core/doc/spec/core/socket/README.ko.md:932-985`(part 소비·복사본 보존·WRITABLE), `bindings/doc/spec/go/README.ko.md:118-127,287-294`(Message/MoveMessage/Bytes ownership, blocking terminal, 단일 completion drain), 사용자 감독 결정 e17b0739a7과 PERF_MULTI_TEST_POLICY §1.2.
- 교차언어: C REQREP `record_request_completion:163-205`의 active deadline·왕복 계산, `setup_client_state:655-705`의 connect-ready, `reply_one_request:901-955`와 `run_server_loop:958-988`의 strict token·readable drain을 대조했다. Go suspension은 goroutine이고 completion terminal은 blocking Submit이므로 소켓별 goroutine으로 표현한다.
- 분류: 러너 A(승인된 Go terminal 계약 적응) + B(서버 busy recv/전체 오류 재시도·늦은 reply 집계·조기 socket close·RESULT 이후 process 실패 누락); binding B(불필요한 allocation).
- 수정 전/후 규칙 수: 서버 응답 경로 2(SEND probe fallback/REQUEST reply)→1(REQUEST reply), 준비 경로 2(monitor/probe)→1(monitor). completion 진행 owner 1→1, multipart copy/cleanup 경로 1→1. REQREP socket 종료 근거는 측정 함수 반환/runner STOP의 분리된 결정에서 runner STOP 한 곳으로 모았다. retry wrapper 개별 backing allocation N→1; 새 pool·timer·retry budget·in-flight cap 없음.
- 대안: 초기 snapshot을 실패 시에만 만드는 경로는 MoveMessage와 multipart preparation 실패의 ownership 처리를 분리해야 한다. 이번 변경은 전체 record를 먼저 보존한다는 기존 불변 조건을 유지한다. 수신 wrapper 재활용은 외부에 전달된 Message 참조를 새 메시지로 재활성화하는 문제가 있어 적용하지 않았다. 공개 caller-owned Message storage API를 추가하지 않았다.
- 새 spec gap: 적용한 변경에서는 발견하지 않았다. **전체 SEND 무할당은 아직 미달**이며 builder 객체·retry native attempt storage·public receive wrapper 비용은 남는다.

## 검증

- `bindings/go/tests/run_tests.sh`: PASS — go test ./..., go vet ./..., guards, samples 7/7.
- native/perf 관련 테스트 5회: PASS. 공개 SEND·REQUEST·MoveMessage·multipart·completion 테스트 5회: PASS.
- native/perf 및 공개 API 관련 `-race`: PASS.
- shell lifecycle mock 3/3 PASS: CLIENT_DONE 뒤 server→client 종료 순서, client exit 7, server exit 7. 최종 결과가 있어도 비정상 exit는 partial/exit 1이다.
- 공개 API signature 변경 없음. 변경 production Go 파일의 exported function declaration 63건과 root API 변경 여부를 비교했다(`go-pass1b-public-api-check.json`). 기존 ownership failure assertion 완화 없음.
- 최종 gate: `go-pass1b-resume-gate.log`; 관련 5회: `go-pass1b-resume-related-5.log`; race: `go-pass1b-resume-race.log`; protocol: `go-pass1b-control-test.log`. pass 1b 기존 public 소유권 5회와 race 로그도 보존했다.

## 측정 조건

- 1분 load ≤ 3에서만 profile/bench를 시작한다. 각 시작 load와 대기는 `go-perf-pass1b-progress.md`에 기록한다.
- pass 1 Core SHA-256은 `232ef7650b9276e281a96652d90f9c915d980e40e6294a9b396583804aaccddc`, 현재 Core는 `d4b95b10ce3f96315740de20d2ea4e1d1d40f3dffd7a50de5530d26dff9a3f00`이다. pass 1/C 역사값과의 비교는 새 paired 판정이 아니다.
- 동일 현재 Core에서 old-runner / runner-only / binding-after를 분리해 측정한다. baseline binding은 pass 1 커밋의 변경 대상 파일을 Go build overlay로 고정했으며 작업 트리를 전환하지 않았다.

## SEND 할당 계측

`go-pass1b-profiles/{before,after}.log`와 alloc pprof는 동일 Core에서 load 2.67에 시작했다. 2-part PAIR inproc 왕복 10,000회, `MoveMessage(body).Message(empty-tail)`, receive/close를 포함한다. B/op와 allocs/op는 Go heap 계수이며 native malloc은 포함하지 않는다. 이 값은 SEND 단독 비용이나 공식 multi throughput이 아니다.

| 메시지 크기 | before B/op | after B/op | before allocs/op | after allocs/op | before ns/op | after ns/op | cgo/op 전/후 |
|---|---:|---:|---:|---:|---:|---:|---:|
| 64 B | 1216 | 848 | 20 | 16 | 35479 | 29407 | 23 / 23 |
| 65536 B | 1216 | 848 | 20 | 16 | 42279 | 36835 | 23 / 23 |

할당량은 30.3%, 할당 횟수는 20% 줄었다. `after.allocs.top.txt`의 alloc-space는 retry payload 24.06%, receive 22.23%, builder 14.81%, native multipart attempt storage 14.81%다. native RID 없는 경로의 256B allocation과 두 번째 builder part의 slice 확장, retry part별 개별 wrapper allocation을 줄인 결과다. cgo 횟수는 줄지 않았다.

## 추가 parity 확인과 남은 실패

- **DD 64B benchmark 종료 실패**: 19:40 최초 after와 20:35 재개 after에서 발생했다. 재개 after는 19/20, partial/exit 1이다. 재개 stack은 `perf_multi_dealer_dealer.go:82`의 active recv drain에 있었고, 이전 진단 stack은 같은 파일 `:138`의 tail drain이었다. Shell은 client 종료 뒤 기존 5초 server shutdown 기한에서 서버를 종료한다. DD의 내부 recv drain에는 StopAt 검사나 stdin STOP 관찰이 없고 첫 socket의 wire stop 한 건만 종료 근거로 사용한다. 같은 Core/pass 1 binding baseline은 64B 272.166k msg/s로 종료했으므로 native API 실패로 단정하지 않는다. 이번 REQREP 수정과 별개의 기존 DD phase/drain 문제이며, timeout 증액·drain 단축·HWM 또는 in-flight 제한으로 숨기지 않았다. 실패 원시 로그는 `go-pass1b-after-raw/`, `go-pass1b-dd-diagnostic-raw/`, `go-pass1b-resume-after-raw/`에 보존했다.
- **raw reply의 C 오류 경로 차이**: 정상 경로는 ROUTER POLLIN→multipart recv→opaque token/RID 확인→native Message direct reply로 같다. 그러나 C `perf_multi_socket_reqrep.hpp:823-839`의 2-part reply는 FINAL backpressure 뒤 empty FINAL만 반복한다. Core spec `core/doc/spec/core/socket/README.ko.md:1093-1102`는 실패 sequence의 staging/check-out 정리 후 전체 reply를 처음부터 재시도하도록 정한다. Go `socket_routed.go:27-30`은 canonical synchronous reply 실패를 반환하며 perf가 이를 fatal로 처리한다. 이 경계까지 C와 동일하다고 주장하지 않는다. 잘못된 C tail-only 재제출을 복제하거나 retry 횟수를 추가하지 않았다. 최종 REQREP raw log에는 reply 오류가 없고, REQUEST timeout만 2건이다. C 소스는 수정 범위 밖이다.
- **전체 SEND 무할당 미달**: public builder, retry snapshot, native attempt storage, 수신 wrapper 비용이 남는다. snapshot을 제출 실패 이후로 옮기면 MoveMessage/preparation 실패 소유권과 native 소비 경계를 함께 재설계해야 한다. public wrapper pool은 기각 대상이며 caller storage 새 API는 추가하지 않았다.
- **Spec gap**: 적용한 Go 변경에 새 gap 없음. 위 C reply 오류 경로는 spec 누락이 아니라 구현 parity 차이다. Go terminal 계약 승인 e17b0739a7을 따르며 spec/doc는 수정하지 않았다.

## 측정 자산과 제한

- 최종 공식 after는 요청한 4 patterns × 5 sizes × TCP × 100 clients × 5초 × 1 run이다. script는 `BASH_ENV`의 외부 load gate로 각 case 시작 load ≤ 3을 확인한다. DEBUG function 진입 때문에 같은 case 시작 로그가 두 번 기록될 수 있으며, 두 실행을 의미하지 않는다. 최종 repository source에는 load gate·SIGQUIT·임시 logger가 없다.
- `go-pass1b-final-measure.sh`는 측정 재현 명령과 산출물 경로를 보존한다. Core SHA-256은 `go-pass1b-final-core-hashes.txt`, pass 1 baseline overlay의 git 5e7e68fa6a 일치는 `go-pass1b-baseline-check.json`으로 확인했다.
- 종료 protocol 수정 전 전체 after도 `reports/go-perf-pass1b-resume-after.txt`로 보존했다(19/20, DD 64B 실패). 최종 after와 더 유리한 셀을 골라 합치지 않는다.
- 최종 runner-only는 10/10 complete다. 이 실행의 raw stderr 보존 hook이 EXIT cleanup에서 실행되지 않아 최종 runner-only timeout 계수는 N/A다. 19:38 runner-only의 raw 계수는 별도 보존되어 있으며, 다른 실행의 계수를 최종값으로 대체하지 않는다. 최종 after는 열린 raw log descriptor를 유지해 cleanup 뒤에도 원본을 보존한다.
- 새 paired C 측정은 하지 않았다. 지정한 `p1go` 역사값과 pass 1 after를 전 크기 표에 남기고, 실패 셀이 있는 pattern은 일부 크기만으로 평균을 산출하지 않는다.

## before / C 대비

Throughput은 k ops/s(REQREP), k msg/s(one-way)다. C와 pass 1은 다른 Core artifact의 역사값이므로 참고 비교다. REQREP의 pass 1 timeout은 전 case 0%; C는 계수가 없어 N/A다.

| Pattern | B | C | pass 1 after | pass 1b after | after/before | after/C | timeout/attempt |
|---|---:|---:|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 1003.84 | 350.84 | FAIL | N/A | N/A | N/A |
| DEALER_DEALER | 256 | 793.09 | 372.50 | 332.70 | 0.893x | 42.0% | N/A |
| DEALER_DEALER | 1024 | 481.33 | 309.81 | 291.03 | 0.939x | 60.5% | N/A |
| DEALER_DEALER | 4096 | 164.84 | 233.34 | 227.59 | 0.975x | 138.1% | N/A |
| DEALER_DEALER | 65536 | 45.09 | 100.19 | 99.32 | 0.991x | 220.3% | N/A |
| DEALER_ROUTER_REQREP | 64 | 112.47 | 3.88 | 24.88 | 6.411x | 22.1% | 0/124479 (0.000%) |
| DEALER_ROUTER_REQREP | 256 | 119.99 | 3.78 | 25.65 | 6.786x | 21.4% | 0/128360 (0.000%) |
| DEALER_ROUTER_REQREP | 1024 | 97.44 | 3.70 | 24.56 | 6.638x | 25.2% | 0/122910 (0.000%) |
| DEALER_ROUTER_REQREP | 4096 | 92.66 | 3.62 | 24.25 | 6.698x | 26.2% | 0/121326 (0.000%) |
| DEALER_ROUTER_REQREP | 65536 | 13.27 | 2.70 | 12.48 | 4.622x | 94.0% | 0/62502 (0.000%) |
| ROUTER_ROUTER_REQREP | 64 | 96.98 | 3.58 | 24.65 | 6.885x | 25.4% | 0/123343 (0.000%) |
| ROUTER_ROUTER_REQREP | 256 | 99.43 | 3.76 | 25.31 | 6.730x | 25.5% | 0/126625 (0.000%) |
| ROUTER_ROUTER_REQREP | 1024 | 90.23 | 3.52 | 24.25 | 6.889x | 26.9% | 0/121354 (0.000%) |
| ROUTER_ROUTER_REQREP | 4096 | 73.66 | 3.28 | 23.32 | 7.111x | 31.7% | 0/116725 (0.000%) |
| ROUTER_ROUTER_REQREP | 65536 | 13.30 | 2.68 | 12.21 | 4.558x | 91.8% | 2/61176 (0.003%) |
| PUBSUB | 64 | 479.36 | 301.46 | 324.66 | 1.077x | 67.7% | N/A |
| PUBSUB | 256 | 519.76 | 336.52 | 308.50 | 0.917x | 59.4% | N/A |
| PUBSUB | 1024 | 559.19 | 349.64 | 254.56 | 0.728x | 45.5% | N/A |
| PUBSUB | 4096 | 489.03 | 346.71 | 313.22 | 0.903x | 64.0% | N/A |
| PUBSUB | 65536 | 47.45 | 74.02 | 78.82 | 1.065x | 166.1% | N/A |

## 러너 / binding 분리

아래 대조는 동일 현재 Core에서 실행했다. runner-only/old는 goroutine·서버 poll/drain·집계 수정 전체의 효과다. after/runner-only만 추가 binding 변경의 관측 차이다. 1 run이므로 작은 차이를 확정 효과로 해석하지 않는다.

| Pattern | B | 순차 러너 k/s | runner-only k/s | binding-after k/s | 러너 배율 | binding 배율 | after timeout |
|---|---:|---:|---:|---:|---:|---:|---:|
| DEALER_ROUTER_REQREP | 64 | 3.74 | 24.48 | 24.88 | 6.55x | 1.016x | 0.000% |
| DEALER_ROUTER_REQREP | 256 | 3.76 | 24.57 | 25.65 | 6.53x | 1.044x | 0.000% |
| DEALER_ROUTER_REQREP | 1024 | 3.66 | 23.63 | 24.56 | 6.46x | 1.039x | 0.000% |
| DEALER_ROUTER_REQREP | 4096 | 3.48 | 22.33 | 24.25 | 6.42x | 1.086x | 0.000% |
| DEALER_ROUTER_REQREP | 65536 | 2.70 | 12.56 | 12.48 | 4.65x | 0.994x | 0.000% |
| ROUTER_ROUTER_REQREP | 64 | 3.48 | 24.50 | 24.65 | 7.04x | 1.006x | 0.000% |
| ROUTER_ROUTER_REQREP | 256 | 3.38 | 24.31 | 25.31 | 7.19x | 1.041x | 0.000% |
| ROUTER_ROUTER_REQREP | 1024 | 3.42 | 23.29 | 24.25 | 6.81x | 1.041x | 0.000% |
| ROUTER_ROUTER_REQREP | 4096 | 2.38 | 20.00 | 23.32 | 8.40x | 1.166x | 0.000% |
| ROUTER_ROUTER_REQREP | 65536 | 1.50 | 11.53 | 12.21 | 7.68x | 1.060x | 0.003% |

## latency 비교

report의 latency 단위는 ms다. REQREP는 실제 측정한 RTT/2이며 100/throughput으로 유도한 값이 아니다.

| Pattern | B | C mean ms | before mean ms | after mean ms | after p95 ms | after p99 ms |
|---|---:|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 25.306 | 1407.463 | FAIL | N/A | N/A |
| DEALER_DEALER | 256 | 3.877 | 1470.641 | 1542.268 | 3254.702 | 3879.672 |
| DEALER_DEALER | 1024 | 124.459 | 1976.960 | 2080.046 | 2943.933 | 3325.160 |
| DEALER_DEALER | 4096 | 1279.082 | 964.774 | 953.647 | 1297.978 | 1450.734 |
| DEALER_DEALER | 65536 | 196.261 | 13.056 | 14.337 | 30.406 | 36.409 |
| DEALER_ROUTER_REQREP | 64 | 2.562 | 0.128 | 1.937 | 8.145 | 15.464 |
| DEALER_ROUTER_REQREP | 256 | 2.745 | 0.132 | 1.871 | 7.943 | 16.773 |
| DEALER_ROUTER_REQREP | 1024 | 2.622 | 0.135 | 1.950 | 8.232 | 17.443 |
| DEALER_ROUTER_REQREP | 4096 | 3.408 | 0.138 | 1.883 | 11.453 | 25.393 |
| DEALER_ROUTER_REQREP | 65536 | 4.865 | 0.182 | 3.162 | 11.932 | 21.910 |
| ROUTER_ROUTER_REQREP | 64 | 1.615 | 0.139 | 1.947 | 8.064 | 16.507 |
| ROUTER_ROUTER_REQREP | 256 | 1.690 | 0.133 | 1.895 | 8.005 | 16.995 |
| ROUTER_ROUTER_REQREP | 1024 | 1.575 | 0.142 | 1.951 | 8.542 | 21.296 |
| ROUTER_ROUTER_REQREP | 4096 | 2.070 | 0.152 | 1.957 | 13.022 | 24.658 |
| ROUTER_ROUTER_REQREP | 65536 | 4.608 | 0.183 | 3.150 | 12.141 | 24.061 |
| PUBSUB | 64 | 1311.020 | 2515.251 | 2500.950 | 4300.097 | 4441.941 |
| PUBSUB | 256 | 1514.987 | 2348.204 | 2506.345 | 4270.085 | 4415.049 |
| PUBSUB | 1024 | 1415.339 | 2110.745 | 2464.292 | 3926.758 | 4163.156 |
| PUBSUB | 4096 | 528.172 | 749.682 | 824.475 | 1015.364 | 1069.088 |
| PUBSUB | 65536 | 295.881 | 170.505 | 131.060 | 313.367 | 330.774 |

## 패턴별 참고 처리량 비율

| Pattern | after/before 평균 | after/C 평균 |
|---|---:|---:|
| DEALER_DEALER | N/A (failed case) | N/A (failed case) |
| DEALER_ROUTER_REQREP | 6.23x | 37.8% |
| ROUTER_ROUTER_REQREP | 6.43x | 40.2% |
| PUBSUB | 0.94x | 80.5% |

## 최종 판정과 산출물

- 결과: `reports/perf_go_multi_linux_20260905_204547.txt`, alias `reports/go-perf-pass1b-after.txt`. 공식 after를 수정 없이 복사했다. `go-pass1b-final-after-raw/`에는 20 case log와 합산 log를 보존했다.
- 추가 binding 효과는 final runner-only 대비 DR의 각 크기에서 0.994~1.086x, RR에서 1.006~1.166x다. 단일 실행의 관측값이며, goroutine 러너 개선 4.65~8.40x와 합산해 library 효과로 제시하지 않는다.
- 처리량 목표: REQREP 평균 C 대비 53% 미달. DD 64B 실패로 전체 평균 N/A이고 DD 256B는 42.0%로 최소 55% 미달이다. PUBSUB 평균 80.5%이나 1024B 45.5%이며 역사 C와 새 paired pass를 확정하지 않는다. 실패 셀을 제외한 DD 평균이나 유리한 이전 값을 채운 평균을 산출하지 않았다.
- 남은 실패: DD 64B server shutdown 실패; RR 65536B REQUEST timeout 2건. 기능 gate 실패 없음. 전체 SEND 무할당, DD phase/drain, C raw reply 오류 경로 parity는 위 제한에 남긴다.
- 검증: 전체 go test/vet·guards·samples 7/7, 관련 5회, 관련 race, shell protocol 3/3, `git diff --check`, `bash -n` PASS. public API 불변. 동일 current Core hash를 최종 before/after에서 확인했다.
- 소유 계층: binding은 builder·native payload storage·단일 completion owner, runner는 goroutine 실행·집계·프로세스 종료 protocol을 소유한다.
- Spec 조항: Core socket Part send/Reply(932~985, 1087~1102), Go ownership/blocking terminal(118~127, 287~294), PERF_MULTI_TEST_POLICY §1.2·§2.1, 감독 결정 e17b0739a7.
- 교차언어 대조: C의 completion-window/direct reply/CLIENT_DONE→server STOP→client STOP과 대조했다. Go goroutine은 승인된 blocking terminal의 suspension 표현이다. C 2-part reply의 backpressure 재시도 차이는 위에 남겼다.
- 분류: runner A+B, binding B. 규칙 수: server 응답 경로 2→1, 준비 gate 2→1; completion owner 1→1, multipart copy/cleanup 1→1. Native admission·재제출 소유자는 늘리지 않았다.
- 변경은 `bindings/go/**`에 한정한다. 기존 pass 1 변경과 Core symlink를 보존했다. spec/doc/core/다른 binding 수정, Core build/clean, commit/push/stash/checkout/reset 없음.

**EXIT:1** — 기능 검증은 통과했지만 공식 benchmark에 DD 64B 실패가 남아 있다.
