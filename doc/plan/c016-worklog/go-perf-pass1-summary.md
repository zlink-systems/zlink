# Go binding hot-path pass 1

**Pass 1 완료.** 최종 after 20/20 complete, Go test/vet·contract guard·샘플 7/7·관련 5회·race 통과. 기존 C 대비 참고 처리량 평균은 DD **102.0%**, DR REQREP **6.9%**, RR REQREP **7.2%**, PUBSUB **83.4%**이다. DD 소형/지연 및 REQREP 목표는 충족하지 못했다. 새 paired C 판정은 하지 않았다.

- 기준: detached `f58514c9959c3ee20eca9b0ce05a694b1fd8ef65`; 작업 경로 `/home/hep7hep7/project/zlink-wt-go-perf`. commit/push/checkout/reset/stash 및 Core build 실행 없음.
- Core: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.17.0`, SHA-256 `232ef7650b9276e281a96652d90f9c915d980e40e6294a9b396583804aaccddc`. before/after 동일 artifact.
- 측정: multi TCP, 100 clients, 64/256/1024/4096/65536B, 5초, 1 run, 기본 2 parts, 기존 HWM·scheduler·drain·fairness 유지. before는 각 case 시작 load ≤ 3, after는 자체 부하 상승 여유를 두어 ≤ 1.5에서 시작했다. case별 load와 대기는 `go-perf-pass1-progress.md`에 기록했다. C 비교값은 사용자가 지정한 기존 paired before이므로 부하 차이가 있는 참고 비율이며 새 paired 합격 판정이 아니다.

## 원인과 판정

| 위치 (기준 HEAD) | 측정·코드 근거 | 소유 계층 / 가이드 §2 | pass 1 처리 |
|---|---|---|---|
| `bindings/go/perf/multi/perf_multi_socket_reqrep.go:136,159-166` | 소켓 100개를 순회하며 각 `Submit` 완료 뒤 다음 요청. trace에서도 요청마다 channel wait 1회 | 러너 / PERF_MULTI_TEST_POLICY §1.2 | **별도 러너 blocker**. 100개 동시 요청 전제가 성립하지 않으므로 100/throughput으로 산출한 70ms는 요청 지연이 아니다. 러너 수정 없음 |
| `bindings/go/internal/native/completion_owner.go:375-389,466-559` | profile-before 요청 17,200건, runtime poller/goroutine 17,200회 생성. after 요청 18,900건에 100회 생성 | binding / §2.3 | 소켓 owner가 poller·goroutine을 보유. idle은 Go channel에서 대기하여 OS thread를 놓음. public poller handoff·shutdown join 유지 |
| `bindings/go/internal/native/dealer_router_request.go:249-270` | 즉시 성공 SEND에도 retry state·완료 entry·채널·cgo handle 생성 및 map 등록 | binding / §2.1 | nonblocking admission과 token 등록을 owner lock으로 연결. 토큰이 있을 때만 entry·채널·heap retry state 생성. 성공 SEND에 completion 자원 할당 없음. **payload·public builder 할당은 남음** |
| `bindings/go/internal/native/completion_owner.go:60-110` | cgo global handle map과 socket owner map에 같은 entry 중복 등록; lookup은 owner map만 사용 | binding / §2.1·2.2 | opaque 단조 증가 key와 기존 owner map 한 곳만 사용. key 재사용·entry pool 없음 |
| `bindings/go/internal/native/socket_multipart.go:151-178` | clone Message → 임시 native 배열 move → submit | binding / §2.1·2.4 | native 배열에 직접 copy. 전체 part 준비 뒤 submit, 오류 시 원본 보존 유지 |
| `bindings/go/internal/native/socket_multipart_receive.go:32-65`, `completion_owner.go:965` 주변 | 임시 native receive → wrapper adopt, completion init+move | binding / §2.4 | receive는 최종 Message storage로 직접 수신. completion은 uninitialized Message로 adopt |
| `bindings/go/internal/native/send_retry.go:14-18,66-86` | caller ownership 목록 별도 할당, `attemptMu`와 payload `sync.Once`가 같은 수명 보호 | binding / §2.2·2.5 | builder parts를 source ownership 목록으로 사용. payload 정리는 기존 attemptMu 한 곳에서 직렬화 |
| `bindings/go/internal/native/message.go:142-148` | RID를 복사할 때마다 cgo memcpy | binding / §2.4 | 동일 native byte storage로 Go copy |

## 비용 계측 (최초 바인딩 개선 기준)

CPU·alloc·block·mutex pprof 및 runtime trace는 `go-profile-before/`, `go-profile-after/`에 있다. 진단 source는 `reports/go-pass1-diagnostics-source.tar.gz`; 임시 hook·계수 코드는 최종 tree에서 제거했다. pprof/trace 실행값은 공식 처리량 판정과 분리한다.

### REQUEST (공식 러너, 64B + empty tail)

| 지표 | C | Go before | Go after |
|---|---:|---:|---:|
| client cgo calls / request (startup 포함) | 0 (native C) | 21.0584 | 16.0657 |
| runtime poller/goroutine 생성 / request | 0 Go goroutine | 1.0000 | 0.0053 (소켓 100개) |
| client goroutine Runnable→Running / request (startup 포함) | 0 | 2.0317 | 2.0390 |
| request caller channel block / request | 0 | 1.0000 | 1.0000 |
| profile run 완료 request | 별도 측정 아님 | 17,200 | 18,900 |

`go-trace-counts.json`은 event/stack을 세어 만든 원시 계수이다. goroutine 전환 수는 OS context switch 수가 아니다. 전체 서버·client를 합친 값도 아니다. CPU profile-before client의 flat cgocall 43.8%, futex 29.8%; mutex 지연은 전체 약 213µs로 70ms/request를 설명하지 않는다. block profile에는 프로파일러 자체의 대기도 포함되므로 전체 합계를 request 지연으로 쓰지 않는다.

### 요청 수명 (별도 public API TCP 재현, 100 sockets 순차, 1 part 64B, 1,000회)

| 평균 단계 | before | after |
|---|---:|---:|
| 제출 → native admission 반환 | 22.632µs | 20.597µs |
| admission 반환 → 서버 수신 반환 | 58.313µs | 56.564µs |
| 서버 수신 반환 → reply 제출 반환 | 14.987µs | 14.745µs |
| reply 제출 반환 → client completion capture | 54.528µs | 53.662µs |
| capture → caller 반환 | 14.457µs | 12.553µs |
| 합계 | 164.917µs | 158.121µs |
| 최대 end-to-end | 1.205ms | 0.877ms |
| client+server cgo calls/op | 21.00 | 15.20 |
| client+server Go heap B/op | 3,454.00 | 2,596.94 |
| client+server Go heap alloc/op | 37.91 | 28.91 |

admission은 Core 내부 시점이 아니라 public native submit의 성공 반환 시점이다. reply/완료 진행은 다른 스레드에서 겹칠 수 있다. 계측한 시점과 기존 before만으로 70ms 고정 대기를 확정할 근거는 없다.

### SEND (inproc pair, body + empty tail, 10,000회, 생성·수신·정리 포함)

| 크기 | 지표 | C public API 진단 | Go before | Go after |
|---|---|---:|---:|---:|
| 64B | Go heap B/op | 해당 없음 | 2,352 | 1,217 |
| 64B | Go heap alloc/op | 해당 없음 | 32 | 20 |
| 64B | native malloc/calloc/realloc B/op | 104.00 | 104.1 | 104.1 |
| 64B | native alloc calls/op | 1.00 | 1.001 | 1.001 |
| 64B | cgo calls/op | 0 | 29 | 23 |
| 65536B | Go heap B/op | 해당 없음 | 2,352 | 1,217 |
| 65536B | Go heap alloc/op | 해당 없음 | 33 | 20 |
| 65536B | native malloc/calloc/realloc B/op | 65,576 | 65,576 | 65,576 |
| 65536B | native alloc calls/op | 1.00 | 1.00 | 1.00 |
| 65536B | cgo calls/op | 0 | 29 | 23 |

native 할당은 동일 Core에 glibc allocation interceptor를 적용하여 구간 차이를 쟀다. Go heap 수치와 합쳐 제시하지 않았다. SEND 즉시 성공에는 완료 channel 왕복과 새 goroutine이 없다. 이 진단은 native 메모리 비용 분리가 목적이며 C와 Go 진단의 ns/op는 공식 벤치마크 비율로 사용하지 않는다.

## 변경과 검증

- 변경 파일: `bindings/go/internal/native/{completion_owner.go,completion_owner_test.go,dealer_router_request.go,message.go,poller_timer.go,poller_timer_test.go,request_reply_helpers.go,send_retry.go,socket_multipart.go,socket_multipart_receive.go,socket_multipart_test.go}`, `bindings/go/tests/raw-core11-allowlist.json`, `bindings/go/perf/multi/perf_multi_dealer_dealer.go`, `bindings/go/perf/run_benchmarks_multi.sh`.
- 공개 API signature·ownership·error contract: 유지. root alias·contracts·include diff 없음. Core/spec/doc/다른 binding diff 없음. 임시 러너 부하 gate와 실패 계수는 측정 종료 뒤 제거했다.
- `bindings/go/tests/run_tests.sh`: PASS (`go test ./...`, `go vet ./...`, raw contract/hot-path guards, samples **7/7**).
- 관련 테스트 `-count=5`: PASS. WRITABLE SEND/REQUEST 재제출·혼합, cancellation/close, ownership, completion join, 소켓 owner 재사용, 즉시 SEND allocation budget 포함.
- 관련 `go test -race`: PASS. payload 중복 Once 제거와 owner 재사용 경계를 확인했다.
- `git diff --check`: PASS (최종 cleanup 뒤 재확인).
- 소유 계층: binding의 completion owner/메시지 storage. Core의 admission·target 선택·retry credit 정책을 복제하지 않았다.
- spec: `core/doc/spec/core/socket/README.ko.md:932-985` Part send·대기 토큰; `bindings/doc/spec/go/README.ko.md:119-122,287-294` ownership·goroutine terminal·단일 drain.
- 교차언어: C++ pass1은 즉시 completion entry 제거와 선행 WRITABLE 보존, Java pass1은 runtime/wake 비용과 재사용 후보를 분리했다. Go는 Future 반환이 아닌 blocking goroutine terminal이므로 runner의 연속 admission 관찰 문제가 별도로 남는다.
- 변경 분류: **B 기존 binding 비용 결함**. REQREP 연속 제출 문제는 계약 적응 blocker로 남겼다. DD active deadline과 프로세스 실패 판정은 별도 B 러너 버그로 수정했다.
- 대안 비교: (1) 선행 completion 보류 map을 새로 추가하거나 (2) 기존 owner lock으로 DONTWAIT admission과 token 등록을 연결. 중복 상태 없는 (2)를 선택했다. poller idle timeout 증가 대신 소켓 자원을 유지하고 Go channel에서 대기한다.
- 수정 전/후 규칙 수: entry 등록 owner **2→1**, payload 종료 직렬화 장치 **3(attemptMu+Once 2개)→1**, clone staging **3→2 단계**, idle poller 폐기·재생성 규칙 **1→0**. 추가 timer·retry budget·새 poller consumer·wrapper pool 없음.

## 추가 진단: errno와 러너 측정 경계

큰 메시지 최초 after에서 DD 64KiB가 quiet before 대비 66.6k→31.3k로 낮아져 CPU 프로파일로 분리했다. 이 값들을 바인딩 회귀로 확정할 수 없는 기존 결함을 발견했다.

| 원인 | 직접 관측 | 소유 계층 / 수정 |
|---|---|---|
| `completion_owner.go`와 `Poller.Wait`가 native wait와 errno를 서로 다른 cgo 호출에서 읽음 | `go-large-errno/raw/result_data.log`: native errno **EINTR(4)** 688회 중 나중 errno가 **0인 경우 25회**, **11인 경우 159회**, 4인 경우 504회. 0을 EIO로 처리해 sender가 종료됨 | **binding B**. `nativePollerWait`의 C bridge가 count/result/errno를 같은 OS thread에서 한 번에 반환. public/runtime wait가 같은 경계를 사용 |
| DD server가 public poll의 no-event를 active 종료로 간주 | CPU profile 실행에서 서버 수신 **34건, invalid header 0** 뒤 나머지는 tail drain에서 버려짐. `go-large-receive/raw/`에 보존 | **runner B**. no-event이면 기존 outer loop의 `StopAt`을 다시 확인. duration·scheduler·drain·fairness 정책 변경 없음 |
| client 비정상 종료를 server RESULT 존재로 성공 처리 | before/after CPU profile client는 EIO로 종료했는데 shell report는 complete. `go-large-failure/raw/` | **runner B**, PERF_MULTI_TEST_POLICY §6.1·6.3. case/process 실패가 있으면 partial/exit 1. mock server RESULT + client exit 7 회귀 검증 결과 partial/exit 1 |

- errno 계약 근거: `core/doc/spec/core/03-errors.ko.md:20,280`은 errno가 호출 thread의 값임을 명시한다. `05-polling.ko.md:248-251`은 poll 반환·error_out 계약을 소유한다. C++ `completion_owner.cpp:754-767`은 같은 native thread에서 poll과 errno를 읽으므로 Go의 cgo 재진입 이동 문제가 없다.
- 최초 대용량 CPU profile(before/after) 및 수정 중 profile report는 원시 진단이며 처리량 채택 자료가 아니다. 원시 결과를 삭제하거나 최초 report 값을 덮어쓰지 않았다.
- errno와 DD deadline을 고친 뒤 CPU profile 실행은 **89,109.8 msg/s, client 정상 종료**. `go-large-final/run.log` 및 CPU/alloc/block/mutex 파일에 보존했다.
- 분리 대조: 원래 binding에 **동일한 errno·DD deadline 수정만 적용한** 진단 baseline은 **95,767.0 msg/s** (profiling off), `go-large-baseline-common-fixes.log`. 최초 66.6k 및 31.3k와 달리 같은 정상 측정 경계에서 비교할 수 있는 참고값이다. 이전 `go-large-baseline-corrected.log`는 shell만 수정하고 Go server의 조기 종료는 남았던 진단이므로 비교값에서 제외한다.
- raw C allowlist는 새 **local cgo bridge** 분류만 반영했다. 테스트 assertion, header hash, public Go API/packaged headers를 완화하지 않았다.
- 수정 후 관련 테스트 5회 및 race PASS; 전체 gate `go-final-gate.log` PASS, samples 7/7. 첫 gate의 local helper inventory 불일치는 분류 목록 갱신 후 해결했다.
- 추가 규칙 감소: poll errno 획득 경계 **2→1**, DD active 종료 결정 **no-event/StopAt 2→StopAt 1**. no-event를 숨기려 timeout을 늘리거나 새 timer/retry budget을 넣지 않았다.

## Spec gap 및 BLOCKERS

1. Core SEND/WRITABLE 계약에 새 gap은 발견하지 않았다. Go spec 문서의 Core 0.16.0 표기는 기존 상태이며 수정하지 않았다.
2. **REQREP 러너 정책 적응 blocker**: 현재 순차 terminal loop는 PERF_MULTI_TEST_POLICY §1.2를 위반한다. 공개 `RequestSubmitOp.Submit(ctx)`에는 admission 관찰·최종 completion을 분리하는 수단이 없다. 무제한 goroutine 제출은 admission보다 앞서 작업이 누적되고, socket당 한 goroutine은 고정 in-flight 상한이므로 둘 다 임의 적용하지 않았다. 공개 API 추가/변경 금지 범위에서 정책에 맞는 경로를 확인하지 못했다. 이 경계의 계약 검토가 필요하다.
3. **전체 SEND 무할당 미달**: 완료 entry·채널·handle 할당은 제거했으나 public builder, retry용 native payload snapshot 및 receive wrapper 할당은 남는다. MoveMessage와 multipart 실패 소유권을 보존한 추가 설계·측정이 필요하다.
4. 기존 C paired before는 Core job 부하와 겹쳤다. after/C는 참고 비율이며 새 paired 최종 통과 판정이 아니다. 본 pass 1만으로 목표/§7.4-11의 후속 Sol pass까지 완료했다고 판단하지 않는다.

5. DD 64/256B는 최종 C 대비 35.0/47.0%로 최소 55% 미달이며 latency 평균도 C의 90.321배다. REQREP뿐 아니라 DD도 전체 성능 통과로 닫지 않는다. queue 깊이를 제한하거나 fairness를 바꾸어 수치를 보상하지 않았다.

## 최초 after (18:17, errno·러너 버그 수정 전)

| Pattern | B | C before k/s | Go supplied before k/s | Go quiet before k/s | Go after k/s | after/quiet | after/C | latency after/C | timeout/attempt |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 1003.84 | 202.95 | 210.29 | 293.34 | 1.395x | 29.2% | 60.420x | N/A |
| DEALER_DEALER | 256 | 793.09 | 190.22 | 202.16 | 327.99 | 1.622x | 41.4% | 342.776x | N/A |
| DEALER_DEALER | 1024 | 481.33 | 106.46 | 108.99 | 250.23 | 2.296x | 52.0% | 17.285x | N/A |
| DEALER_DEALER | 4096 | 164.84 | 82.83 | 177.89 | 185.52 | 1.043x | 112.5% | 0.866x | N/A |
| DEALER_DEALER | 65536 | 45.09 | 18.45 | 66.60 | 31.26 | 0.469x | 69.3% | 0.083x | N/A |
| DEALER_ROUTER_REQREP | 64 | 112.47 | 1.52 | 3.38 | 3.84 | 1.136x | 3.4% | 0.051x | 0/19200 (0%) |
| DEALER_ROUTER_REQREP | 256 | 119.99 | 1.38 | 3.36 | 3.72 | 1.107x | 3.1% | 0.049x | 0/18600 (0%) |
| DEALER_ROUTER_REQREP | 1024 | 97.44 | 1.52 | 3.22 | 3.68 | 1.143x | 3.8% | 0.052x | 0/18400 (0%) |
| DEALER_ROUTER_REQREP | 4096 | 92.66 | 1.44 | 3.16 | 3.54 | 1.120x | 3.8% | 0.041x | 0/17700 (0%) |
| DEALER_ROUTER_REQREP | 65536 | 13.27 | 1.20 | 2.38 | 2.62 | 1.101x | 19.7% | 0.038x | 0/13100 (0%) |
| ROUTER_ROUTER_REQREP | 64 | 96.98 | 1.38 | 3.12 | 3.54 | 1.135x | 3.7% | 0.087x | 0/17700 (0%) |
| ROUTER_ROUTER_REQREP | 256 | 99.43 | 1.30 | 3.10 | 3.64 | 1.174x | 3.7% | 0.081x | 0/18200 (0%) |
| ROUTER_ROUTER_REQREP | 1024 | 90.23 | 1.24 | 3.02 | 3.52 | 1.166x | 3.9% | 0.090x | 0/17600 (0%) |
| ROUTER_ROUTER_REQREP | 4096 | 73.66 | 1.28 | 2.98 | 3.48 | 1.168x | 4.7% | 0.069x | 0/17400 (0%) |
| ROUTER_ROUTER_REQREP | 65536 | 13.30 | 1.06 | 1.82 | 2.66 | 1.462x | 20.0% | 0.040x | 0/13300 (0%) |
| PUBSUB | 64 | 479.36 | 148.43 | 253.68 | 275.67 | 1.087x | 57.5% | 1.897x | N/A |
| PUBSUB | 256 | 519.76 | 150.83 | 303.93 | 369.68 | 1.216x | 71.1% | 1.603x | N/A |
| PUBSUB | 1024 | 559.19 | 189.68 | 325.90 | 352.28 | 1.081x | 63.0% | 1.504x | N/A |
| PUBSUB | 4096 | 489.03 | 158.52 | 299.05 | 329.86 | 1.103x | 67.5% | 1.497x | N/A |
| PUBSUB | 65536 | 47.45 | 52.22 | 68.35 | 54.83 | 0.802x | 115.5% | 0.576x | N/A |


| Pattern | after/C aggregate (size 비율 산술평균) | latency after/C 평균 | after/quiet 평균 | 목표 | 판정 |
|---|---:|---:|---:|---:|---|
| DEALER_DEALER | 60.9% | 84.286x | 1.365x | 65% | 미달 |
| DEALER_ROUTER_REQREP | 6.8% | 0.046x | 1.121x | 53% | 미달 |
| ROUTER_ROUTER_REQREP | 7.2% | 0.074x | 1.221x | 53% | 미달 |
| PUBSUB | 74.9% | 1.415x | 1.058x | 65% | 참고 비율상 충족; paired 재검증 필요 |


- 결과 report: `reports/perf_go_multi_linux_20260905_181756.txt` (alias `reports/go-perf-pass1-after.txt`는 마지막 최종 report를 가리킨다), **최초 report success 20 / fail 0 / complete**. DD 행은 위 조기 종료 결함 때문에 최초 library-only 성능 판단 자료로 사용할 수 없다.
- 같은 Core의 저부하 Go before: `go-before-quiet.txt`, 원본 `perf_go_multi_linux_20260905_175817.txt`, **20/20 complete**.
- C supplied before: `perf_c_multi_linux_20260905_{174534,174651,174747,174844}_p1go.txt`; Go supplied before: `perf_go_multi_linux_20260905_{174606,174718,174814,174911}.txt`, 원본은 `/home/hep7hep7/project/zlink/bindings/{c,go}/perf/results/multi/report/`.
- 기존 before/C timeout 수치는 report에 없어 비율 변화는 **N/A**이다. after REQREP timeout/attempt는 임시 계수로 얻었고 모두 0%; one-way는 REQUEST timeout 적용 대상이 아니다.
- pprof before/after의 native hash와 공식 after hash가 같음을 최종 재확인했다. 최종 before/after 대비는 모든 size를 그대로 기록했으며 낮아진 셀을 제외하거나 유리한 반복값으로 바꾸지 않았다.


## 최종 after (errno·러너 수정 포함)

| Pattern | B | C before k/s | 최초 after k/s | 최종 after k/s | 최종/C | 최종 latency(ms) | latency 최종/C | timeout/attempt |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 1003.84 | 293.34 | 350.84 | 35.0% | 1407.463 | 55.618x | N/A |
| DEALER_DEALER | 256 | 793.09 | 327.99 | 372.50 | 47.0% | 1470.641 | 379.280x | N/A |
| DEALER_DEALER | 1024 | 481.33 | 250.23 | 309.81 | 64.4% | 1976.960 | 15.884x | N/A |
| DEALER_DEALER | 4096 | 164.84 | 185.52 | 233.34 | 141.6% | 964.774 | 0.754x | N/A |
| DEALER_DEALER | 65536 | 45.09 | 31.26 | 100.19 | 222.2% | 13.056 | 0.067x | N/A |
| DEALER_ROUTER_REQREP | 64 | 112.47 | 3.84 | 3.88 | 3.4% | 0.128 | 0.050x | 0/19400 (0%) |
| DEALER_ROUTER_REQREP | 256 | 119.99 | 3.72 | 3.78 | 3.2% | 0.132 | 0.048x | 0/18900 (0%) |
| DEALER_ROUTER_REQREP | 1024 | 97.44 | 3.68 | 3.70 | 3.8% | 0.135 | 0.051x | 0/18500 (0%) |
| DEALER_ROUTER_REQREP | 4096 | 92.66 | 3.54 | 3.62 | 3.9% | 0.138 | 0.040x | 0/18100 (0%) |
| DEALER_ROUTER_REQREP | 65536 | 13.27 | 2.62 | 2.70 | 20.3% | 0.182 | 0.037x | 0/13500 (0%) |
| ROUTER_ROUTER_REQREP | 64 | 96.98 | 3.54 | 3.58 | 3.7% | 0.139 | 0.086x | 0/17900 (0%) |
| ROUTER_ROUTER_REQREP | 256 | 99.43 | 3.64 | 3.76 | 3.8% | 0.133 | 0.079x | 0/18800 (0%) |
| ROUTER_ROUTER_REQREP | 1024 | 90.23 | 3.52 | 3.52 | 3.9% | 0.142 | 0.090x | 0/17600 (0%) |
| ROUTER_ROUTER_REQREP | 4096 | 73.66 | 3.48 | 3.28 | 4.5% | 0.152 | 0.073x | 0/16400 (0%) |
| ROUTER_ROUTER_REQREP | 65536 | 13.30 | 2.66 | 2.68 | 20.1% | 0.183 | 0.040x | 0/13400 (0%) |
| PUBSUB | 64 | 479.36 | 275.67 | 301.46 | 62.9% | 2515.251 | 1.919x | N/A |
| PUBSUB | 256 | 519.76 | 369.68 | 336.52 | 64.7% | 2348.204 | 1.550x | N/A |
| PUBSUB | 1024 | 559.19 | 352.28 | 349.64 | 62.5% | 2110.745 | 1.491x | N/A |
| PUBSUB | 4096 | 489.03 | 329.86 | 346.71 | 70.9% | 749.682 | 1.419x | N/A |
| PUBSUB | 65536 | 47.45 | 54.83 | 74.02 | 156.0% | 170.505 | 0.576x | N/A |

| Pattern | supplied before/C 평균 | 최종 after/C 평균 | latency 최종/C 평균 | 처리량 목표 | 판정 |
|---|---:|---:|---:|---:|---|
| DEALER_DEALER | 31.5% | 102.0% | 90.321x | 65% | 참고 처리량 평균 충족; paired 확정 아님; 64/256B 최소 55% 미달·지연 초과 |
| DEALER_ROUTER_REQREP | 2.9% | 6.9% | 0.045x | 53% | 미달 |
| ROUTER_ROUTER_REQREP | 2.8% | 7.2% | 0.074x | 53% | 미달 |
| PUBSUB | 47.3% | 83.4% | 1.391x | 65% | 참고 처리량 평균 충족; paired 확정 아님 |


최종 report는 `reports/perf_go_multi_linux_20260905_184915.txt` 및 `reports/go-perf-pass1-after.txt`이다. **success 20 / fail 0 / complete**. `go-final-raw/`에 모든 case의 server/client 출력을 보존했고 native 오류·panic·client 실패가 없는지 확인했다. 최종 REQREP 계수: **172500 attempts, 0 timeout, 0 failure**.

DD 64KiB의 바인딩 비용 분리 참고: 공통 errno/측정 경계 수정만 적용한 baseline **95.767k** → 최종 **100.185k msg/s (1.046x)**. 이 셀은 두 실행 모두 같은 수정된 측정 경계이며 profiling off이다. 최초 66.6k→31.3k를 바인딩 회귀로 단정하지 않는다. 다른 DD 셀의 최초/최종 차이는 runner 조기 종료 수정과 분리되지 않아 library-only 이득으로 합산하지 않는다.

최종 검증 로그: `go-final-gate.log`, `go-final-related-5.log`, `go-final-race.log`, `go-runner-exit-regression.log`. 최종 `git diff --check`, `bash -n` PASS. 남은 기능 테스트 실패 없음. REQREP의 연속 admission 관찰 경계와 전체 SEND 무할당은 위 BLOCKERS에 남겼다.


최종 source 정리에서 호출자가 사라진 `cloneParts`, `prepareMultipart`, `preparedMultipart.ptr/count/commit`과 중복 `closeConsumedParts`를 제거했다. packet 정리는 기존 `closeMessageSlice` 한 곳으로 모았다. 닫힌 multipart 입력의 native `InvalidHandle/EFAULT`와 앞선 source 보존을 회귀 테스트로 확인했다. 이 마지막 정리는 측정한 정상 경로의 동작을 바꾸지 않는다.

`go-public-api-check.json`으로 수정 범위와 exported function signature 불변을 확인했다. root alias/contracts/include diff는 0이다. 최종 임시 profiler·load gate·계수 코드는 남아 있지 않다. Core symlink와 기존 사용자 파일은 유지했다. Commit/push/reset/checkout/stash 실행 없음.
