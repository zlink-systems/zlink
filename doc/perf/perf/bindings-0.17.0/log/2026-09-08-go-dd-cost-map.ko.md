# Go Multi DEALER/DEALER 64 B 비용 지도

## 결론

고정 Core prefix에서 새로 짝지은 1-run 기준은 C 1.696 Mmsg/s, Go 385.694
Kmsg/s였다. 메시지당 시간은 각각 589.7 ns와 2,592.7 ns이며 잔여 격차는
2,003.0 ns/msg이다. Go/C 처리량은 22.7%, 평균 latency는 0.5868/0.0712 =
8.24배였다.

wall-time 기준으로 20%를 넘은 항목은 cgo 경계뿐이었다. 그러나 해당 경계를
만드는 native message 생성·복제·retry snapshot·수신 owner는 공개 `Message`
ownership과 blocking `Submit` terminal 계약에 묶인다. 경계 두 개를 private
bridge 하나로 합치는 실험은 microbench에서는 개선됐지만 실제 DD를 두 번 연속
회귀시켜 되돌렸다. 요청마다 만드는 goroutine/channel은 wall-time 격차의 15.7%로
20%에 못 미쳤고, worker 재사용 실험은 C turn cadence를 바꾸어 더 크게 회귀했다.
따라서 채택한 pass 1 코드 diff는 없다. 원본 Go 러너와 binding으로 모두 복구했다.

## 측정 조건과 기준

- 시각: 2026-09-08 13:05 KST 이후
- 환경: `tcp`, 100 clients, 64 B, duration 5 s, runs 1, part-count 2,
  auto-HWM balanced
- Core: `ZLINK_CORE_SOURCE=release`,
  `ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.3-alpha`
- 실제 runtime: 위 prefix의 `lib/libzlink.so.0.17.2`, revision
  `d6432ec4fa8c82787a0b35b4295b32e516af7440`, release tag
  `core/v0.17.3-alpha`
- 기준 티켓:
  `.artifacts/perf-queue/log/2-1788840174-26332-codex-go-dd-cost-map_paired_before_C_and_Go_64.log`
- 기준 report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_130514_go-dd-cost-before-c173.txt`,
  `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260908_130520_go-dd-cost-before-go173.txt`

계획서의 Core 0.17.2 수치는 비교에 사용하지 않았다. C와 Go는 6초 간격으로 같은
티켓에서 연속 실행했고, C report의 provenance와 Go runner가 출력한 runtime
경로·SHA를 확인했다.

## 요청당 비용 지도

아래 ns 기여율은 end-to-end 잔여 2,003.0 ns/msg를 분모로 한다. Go 프로세스가
여러 CPU에서 동작하므로 격리 wall-time과 CPU-time은 별도로 적었다. profile
항목은 표본 기반 근사이며 서로 중첩될 수 있다.

| 항목 | C | Go | 격차 기여 | 성격 |
|---|---:|---:|---:|---|
| end-to-end 메시지 시간 | 589.7 ns/msg | 2,592.7 ns/msg | 2,003.0 ns, 100% | 측정값 |
| native 경계 전환 | 언어 경계 0회 | 23 cgo/op(송신 18, 수신 5), `runtime.cgocall` flat 약 846.7 ns | 약 42.3% | runtime. 전환 자체는 내부이나 전환을 만드는 message ownership/retry 연산은 계약 |
| Go heap 할당 | 러너 steady-state 0 B/0회; Core 내부는 공통 기준 | binding send+recv 848 B/16회 + coordinator 32 B/1회 = 880 B/17회 | `mallocgc` 약 190 ns, 9.5% | 대부분 계약 객체·retry snapshot; coordinator 32 B만 러너 |
| runner goroutine/channel | 0회 handoff; 같은 스레드에서 DONTWAIT 제출/WRITABLE 처리 | goroutine 생성 1회 + 완료 channel 1회 = handoff 2회/msg | wall 315.1 ns, 15.7%; 총 CPU 659.2 ns, 32.9% | 러너 runtime, 공개 계약 아님 |
| 나머지 binding/Core/scheduler | C direct 호출 기준에 포함 | builder, native 작업 본체, poll/scheduler, 계측 | 약 651.2 ns, 32.5% | 혼합·미확인 |

할당 profile의 848 B/16회 안에서는 `newSendRetryPayload`가 bytes 24.5%,
`recvMultipart` 22.8%, send builder 15.1%, attempt clone 15.1%, 송신/수신
`Message` 생성이 각각 9.4%였다. 이 비율은 alloc-space 분해이며 시간 비율로
해석하지 않는다.

근거:

- coordinator bench 5회: 311.5~317.9 ns/msg, 중앙값 315.1 ns/msg,
  32 B/msg, 1 alloc/msg. 티켓
  `.artifacts/perf-queue/log/2-1788840284-28253-codex-go-dd-cost-map_coordinator_microbenchmar.log`
- coordinator CPU profile: 38,045,400 messages에 25.08 CPU-s = 659.2
  CPU-ns/msg. `runtime.newproc1`, channel, lock, scheduler가 상위였다.
- managed multipart 64 B bench: 중앙값 1,967 ns/op, 848 B/op,
  16 allocs/op, 23 cgo/op. 티켓
  `.artifacts/perf-queue/log/2-1788840294-28500-codex-go-dd-cost-map_managed_multipart_64B_all.log`
- managed CPU profile: 1,997 ns/op에서 `runtime.cgocall` flat 42.4%, cum
  78.0%, `mallocgc` cum 9.5%. 티켓
  `.artifacts/perf-queue/log/2-1788841041-78941-codex-go-dd-cost-map_managed_multipart_CPU_pro.log`
- host cgo call 중앙값: add-int 35.56 ns, one-pointer 41.86 ns. 송신 18회만
  최저값으로 계산해도 640.1 ns/msg, 잔여 격차의 32.0%다. 티켓
  `.artifacts/perf-queue/log/2-1788840739-60134-codex-go-dd-cost-map_host_cgo_boundary_nanosec.log`

## 지배 항목과 계약 경계

Go binding은 managed Send 전에 immutable retry snapshot을 만들고, 매 native
admission attempt에서 다시 clone한다. Core는 backpressure 때 packet 대신
WRITABLE token만 보존하므로 exact packet 재제출과 blocking public terminal을
지키려면 snapshot 자체를 없앨 수 없다. 근거는
`bindings/go/internal/native/send_retry.go:10-68`,
`bindings/go/internal/native/socket_multipart.go:90-120`,
`bindings/go/tests/hot-path-cost-inventory.json:35-42`다.

C는 `bindings/c/perf/multi/src/perf_multi_dealer_dealer_client.cpp:158-215`에서
같은 스레드로 native DONTWAIT submit을 호출하고, retained bytes와 token을 직접
소유한다. Go는 공개 blocking `Submit`을 별도 goroutine에서 호출해야 메인 poller가
WRITABLE/recv를 진행할 수 있다. 즉 C와 Go의 차이는 단순 함수 호출 수뿐 아니라
public terminal과 runner scheduling 경계의 결합이다.

## pass 1 실험과 판정

### A. cgo init+copy bridge — 기각, 복구

`zlink_msg_init`과 `zlink_msg_copy`를 private C helper 한 번으로 합쳤다. public API와
message 보존/retry snapshot 규칙은 바꾸지 않았다.

- micro before → after: 23 → 19 cgo/op, 1,967 → 1,782 ns/op(-9.4%),
  848 B/16 allocs는 동일
- end-to-end before: C 1,695,769.6, Go 385,693.6 msg/s; Go latency
  0.5868 ms
- 첫 after: C 1,710,064.8, Go 148,786.0 msg/s; Go latency 0.6475 ms
- 재확인 after: C 1,708,896.8, Go 157,593.4 msg/s; Go latency
  0.6179 ms

두 번 연속 실제 처리량이 59~61% 회귀했으므로 bridge를 전부 되돌렸다.

- micro after 티켓:
  `.artifacts/perf-queue/log/2-1788841370-92373-codex-go-dd-pass1_after_managed_multipart_64B_.log`
- paired after 티켓:
  `.artifacts/perf-queue/log/2-1788841380-92679-codex-go-dd-pass1_paired_after_C_and_Go_DD_plu.log`
- 재확인 티켓:
  `.artifacts/perf-queue/log/2-1788841705-13259-codex-go-dd-pass1_contradiction_recheck_paired.log`

### B. 소켓당 장기 worker — 기각, 복구

blocking Submit이 recv를 막지 않도록 소켓당 worker 1개를 두고 메시지를 연속
제출했다. 미admission 1 규칙은 지켰지만 100개 worker가 admitted queue를 서로
앞서 채워 C의 단일-thread cadence와 달라졌다.

- DD: C 1,696,731.6, Go 708,280.0 msg/s로 처리량은 올랐으나 Go latency가
  1,500.97 ms로 폭증
- DR SS: `server_shutdown_failed`
- 티켓:
  `.artifacts/perf-queue/log/2-1788842127-38936-codex-go-dd-worker-pass1_paired_after_C_and_Go.log`

### C. C round + worker 재사용 — 기각, 복구

round당 available socket 1회 dispatch는 유지하고 goroutine만 소켓별 worker로
재사용했다.

- coordinator micro: 315.1 → 276.7 ns/msg(-12.2%), 32 B/1 alloc → 0
- DD: C 1,709,486.6, Go 95,650.6 msg/s, Go latency 1.2768 ms
- DR SS: C 483,049.0, Go 265,370.0 ops/s(54.9%), Go latency
  399.58 ms(C 0.2059 ms의 1,941배)

격리 비용은 줄었지만 scheduler cadence가 바뀌며 실제 결과가 악화돼 전부
되돌렸다.

- micro 티켓:
  `.artifacts/perf-queue/log/2-1788842406-65258-codex-go-dd-worker-reuse_pass1_coordinator_aft.log`
- paired DD+DR SS 티켓:
  `.artifacts/perf-queue/log/2-1788842416-66858-codex-go-dd-worker-reuse_pass1_paired_after_C_.log`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_134047_go-dd-worker-reuse-after-c173.txt`,
  `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260908_134100_go-dd-worker-reuse-after-go173.txt`

## 남은 격차의 성격

- 계약: public `Message` ownership, failure 때 caller message 보존, Core가 token만
  보존하는 동안 exact multipart를 유지하는 retry snapshot, received wrapper.
- runtime: cgo 23회와 Go heap 16회, goroutine/channel scheduler 비용.
- 미확인: 동일 micro 비용을 줄여도 실제 DD 처리량이 역행한 원인과, worker의
  작은 cadence 변화가 DD/DR latency를 ms~s 단위로 증폭하는 지점. 추가 상태,
  timeout, in-flight cap으로 보상하지 않았다.

수정 전/후 규칙 수: 최종은 **변화 없음**. 소켓당 admission owner 1, C turn,
binding-owned exact retry packet이라는 기존 세 규칙을 유지했다.

## 검증

- `go test ./...` (`bindings/go/perf`): 통과
- `go vet ./...` (`bindings/go/perf`): 통과
- binding targeted retry/ownership tests: 통과
- binding 전체 `go test ./internal/native`: 선행 실패 —
  `TestRawCore11AllowlistMatchesHeadersAndCgo`에서 작업 전부터 존재한
  `bindings/go/include/zlink.h` SHA 불일치. 해당 header/allowlist는 수정하지 않았다.

성능·profile 실행은 모두 `perf-ticket.sh submit -p 2 -o codex`로 실행했다.
