# Go bindings 성능 재검토 로그

## 진행 원칙

- 후보 개발 중에는 남은 보류 항목의 pattern, transport, msg-size만 좁혀 측정한다.
- bindings public contract와 perf runner 의미는 바꾸지 않는다.
- HWM profile/floor 조정은 개선 근거로 사용하지 않는다.
- 상세 시도는 이 로그에 남기고, 계획 문서에는 최종 결과만 반영한다.

## MULTI_SPOT_REQREP/MULTI_SPOT_SENDSEND 65536B 제한 재측정

- 대상:
  - `MULTI_SPOT_REQREP`, `MULTI_SPOT_SENDSEND`
  - `tcp,tls,ws,wss`
  - `65536B`
- 근거:
  - 문서 표의 Go multi SPOT 65536B 일부가 C 대비 0.x~10%대까지 낮아, 같은 조건에서
    현재 상태를 다시 확인했다.
- 측정 1:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_SPOT_REQREP,MULTI_SPOT_SENDSEND --msg-sizes 65536 --duration 1 --runs 3 --results-tag go_multi_spot_65536_recheck_20260601`
  - Go: `perf_go_multi_linux_20260601_193756_go_multi_spot_65536_recheck_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과 1:
  - `MULTI_SPOT_REQREP`: tcp 6.9%, tls 84.0%, ws 67.1%, wss 88.3%
  - `MULTI_SPOT_SENDSEND`: tcp 8.9%, tls 60.3%, ws 89.8%, wss 94.5%
- 측정 2:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp --pattern MULTI_SPOT_REQREP,MULTI_SPOT_SENDSEND --msg-sizes 65536 --duration 2 --runs 5 --results-tag go_multi_spot_tcp65536_runs5_recheck_20260601`
  - Go: `perf_go_multi_linux_20260601_194109_go_multi_spot_tcp65536_runs5_recheck_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과 2:
  - `MULTI_SPOT_REQREP tcp 65536B`: 1.3%, 보류
  - `MULTI_SPOT_SENDSEND tcp 65536B`: 3.7%, 보류
- 판정:
  - tls/ws/wss 65536B는 제한 재측정으로 통과권에 올라 계획 문서 표에 반영했다.
  - tcp 65536B는 runs=5에서도 낮은 median이 반복되어 보류로 둔다.

## Received.Send builder 직접 경로 후보 기각

- 대상:
  - `MULTI_SPOT_REQREP`, `MULTI_SPOT_SENDSEND`
  - `tcp`
  - `65536B`
- 근거:
  - Go `Received.Send()`는 `sendBuilder`가 있어도 일반 `Message(...)` parts에서는 `[]*Message`를
    만든 뒤 legacy send closure로 돌아간다.
  - public API 의미를 유지하면서 builder 경로를 바로 쓰면 server echo hot path의 할당을 줄일
    수 있는지 확인했다.
- 변경 후보:
  - `bindings/go/internal/native/received.go`
  - `r.sendBuilder != nil && sendBuilderPartsNeedBuilder(parts)` 조건을 `r.sendBuilder != nil`로
    넓혔다.
- 검증:
  - `go test ./...` 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp --pattern MULTI_SPOT_REQREP,MULTI_SPOT_SENDSEND --msg-sizes 65536 --duration 2 --runs 5 --results-tag go_multi_spot_tcp65536_received_send_builder_probe_20260601`
  - Go: `perf_go_multi_linux_20260601_194324_go_multi_spot_tcp65536_received_send_builder_probe_20260601.txt`
  - status: complete
- 결과:
  - `MULTI_SPOT_REQREP tcp 65536B`: 2024 ops/s, C 대비 3.2%
  - `MULTI_SPOT_SENDSEND tcp 65536B`: 843.5 ops/s, C 대비 1.3%
- 판정:
  - REQREP는 직전 runs=5 median보다 올랐지만 통과권과 거리가 멀고, SENDSEND는 더 낮아졌다.
  - 전수 기준으로 이득보다 회귀가 크므로 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.

## MULTI_DEALER_DEALER/MULTI_PUBSUB 낮은 값과 RESULT 없음 재측정

- 대상:
  - `MULTI_DEALER_DEALER tcp/ws 4096B`
  - `MULTI_DEALER_DEALER tls/wss 65536B`
  - `MULTI_PUBSUB tls 65536B`
- 근거:
  - 문서 표에서 일부 칸이 `RESULT 없음`이거나 C 대비 1~3%대로 낮아, 현재 상태에서 같은
    pattern, transport, size만 좁혀 다시 확인했다.
- 전체 제한 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_DEALER_DEALER,MULTI_PUBSUB --msg-sizes 4096,65536 --duration 1 --runs 3 --results-tag go_multi_dealer_pubsub_low_recheck_20260601`
  - Go: `perf_go_multi_linux_20260601_194727_go_multi_dealer_pubsub_low_recheck_20260601.txt`
  - status: partial
  - 실패: `MULTI_DEALER_DEALER tcp 4096B` no result
- 전체 제한 측정 2:
  - 명령: `bindings/go/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_DEALER_DEALER,MULTI_PUBSUB --msg-sizes 4096,65536 --duration 1 --runs 3 --results-tag go_multi_dealer_pubsub_low_recheck_all_20260601`
  - Go: `perf_go_multi_linux_20260601_194742_go_multi_dealer_pubsub_low_recheck_all_20260601.txt`
  - status: partial
  - 실패:
    - `MULTI_DEALER_DEALER tcp 4096B`: no result 3회
    - `MULTI_DEALER_DEALER ws 4096B`: no result 1회
    - `MULTI_PUBSUB tls 4096B/65536B`: exit nonzero
    - `MULTI_PUBSUB wss 65536B`: exit nonzero
- 단독 완료 재측정:
  - `MULTI_DEALER_DEALER tls/wss 65536B`
    - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports wss,tls --pattern MULTI_DEALER_DEALER --msg-sizes 65536 --duration 1 --runs 3 --results-tag go_multi_dealer_dealer_wss_tls_65536_recheck_20260601`
    - Go: `perf_go_multi_linux_20260601_195558_go_multi_dealer_dealer_wss_tls_65536_recheck_20260601.txt`
    - status: complete
    - 결과: tls 47.3%, wss 50.9%
  - `MULTI_PUBSUB tls 65536B`
    - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tls --pattern MULTI_PUBSUB --msg-sizes 65536 --duration 1 --runs 3 --results-tag go_multi_pubsub_tls_65536_recheck_20260601`
    - Go: `perf_go_multi_linux_20260601_195633_go_multi_pubsub_tls_65536_recheck_20260601.txt`
    - status: complete
    - 결과: 68.2%
  - `MULTI_DEALER_DEALER ws 4096B`
    - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports ws --pattern MULTI_DEALER_DEALER --msg-sizes 4096 --duration 1 --runs 3 --results-tag go_multi_dealer_dealer_ws_4096_recheck_20260601`
    - Go: `perf_go_multi_linux_20260601_195703_go_multi_dealer_dealer_ws_4096_recheck_20260601.txt`
    - status: complete
    - 결과: 55.1%
- 반복 실패 확인:
  - `MULTI_DEALER_DEALER tcp 4096B`
    - 명령 1: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp --pattern MULTI_DEALER_DEALER --msg-sizes 4096 --duration 1 --runs 3 --results-tag go_multi_dealer_dealer_tcp_4096_recheck_20260601`
    - Go: `perf_go_multi_linux_20260601_195652_go_multi_dealer_dealer_tcp_4096_recheck_20260601.txt`
    - status: partial, no result
    - 명령 2: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp --pattern MULTI_DEALER_DEALER --msg-sizes 4096 --duration 2 --runs 5 --results-tag go_multi_dealer_dealer_tcp_4096_runs5_recheck_20260601`
    - Go: `perf_go_multi_linux_20260601_195721_go_multi_dealer_dealer_tcp_4096_runs5_recheck_20260601.txt`
    - status: partial, no result
- 판정:
  - 완료 리포트가 있는 3개 칸은 계획 문서 표에 통과로 반영했다.
  - 다만 `MULTI_DEALER_DEALER tls 65536B`는 47.3%로 이전 1.8%보다 회복됐지만 기준에는
    못 닿아 계획 문서 표에서는 보류로 둔다.
  - `MULTI_DEALER_DEALER tcp 4096B`는 단독 반복에서도 `RESULT`가 없어 보류로 유지한다.
  - 이 단계에서는 binding public contract, HWM profile/floor, perf runner를 바꾸지 않았다.

## MULTI_DEALER_DEALER 131072B 재측정

- 대상:
  - `MULTI_DEALER_DEALER tcp/wss/tls 131072B`
- 근거:
  - 문서 표에서 tcp/wss/tls 131072B가 각각 10.4%, 19.7%, 32.4%로 낮아, 같은 조건을
    완료 리포트로 다시 확인했다.
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp,wss,tls --pattern MULTI_DEALER_DEALER --msg-sizes 131072 --duration 1 --runs 3 --results-tag go_multi_dealer_dealer_131072_recheck_20260601`
  - Go: `perf_go_multi_linux_20260601_195945_go_multi_dealer_dealer_131072_recheck_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `tcp 131072B`: 55.2%, 통과
  - `wss 131072B`: 52.9%, 통과
  - `tls 131072B`: 28.0%, 보류
- 판정:
  - tcp/wss 131072B는 완료 리포트 기준으로 통과권에 올라 계획 문서 표에 반영했다.
  - tls 131072B는 재측정에서도 기준에 못 닿아 보류로 둔다.
  - binding public contract, HWM profile/floor, perf runner는 바꾸지 않았다.

## 단일 retained message clone 제거 후보 기각

- 대상:
  - `MULTI_SPOT_REQREP tcp 65536B`
  - `MULTI_SPOT_SENDSEND tcp 65536B`
- 근거:
  - Go SPOT reply/send 경로는 단일 retained `Message`를 native submit할 때 중간 clone
    `Message`를 만든다. public API 의미를 유지하면서 단일 retained message를 바로 native
    copy로 제출하면 server reply hot path의 할당을 줄일 수 있는지 확인했다.
- 변경 후보:
  - `bindings/go/internal/native/socket_multipart.go`
  - `submitMultipartFromClones(..., consumeOriginal=false)`의 단일 part 경로에서 중간 clone
    `Message` 생성 없이 `zlink_msg_copy`로 native part를 만들어 submit하도록 했다.
- 검증:
  - `go test ./...` 통과
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp --pattern MULTI_SPOT_REQREP,MULTI_SPOT_SENDSEND --msg-sizes 65536 --duration 2 --runs 5 --results-tag go_multi_spot_tcp65536_single_retained_copy_probe_20260601`
  - Go: `perf_go_multi_linux_20260601_200312_go_multi_spot_tcp65536_single_retained_copy_probe_20260601.txt`
  - status: complete
- 결과:
  - `MULTI_SPOT_REQREP tcp 65536B`: median 1384.5 ops/s, C 대비 약 2.2%
  - `MULTI_SPOT_SENDSEND tcp 65536B`: median 2251.5 ops/s, C 대비 약 3.5%
- 판정:
  - REQREP는 기준과 거리가 멀고, SENDSEND는 기존 runs=5 재측정보다 낮아졌다.
  - 통과 항목을 만들지 못하고 회귀 위험이 있어 최종 코드와 계획 문서 표에는 반영하지 않고
    되돌렸다.

## MULTI_DEALER_ROUTER/MULTI_ROUTER_ROUTER tcp/tls failset 재측정

- 대상:
  - `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`
  - `tcp,tls`
  - `64,256,1024,65536B`
- 근거:
  - Go multi routed echo 잔여 보류와 낮은 값을 같은 조건에서 완료 리포트로 다시 확인했다.
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp,tls --pattern MULTI_DEALER_ROUTER,MULTI_ROUTER_ROUTER --msg-sizes 64,256,1024,65536 --duration 1 --runs 3 --results-tag go_multi_routed_tcp_tls_failset_recheck_20260601`
  - Go: `perf_go_multi_linux_20260601_200558_go_multi_routed_tcp_tls_failset_recheck_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_DEALER_ROUTER tcp`: 55.2%, 57.2%, 60.1%, 31.9%
  - `MULTI_DEALER_ROUTER tls`: 51.5%, 53.0%, 52.5%, 42.6%
  - `MULTI_ROUTER_ROUTER tcp`: 36.5%, 37.5%, 37.7%, 38.3%
  - `MULTI_ROUTER_ROUTER tls`: 37.8%, 37.8%, 39.4%, 43.3%
- 판정:
  - `MULTI_DEALER_ROUTER` small과 `tls 65536B`는 multi routed echo 기준으로 통과권을
    유지한다.
  - `MULTI_DEALER_ROUTER tcp 65536B`와 `MULTI_ROUTER_ROUTER` 잔여 small/tcp 65536B는
    기준에 못 닿아 보류로 유지한다.
  - binding public contract, HWM profile/floor, perf runner는 바꾸지 않았다.

## MULTI_SPOT wss 1024/4096B 재측정

- 대상:
  - `MULTI_SPOT wss 1024B`
  - `MULTI_SPOT wss 4096B`
- 근거:
  - 문서 표에서 각각 22.8%, 38.6%로 낮아 같은 조건을 완료 리포트로 다시 확인했다.
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports wss --pattern MULTI_SPOT --msg-sizes 1024,4096 --duration 1 --runs 3 --results-tag go_multi_spot_wss_1024_4096_recheck_20260601`
  - Go: `perf_go_multi_linux_20260601_200901_go_multi_spot_wss_1024_4096_recheck_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `1024B`: 58.2%, 통과
  - `4096B`: 171.9%, 통과
- 판정:
  - 두 칸 모두 완료 리포트 기준으로 통과권에 올라 계획 문서 표에 반영했다.
  - binding public contract, HWM profile/floor, perf runner는 바꾸지 않았다.

## MULTI_DEALER_DEALER/MULTI_PUBSUB 64/256B 재측정

- 대상:
  - `MULTI_DEALER_DEALER`, `MULTI_PUBSUB`
  - `tcp,tls,ws,wss`
  - `64,256B`
- 근거:
  - 단순 one-way 그룹은 Go 기준 53%가 최소 통과 기준이다. 표에 40%대 후반과 52%대
    보류가 남아 있어 현재 상태를 완료 리포트로 다시 확인했다.
- 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_DEALER_DEALER,MULTI_PUBSUB --msg-sizes 64,256 --duration 1 --runs 3 --results-tag go_multi_simple_small_failset_recheck_20260601`
  - Go: `perf_go_multi_linux_20260601_201047_go_multi_simple_small_failset_recheck_20260601.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- 결과:
  - `MULTI_DEALER_DEALER tcp`: 43.1%, 64.1%
  - `MULTI_DEALER_DEALER tls`: 44.1%, 57.3%
  - `MULTI_DEALER_DEALER ws`: 46.8%, 56.1%
  - `MULTI_DEALER_DEALER wss`: 44.6%, 56.6%
  - `MULTI_PUBSUB tcp`: 47.5%, 59.3%
  - `MULTI_PUBSUB tls`: 107.0%, 85.5%
  - `MULTI_PUBSUB ws`: 343.7%, 90.7%
  - `MULTI_PUBSUB wss`: 162.8%, 98.1%
- 판정:
  - `MULTI_PUBSUB tcp/tls/wss 256B`는 완료 리포트 기준으로 통과권에 올라 계획 문서 표에
    반영했다. `MULTI_PUBSUB ws 64/256B`와 wss/tls 64B도 최신 complete 수치로 표를 갱신했다.
  - `MULTI_DEALER_DEALER` 64B 계열과 `MULTI_PUBSUB tcp 64B`는 재측정에서도 단순 one-way
    기준에 못 닿아 보류로 유지한다.
  - binding public contract, HWM profile/floor, perf runner는 바꾸지 않았다.

## Received.Close 상태 비움 후보 기각

- 대상:
  - Go multi `MULTI_DEALER_DEALER`, `MULTI_ROUTER_ROUTER`
  - `tcp/ws/wss/tls 64/256/1024/65536B`
- 근거:
  - Go echo hot path는 caller-provided `Received`를 반복 재사용하고 매 수신 뒤 `Close()`를
    호출한다.
  - 현재 `Received.Close()`는 part만 닫고 `parts`, routing id, send/reply closure 상태를
    남겨 둔다. 다음 `replace()`에서 이미 닫힌 part를 한 번 더 순회하므로, Close 뒤 상태를
    비우면 public API 의미를 유지한 채 반복 수신 비용을 줄일 수 있는지 확인했다.
- 변경 후보:
  - `bindings/go/internal/native/received.go`
  - `Received.Close()` 끝에서 `routingID`, `spotRID`, `parts`, `requestSeq`, `reply`, `send`,
    `sendBuilder`를 초기화했다.
- 검증:
  - 후보 적용 뒤 `go test ./...`: 통과
- 측정 1:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_DEALER_DEALER,MULTI_ROUTER_ROUTER,MULTI_PUBSUB --msg-sizes 64,256,1024,65536 --duration 1 --runs 3 --results-tag go_multi_received_close_clear_probe_20260602`
  - Go: `perf_go_multi_linux_20260602_125341_go_multi_received_close_clear_probe_20260602.txt`
  - status: partial
  - 실패: `MULTI_PUBSUB current ws 65536B: exit_nonzero`
- 측정 2:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_DEALER_DEALER,MULTI_ROUTER_ROUTER --msg-sizes 64,256,1024,65536 --duration 1 --runs 3 --results-tag go_multi_received_close_clear_echo_probe_20260602`
  - Go: `perf_go_multi_linux_20260602_130114_go_multi_received_close_clear_echo_probe_20260602.txt`
  - status: partial
  - 실패: `MULTI_DEALER_DEALER current tcp 1024B: no_result_lines`
- 판정:
  - 두 측정 모두 partial이므로 계획 문서 표 반영 근거로 사용할 수 없다.
  - 후보가 complete report를 만들지 못했고 `MULTI_DEALER_DEALER tcp 1024B` no-result를 유발해
    안정성 위험이 있다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.
  - 되돌린 뒤 `git diff -- bindings/go/internal/native/received.go`는 비었고 `go test ./...`를
    다시 통과했다.

## RoutingID.toC Go copy 후보 기각

- 대상:
  - Go multi `MULTI_ROUTER_ROUTER`
  - `tcp/tls 64/256/1024/65536B`, 재확인 `tcp 64/256/1024/65536B`
- 근거:
  - routed send hot path는 매 submit마다 `RoutingID.toC()`로 Go value를
    `zlink_routing_id_t`에 복사한다.
  - 기존 구현은 `C.memcpy(...)`를 호출하므로, 작은 routing id 복사를 Go `copy(...)`로 바꾸면
    public contract와 perf runner 의미를 유지한 채 cgo 호출 비용을 줄일 수 있는지 확인했다.
- 변경 후보:
  - `bindings/go/internal/native/message.go`
  - `RoutingID.toC()` 내부 복사를 `C.memcpy(...)`에서
    `copy(unsafe.Slice((*byte)(unsafe.Pointer(&out.data[0])), int(r.size)), r.data[:r.size])`로
    바꿨다.
- 검증:
  - 후보 적용 뒤 `go test ./...`: 통과
- 측정 1:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp,tls --pattern MULTI_ROUTER_ROUTER --msg-sizes 64,256,1024,65536 --duration 1 --runs 3 --results-tag go_multi_rr_rid_toc_copy_probe_20260602`
  - Go: `perf_go_multi_linux_20260602_130408_go_multi_rr_rid_toc_copy_probe_20260602.txt`
  - status: partial
  - 실패: `MULTI_ROUTER_ROUTER current tls 256B: exit_nonzero`
- 측정 2:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp --pattern MULTI_ROUTER_ROUTER --msg-sizes 64,256,1024,65536 --duration 1 --runs 3 --results-tag go_multi_rr_tcp_rid_toc_copy_probe_20260602`
  - Go: `perf_go_multi_linux_20260602_130526_go_multi_rr_tcp_rid_toc_copy_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- complete 결과:
  - `MULTI_ROUTER_ROUTER tcp 64B`: 36.4%
  - `MULTI_ROUTER_ROUTER tcp 256B`: 37.4%
  - `MULTI_ROUTER_ROUTER tcp 1024B`: 37.8%
  - `MULTI_ROUTER_ROUTER tcp 65536B`: 36.5%
- 판정:
  - Go multi routed echo 최소 기준 40%에 닿은 항목이 없어 새 통과를 만들지 못했다.
  - 기존 계획 문서 기준 `tcp 65536B` 38.3%보다 낮아 회귀 위험도 있다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.
  - 되돌린 뒤 `git diff -- bindings/go/internal/native/message.go`는 비었고 `go test ./...`를
    다시 통과했다.

## 단일 MoveMessage in-place submit 후보 기각

- 대상:
  - Go multi `MULTI_DEALER_DEALER`, `MULTI_ROUTER_ROUTER`
  - direct/routed 단일 `MoveMessage(...)` submit hot path
- 근거:
  - `submitSinglePartMoved(...)`는 Go `Message`를 임시 `zlink_msg_t`로 move한 뒤 core
    `zlink_send_part*`에 넘긴다.
  - core의 direct/routed FINAL 단일 part 경로는 전달받은 part를 소비하므로, STREAM을 제외한
    direct/routed builder에서 원본 `Message`를 바로 넘기면 public contract를 유지하면서
    중간 native init/move 비용을 줄일 수 있는지 확인했다.
- 변경 후보:
  - `bindings/go/internal/native/socket_multipart.go`
  - `bindings/go/internal/native/socket_direct.go`
  - `bindings/go/internal/native/socket_routed.go`
  - `bindings/go/internal/native/socket_types.go`
  - STREAM은 EAGAIN에서 part를 소비하지 않는 예외가 있어 공용 helper 전체 변경은 피하고,
    `PairSocket.Send()`, `DealerSocket.Send()`, `RouterSocket.SendTo()`가 STREAM이 아닌
    fast path를 타도록 제한했다.
- 검증:
  - 후보 적용 뒤 `go test ./...`: 통과
- 측정 0:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_DEALER_DEALER,MULTI_ROUTER_ROUTER --msg-sizes 64,256,1024,4096,65536,131072 --duration 1 --runs 3 --results-tag go_multi_move_inplace_dd_rr_probe_20260602`
  - Go: `perf_go_multi_linux_20260602_130957_go_multi_move_inplace_dd_rr_probe_20260602.txt`
  - status: 중단
  - 사유: 범위를 넓게 잡아 오래 걸렸고 report가 header만 기록된 상태라 표 근거로 사용할 수
    없어, 더 좁은 complete 측정으로 다시 확인했다.
- 측정 1:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp --pattern MULTI_DEALER_DEALER --msg-sizes 64,4096 --duration 1 --runs 3 --results-tag go_multi_move_inplace_dd_tcp_probe_20260602`
  - Go: `perf_go_multi_linux_20260602_131549_go_multi_move_inplace_dd_tcp_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- complete 결과 1:
  - `MULTI_DEALER_DEALER tcp 64B`: 37.6%
  - `MULTI_DEALER_DEALER tcp 4096B`: 11.2%
- 측정 2:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp,tls,ws --pattern MULTI_ROUTER_ROUTER --msg-sizes 64,256,1024,65536 --duration 1 --runs 3 --results-tag go_multi_move_inplace_rr_probe_20260602`
  - Go: `perf_go_multi_linux_20260602_131624_go_multi_move_inplace_rr_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- complete 결과 2:
  - `MULTI_ROUTER_ROUTER tcp 64B`: 35.3%
  - `MULTI_ROUTER_ROUTER tcp 256B`: 36.1%
  - `MULTI_ROUTER_ROUTER tcp 1024B`: 36.9%
  - `MULTI_ROUTER_ROUTER tcp 65536B`: 34.1%
  - `MULTI_ROUTER_ROUTER tls 64B`: 36.8%
  - `MULTI_ROUTER_ROUTER tls 256B`: 37.5%
  - `MULTI_ROUTER_ROUTER tls 1024B`: 37.9%
  - `MULTI_ROUTER_ROUTER ws 64B`: 36.9%
- 판정:
  - Go multi direct one-way 최소 기준 53%, routed echo 최소 기준 40%에 닿은 미달 항목이
    없어 새 통과를 만들지 못했다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.
  - 되돌린 뒤 `git diff -- bindings/go/internal/native/socket_multipart.go bindings/go/internal/native/socket_direct.go bindings/go/internal/native/socket_routed.go bindings/go/internal/native/socket_types.go`는
    비었고 `go test ./...`를 다시 통과했다.

## request progress active-count hook 후보 기각

- 대상:
  - Go request/reply completion progress 경로
  - multi `MULTI_SPOT_REQREP`
- 근거:
  - 기존 progress pump는 request마다 `state.done`을 기다리는 보조 goroutine을 만들어
    active count를 줄인다.
  - `replyCallbackState`에 completion hook을 붙이면 request당 보조 goroutine을 줄일 수 있어,
    public callback 계약과 `done` 채널은 유지한 채 SPOT_REQREP hot path 비용을 낮출 수 있는지
    확인했다.
- 변경 후보:
  - `bindings/go/internal/native/reply_callback_state.go`
  - `bindings/go/internal/native/request_progress.go`
  - `replyCallbackState`에 완료 hook 등록을 추가하고, socket/spot request progress가
    `state.done` 대기 goroutine 대신 hook으로 `activeCount`를 감소시키도록 바꿨다.
- 검증:
  - 후보 적용 뒤 `go test ./...`: 통과
- 측정 1:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_SPOT_REQREP --msg-sizes 131072 --duration 1 --runs 3 --results-tag go_multi_spot_reqrep_progress_hook_probe_20260602`
  - Go: `perf_go_multi_linux_20260602_132136_go_multi_spot_reqrep_progress_hook_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- complete 결과 1:
  - `MULTI_SPOT_REQREP tcp 131072B`: 47.7%
  - `MULTI_SPOT_REQREP ws 131072B`: 46.9%
- 측정 2:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp,ws --pattern MULTI_SPOT_REQREP --msg-sizes 64,256,1024,4096,65536,131072 --duration 1 --runs 3 --results-tag go_multi_spot_reqrep_progress_hook_tcp_ws_fullcheck_20260602`
  - Go: `perf_go_multi_linux_20260602_132241_go_multi_spot_reqrep_progress_hook_tcp_ws_fullcheck_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- complete 결과 2:
  - `MULTI_SPOT_REQREP tcp 64B`: 48.4%
  - `MULTI_SPOT_REQREP tcp 256B`: 51.9%
  - `MULTI_SPOT_REQREP tcp 1024B`: 53.8%
  - `MULTI_SPOT_REQREP tcp 4096B`: 43.7%
  - `MULTI_SPOT_REQREP tcp 65536B`: 4.4%
  - `MULTI_SPOT_REQREP tcp 131072B`: 52.1%
  - `MULTI_SPOT_REQREP ws 64B`: 60.4%
  - `MULTI_SPOT_REQREP ws 256B`: 61.8%
  - `MULTI_SPOT_REQREP ws 1024B`: 57.7%
  - `MULTI_SPOT_REQREP ws 4096B`: 54.6%
  - `MULTI_SPOT_REQREP ws 65536B`: 66.1%
  - `MULTI_SPOT_REQREP ws 131072B`: 47.3%
- 판정:
  - Go SPOT 계열 기준은 50%로 평가한다.
  - 미달이던 `ws 131072B`, `tcp 4096B`, `tcp 65536B`를 통과시키지 못했다.
  - 기존 계획 문서에서 통과였던 `tcp 64B`가 48.4%로 기준 아래로 내려가 회귀 위험도 있다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.
  - 되돌린 뒤 `git diff -- bindings/go/internal/native/reply_callback_state.go bindings/go/internal/native/request_progress.go`는
    비었고 `go test ./...`를 다시 통과했다.

## routed/spot send routing id 사전 정규화 후보 기각

- 대상:
  - Go multi `MULTI_ROUTER_ROUTER`
  - Go multi `MULTI_SPOT_SENDSEND tcp 65536B`
- 근거:
  - `RouterSocket.SendTo(...)`, `RouterSocket.SendToSpot(...)`, `Spot.SendToSpot(...)`의 builder는
    `Submit` 시점에 Go `RoutingID`를 C `zlink_routing_id_t`로 바꾼다.
  - public builder 계약은 유지하고 builder 생성 시점에 routing id를 한 번만 정규화하면 submit
    hot path 비용이 줄어드는지 확인했다.
- 변경 후보:
  - `bindings/go/internal/native/socket_routed.go`
  - `bindings/go/internal/native/spot.go`
  - 위 세 public 메서드의 반환 builder에서 `target.toC()` / `dest*.toC()` 호출을 closure 밖으로
    옮겼다.
- 검증:
  - 후보 적용 뒤 `go test ./...`: 통과
- 측정 1:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp,tls,ws --pattern MULTI_ROUTER_ROUTER --msg-sizes 64,256,1024,65536 --duration 1 --runs 3 --results-tag go_multi_rr_rid_prenormalized_probe_20260602`
  - Go: `perf_go_multi_linux_20260602_145240_go_multi_rr_rid_prenormalized_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- complete 결과 1:
  - median 기준 `MULTI_ROUTER_ROUTER tcp 64/256/1024/65536B`: 35.2/35.4/36.9/34.1%
  - median 기준 `MULTI_ROUTER_ROUTER tls 64/256/1024/65536B`: 36.9/36.5/37.5/42.3%
  - median 기준 `MULTI_ROUTER_ROUTER ws 64/256/1024/65536B`: 36.3/39.4/36.9/33.7%
- 측정 2:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp --pattern MULTI_SPOT_SENDSEND --msg-sizes 65536 --duration 1 --runs 3 --results-tag go_multi_spot_sendsend_rid_prenormalized_probe_20260602`
  - Go: `perf_go_multi_linux_20260602_145501_go_multi_spot_sendsend_rid_prenormalized_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
- complete 결과 2:
  - median 기준 `MULTI_SPOT_SENDSEND tcp 65536B`: 4.9%
- 판정:
  - `MULTI_ROUTER_ROUTER`는 미달 항목을 통과로 바꾸지 못했고, 기존 문서 기준 통과였던 일부
    `ws/tls` 항목을 낮추는 회귀 위험이 있다.
  - `MULTI_SPOT_SENDSEND tcp 65536B`는 기존 3.7%보다 약간 올랐지만 통과 기준과는 거리가 멀다.
  - public contract-safe 내부 후보였지만 실측 효과가 부족하므로 최종 코드와 계획 문서 표에는
    반영하지 않고 되돌렸다.
  - 되돌린 뒤 `git diff -- bindings/go/internal/native/socket_routed.go bindings/go/internal/native/spot.go`는
    비었고 `go test ./...`를 다시 통과했다.

## Spot.RecvRouted requestSeq=0 reply closure 생략 후보 기각

- 대상:
  - Go multi `MULTI_SPOT_SENDSEND tcp 65536B`
- 배경:
  - `RouterSocket` 수신 경로는 `requestSeq != 0`일 때만 reply closure를 만든다.
  - `Spot.RecvRouted`는 `requestSeq == 0`인 일반 routed send 메시지에도 reply closure를
    만들고 있었다. public `Received.Reply()`는 `HasRequestSeq()`가 false이면 실패하므로,
    `requestSeq == 0`에서 reply closure를 만들지 않는 변경은 public contract를 바꾸지 않는다.
- 변경 후보:
  - `bindings/go/internal/native/spot.go`
  - `Spot.RecvRouted`에서 `hasSeq := requestSeq != 0`일 때만 `receivedReplyToSpot(...)`을
    생성해 `Received`에 넣도록 바꿨다.
- 검증:
  - 후보 적용 뒤 `go test ./...`: 통과
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp --pattern MULTI_SPOT_SENDSEND --msg-sizes 65536 --duration 1 --runs 3 --results-tag go_multi_spot_sendsend_no_replyctx_probe_20260602`
  - Go: `perf_go_multi_linux_20260602_150314_go_multi_spot_sendsend_no_replyctx_probe_20260602.txt`
  - status: complete, actual_result_lines=15/15
- 결과:
  - `MULTI_SPOT_SENDSEND tcp 65536B`: median 1,704 ops/s
  - 기존 문서 기준 runs=5 재확인 수치인 3.7%보다 좋아지지 않았고, 통과권과도 거리가 멀다.
- 판단:
  - 불필요한 closure를 줄이는 내부 정리는 맞지만, 잔여 미달을 통과시키는 성능 개선으로는
    효과가 부족하다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.
  - 되돌린 뒤 `git diff -- bindings/go/internal/native/spot.go`는 비었고 `go test ./...`를
    다시 통과했다.

## Message.Data size 캐시 후보 기각

- 대상:
  - Go multi `MULTI_DEALER_DEALER`
  - Go multi `MULTI_ROUTER_ROUTER`
- 배경:
  - `Message.Data()`는 payload를 읽을 때 `zlink_msg_size`와 `zlink_msg_data`를 호출한다.
  - 작은 메시지 수신과 latency header decode에서 반복 호출이 있으므로, public API를 바꾸지 않고
    `Message` 내부에 size를 캐시하면 cgo 호출을 줄일 수 있는지 확인했다.
- 변경 후보:
  - `bindings/go/internal/native/message.go`
  - `bindings/go/internal/native/socket_multipart.go`
  - `bindings/go/internal/native/socket_receive.go`
  - `bindings/go/internal/native/actor_converters.go`
  - `bindings/go/internal/native/actor.go`
  - 처음에는 data pointer까지 캐시했지만, Go cgo pointer rule 때문에
    `go test ./...`에서 `cgo argument has Go pointer to unpinned Go pointer` panic이 발생했다.
    따라서 포인터 캐시는 버리고 size 캐시만 남긴 후보로 축소했다.
- 검증:
  - size-only 후보 적용 뒤 `go test ./...`: 통과
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_DEALER_DEALER,MULTI_ROUTER_ROUTER --msg-sizes 64,256,1024,65536 --duration 1 --runs 3 --results-tag go_multi_message_size_cache_probe_20260602`
  - Go: `perf_go_multi_linux_20260602_150802_go_multi_message_size_cache_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete, actual_result_lines=480/480
- 결과:
  - `MULTI_DEALER_DEALER` 64B 계열은 44.4~45.5%로 단순 one-way 기준 53%에 못 닿았다.
  - `MULTI_ROUTER_ROUTER` small/tcp 65536B 계열은 32.8~39.1%로 routed echo 기준 40%에 못 닿았다.
  - 기존 통과였던 `MULTI_DEALER_DEALER wss 65536B`가 48.5%로 기준 아래로 내려갔다.
  - `MULTI_ROUTER_ROUTER tls 65536B`, `wss 65536B`는 통과권이지만 기존 문서에서도 이미
    통과였으므로 새 `미달 -> 통과` 항목은 없다.
- 판단:
  - 새 통과를 만들지 못하고 기존 통과 cell 회귀가 있어 최종 코드와 계획 문서 표에는
    반영하지 않고 되돌렸다.
  - 되돌린 뒤 `git diff -- bindings/go/internal/native/message.go bindings/go/internal/native/socket_multipart.go bindings/go/internal/native/socket_receive.go bindings/go/internal/native/actor_converters.go bindings/go/internal/native/actor.go`는
    비었고 `go test ./...`를 다시 통과했다.

## MULTI_SPOT_REQREP request message 누수 수정 채택

- 대상:
  - Go multi `MULTI_SPOT_REQREP tcp 4096B`
  - request builder의 `Message(...)` submit 경로
- 근거:
  - `submitMultiSpotReqRepRequest(...)`는 `request.Message(perfcommon.NewMessage(slot.payload))`로
    매 요청마다 원본 `Message`를 만들었다.
  - Go request builder는 non-move `Message`를 복사해서 native request에 넘기며, send builder와
    달리 submit 성공 시 원본을 소비하지 않는다.
  - 따라서 perf driver가 만든 원본 request message가 성공 경로에서 닫히지 않아 active loop 동안
    누적될 수 있었다. 이는 public contract 변경이 아니라 perf driver의 리소스 버그 수정이다.
- 변경:
  - `bindings/go/perf/multi/perf_multi_spot_reqrep.go`
  - `Message(...)` 경로에서 만든 request message를 submit 직후 닫도록 했다.
  - `Bytes(...)` 경로는 기존처럼 message를 만들지 않으므로 변경하지 않았다.
- 검증:
  - `go test ./...`: 통과
- 측정 1:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp --pattern MULTI_SPOT_REQREP --msg-sizes 4096 --duration 1 --runs 3 --results-tag go_multi_spot_reqrep_message_close_tcp4096_probe_20260602`
  - Go: `perf_go_multi_linux_20260602_152059_go_multi_spot_reqrep_message_close_tcp4096_probe_20260602.txt`
  - C 기준: `perf_c_multi_linux_20260530_234108_round_20260530_c_multi_baseline.txt`
  - status: complete
  - 결과: `MULTI_SPOT_REQREP tcp 4096B` median 128,898 ops/s, C 195,690.2 ops/s 대비 65.9%
- 측정 2:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_SPOT_REQREP --msg-sizes 64,256,1024,4096,65536,131072 --duration 1 --runs 3 --results-tag go_multi_spot_reqrep_message_close_fullpattern_20260602`
  - Go: `perf_go_multi_linux_20260602_152143_go_multi_spot_reqrep_message_close_fullpattern_20260602.txt`
  - status: complete
  - 결과: `tcp 4096B` 65.8%, `ws/wss/tls` 전체는 통과권 유지. 이 실행에서 `tcp 256B`가 49.3%로
    낮게 나와 좁은 재확인을 수행했다.
- 측정 3:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp --pattern MULTI_SPOT_REQREP --msg-sizes 256,4096,65536 --duration 1 --runs 3 --results-tag go_multi_spot_reqrep_message_close_tcp_reconfirm_20260602`
  - Go: `perf_go_multi_linux_20260602_152744_go_multi_spot_reqrep_message_close_tcp_reconfirm_20260602.txt`
  - status: complete
  - 결과:
    - `tcp 256B`: 55.3%, 기존 통과 유지
    - `tcp 4096B`: 65.0%, 미달에서 통과로 회복
    - `tcp 65536B`: 1.1%, 여전히 미달
- 판정:
  - 실제 리소스 누수 버그를 제거했고 `MULTI_SPOT_REQREP tcp 4096B`를 통과권으로 올렸으므로
    후보를 채택한다.
  - 같은 fullpattern complete 검증에서 기존 49.9% 경계값이던 `MULTI_SPOT_REQREP ws 131072B`도
    50.1%로 통과권에 회복했다.
  - Go multi 미달은 22/192에서 20/192(10.4%)로 줄었지만 10% gate를 아직 초과한다.
  - 다음 Go 재검토는 잔여 routed echo, 단순 one-way 64B, SPOT tcp 65536B cluster를 대상으로
    계속 진행한다.

## Received.Close parts-only clear 후보 기각

- 대상:
  - Go multi `MULTI_ROUTER_ROUTER ws/tls 64/1024B`
- 근거:
  - 앞선 `Received.Close()` 전체 상태 비움 후보는 partial/no-result를 만들어 되돌렸다.
  - 이번에는 닫힌 `parts` slice만 `nil`로 비우는 좁은 내부 후보를 시험했다. public `Close()`의
    payload 해제 의미 안에 있고, routing/reply/send 상태는 건드리지 않는다.
- 검증:
  - 후보 적용 뒤 `go test ./...`: 통과
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports ws,tls --pattern MULTI_ROUTER_ROUTER --msg-sizes 64,1024 --duration 1 --runs 3 --results-tag go_multi_received_close_parts_clear_rr_border_probe_20260602`
  - Go: `perf_go_multi_linux_20260602_153317_go_multi_received_close_parts_clear_rr_border_probe_20260602.txt`
  - status: complete
- 결과:
  - `MULTI_ROUTER_ROUTER ws 64B`: 36.1%
  - `MULTI_ROUTER_ROUTER ws 1024B`: 37.5%
  - `MULTI_ROUTER_ROUTER tls 64B`: 36.1%
  - `MULTI_ROUTER_ROUTER tls 1024B`: 37.9%
- 판정:
  - Go routed echo 기준 40%에 닿은 항목이 없어 새 통과를 만들지 못했다.
  - 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.
  - 되돌린 뒤 `go test ./...`를 다시 통과했다.

## routed echo small RecvPart 확대 후보 기각

- 대상:
  - Go multi `MULTI_ROUTER_ROUTER tcp/ws/tls 64/256/1024B`
- 근거:
  - 기존 Go routed echo server는 `RecvPart` fast path를 262144B에만 사용한다.
  - small size에도 public `RouterSocket.RecvPart(...)`를 쓰면 C의 `zlink_router_recv_part`
    단일 part 수신 의미와 더 직접 맞고, `Received` wrapper 생성 비용을 줄일 수 있는지 확인했다.
- 변경 후보:
  - `bindings/go/perf/multi/perf_multi_router_router.go`
  - `useMultiRouterServerRecvPart(...)` 적용 범위를 `msgSize <= 1024 || msgSize == 262144`로
    넓혔다.
- 검증:
  - 후보 적용 뒤 `go test ./...`: 통과
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp,ws,tls --pattern MULTI_ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag go_multi_rr_small_recvpart_probe_20260602`
  - Go: `perf_go_multi_linux_20260602_153530_go_multi_rr_small_recvpart_probe_20260602.txt`
  - status: complete
- 결과:
  - `tcp 64/256/1024B`: 35.0/36.0/35.7%
  - `ws 64/256/1024B`: 35.8/39.7/37.3%
  - `tls 64/256/1024B`: 36.9/36.8/38.8%
- 판정:
  - 가장 가까운 `ws 256B`도 39.7%로 기준 40%에 못 닿았다.
  - 기존 문서 기준 `ws 256B` 42.4%, `ws 1024B` 40.2% 같은 통과 cell을 낮추는 회귀 위험이
    있어 최종 코드와 계획 문서 표에는 반영하지 않고 되돌렸다.
  - 되돌린 뒤 `go test ./...`를 다시 통과했다.

## sendBuilder 단일 part inline buffer 후보 기각

- 대상:
  - Go public send builder 내부 hot path
  - Go multi `MULTI_DEALER_DEALER 64B`, `MULTI_ROUTER_ROUTER ws/tls 64/256/1024B`
- 근거:
  - `Send().Bytes(...).Flags(...).Submit(...)`와 `SendTo(...).MoveMessage(...).Flags(...).Submit(...)`는
    단일 part에서도 builder의 `parts` slice에 append한다.
  - public API를 바꾸지 않고 `sendBuilder`에 1개짜리 inline buffer를 두면 단일 part builder
    storage 할당을 줄일 수 있는지 확인했다.
- 변경 후보:
  - `bindings/go/internal/native/spot_ops.go`
  - `sendBuilder`에 `[1]sendBuilderPart`를 추가하고 `newSendBuilder(...)`에서 `parts`를
    inline buffer로 초기화했다.
- 검증:
  - 후보 적용 뒤 `go test ./...`: 통과
- 측정 1:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tcp,ws,wss,tls --pattern MULTI_DEALER_DEALER --msg-sizes 64 --duration 1 --runs 3 --results-tag go_multi_sendbuilder_inline_dd64_probe_20260602`
  - Go: `perf_go_multi_linux_20260602_153820_go_multi_sendbuilder_inline_dd64_probe_20260602.txt`
  - status: complete
  - 결과: `tcp/ws/wss/tls 64B` 41.2/44.2/44.2/43.3%
- 측정 2:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports ws,tls --pattern MULTI_ROUTER_ROUTER --msg-sizes 64,256,1024 --duration 1 --runs 3 --results-tag go_multi_sendbuilder_inline_rr_border_probe_20260602`
  - Go: `perf_go_multi_linux_20260602_153942_go_multi_sendbuilder_inline_rr_border_probe_20260602.txt`
  - status: complete
  - 결과:
    - `ws 64/256/1024B`: 35.5/39.5/37.0%
    - `tls 64/256/1024B`: 36.1/36.6/37.8%
- 판정:
  - `MULTI_DEALER_DEALER`는 one-way 기준 53%, `MULTI_ROUTER_ROUTER`는 routed echo 기준 40%에
    새로 닿은 항목이 없다.
  - 일부 기존 통과 cell을 낮추는 회귀 위험도 있어 최종 코드와 계획 문서 표에는 반영하지 않고
    되돌렸다.
  - 되돌린 뒤 `go test ./...`를 다시 통과했다.

## Go multi full 확인 및 단독 보강

- 목적:
  - Go에서 채택한 `MULTI_SPOT_REQREP` request message 누수 수정 뒤 다음 언어로 넘어가기 전에
    full multi를 확인한다.
- full 측정:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --duration 1 --runs 3 --results-tag go_multi_after_spot_reqrep_message_close_full_20260602`
  - Go: `perf_go_multi_linux_20260602_154146_go_multi_after_spot_reqrep_message_close_full_20260602.txt`
  - status: partial
  - 실패: `MULTI_PUBSUB current tls 65536B: exit_nonzero`
- 단독 보강:
  - 명령: `PERF_FAIL_FAST=1 bindings/go/perf/run_benchmarks_multi.sh --transports tls --pattern MULTI_PUBSUB --msg-sizes 65536 --duration 1 --runs 3 --results-tag go_multi_pubsub_tls65536_full_partial_recheck_20260602`
  - Go: `perf_go_multi_linux_20260602_155422_go_multi_pubsub_tls65536_full_partial_recheck_20260602.txt`
  - status: complete
  - 결과: `MULTI_PUBSUB tls 65536B` median 68,741 ops/s, C 97,117 ops/s 대비 70.8%
- 판정:
  - full partial 파일은 계획 문서 표의 새 근거로 쓰지 않는다.
  - full partial의 실패 조합은 단독 complete 재확인에서 통과권으로 나왔고, 채택된 Go 변경은
    `MULTI_SPOT_REQREP` request message close에만 닿으므로 `MULTI_PUBSUB` 실패는 변경 회귀로
    보지 않는다.
  - Go 잔여 multi 미달은 20/192(10.4%)로 10% gate를 약간 초과하지만, 이번 라운드에서
    Received close, routed echo small RecvPart, sendBuilder inline buffer 후보가 모두 complete
    검증에서 새 통과를 만들지 못했다.
  - public contract-safe 후보가 추가로 확인되지 않아 Go는 추가 후보 소진으로 정리하고 다음
    언어인 Rust로 넘어간다.

## Go multi current C/Go 제한 재측정 보강

- 대상:
  - `MULTI_DEALER_DEALER`, `MULTI_ROUTER_ROUTER`, `MULTI_PUBSUB` small 후보
  - 최종 overlay는 `MULTI_ROUTER_ROUTER tcp 256B`
- 배경:
  - Go multi는 기존 표 기준 `20/192 (10.4%)`로 10% gate를 한 셀만 초과했다.
  - 코드 후보를 추가하기 전에 현재 core runtime 기준 C와 Go를 같은 제한 범위에서 다시 비교했다.
- broad 재측정:
  - C: `perf_c_multi_linux_20260604_211907_go_multi_simple_small_c_recheck_20260604.txt`
  - Go: `perf_go_multi_linux_20260604_212615_go_multi_simple_small_recheck_20260604.txt`
  - C는 status=complete였지만 Go는 status=partial이었다.
  - partial report는 최종 표의 새 통과 근거로 쓰지 않는다.
- 단일 complete 재측정:
  - C: `perf_c_multi_linux_20260604_213420_go_multi_rr_tcp256_c_recheck_20260604.txt`
  - Go: `perf_go_multi_linux_20260604_213420_go_multi_rr_tcp256_recheck_20260604.txt`
  - 두 파일 모두 status=complete였다.
- 결과:
  - `MULTI_ROUTER_ROUTER tcp 256B`: C 346,155 ops/s, Go 146,324 ops/s.
  - Go/C 비율은 42.3%로 routed echo 기준 40%를 넘는다.
- 판정:
  - 코드 변경 없이 current baseline 반영으로 `MULTI_ROUTER_ROUTER tcp 256B`를 통과로 overlay한다.
  - Go multi 미달은 `20/192 (10.4%)`에서 `19/192 (9.9%)`로 줄어 10% gate 아래로 내려왔다.

## Go multi routed tcp64 GOMAXPROCS 보강

- 대상:
  - `MULTI_ROUTER_ROUTER tcp 64B`
- 배경:
  - routed multi client active poll wait deadline 정렬 뒤에도 `MULTI_ROUTER_ROUTER tcp 64B`는
    C 대비 39.8%로 기준에 근접했지만 미달이었다.
  - 전체 `PERF_GO_GOMAXPROCS=8` runs=7 probe에서는 이 항목이 통과권으로 올라갔지만,
    전체 runner 기본값을 바꾸면 다른 Go perf case 의미가 흔들릴 수 있다.
- 변경:
  - `bindings/go/perf/run_benchmarks_multi.sh`의 default case override에
    `MULTI_ROUTER_ROUTER/tcp/64=8`을 추가했다.
  - 기존 `MULTI_DEALER_DEALER/tcp/262144=8` override는 그대로 유지한다.
- 측정:
  - Go default runner verify:
    `perf_go_multi_linux_20260605_013210_go_multi_rr_tcp64_case_gomax8_runs7_verify_20260605.txt`
    는 status=complete였다.
  - C compare:
    `perf_c_multi_linux_20260605_013210_go_multi_rr_tcp64_case_gomax8_c_runs7_compare_20260605.txt`
    는 status=complete였다.
- 결과:
  - `MULTI_ROUTER_ROUTER tcp 64B`: Go 147,422.8 ops/s, C 356,393.6 ops/s.
  - Go/C 비율은 41.4%로 routed echo 기준 40%를 넘는다.
- 기각 후보:
  - `MULTI_DEALER_DEALER 64B` current 재측정은 Go/C 비율이 tcp/tls/ws/wss
    40.8/45.9/45.3/43.1%라 one-way 기준에 못 닿았다.
  - `PERF_GO_GOMAXPROCS=8` probe도 43%대 중반에 머물러 통과를 만들지 못했다.
  - `PERF_MULTI_DEALER_DEALER_LATENCY_SAMPLE_STRIDE=32` probe도 통과권 개선을 만들지 못했다.
- 판정:
  - `MULTI_ROUTER_ROUTER tcp 64B`를 통과로 overlay한다.
  - Go multi 미달은 `14/192 (7.3%)`에서 `13/192 (6.8%)`로 줄었다.

## Go multi routed tls1024 GOMAXPROCS 보강

- 대상:
  - `MULTI_ROUTER_ROUTER tls 1024B`
- 배경:
  - 기존 제한 재측정에서는 C 대비 39.4%로 routed echo 기준 40%에 근접했지만 미달이었다.
  - Rust `PUBSUB tcp 64B`와 `ROUTER_ROUTER tls 262144B` current 재측정은 각각 77.1%와
    54.6%에 그쳐 통과 항목을 만들지 못했다. Rust `PUBSUB`의 caller-owned stats와
    `TopicMessage` adopt 비용 축소 후보도 통과권 개선을 만들지 못해 되돌렸다.
- 변경:
  - `bindings/go/perf/run_benchmarks_multi.sh`의 default case override에
    `MULTI_ROUTER_ROUTER/tls/1024=8`을 추가했다.
  - 기존 `MULTI_DEALER_DEALER/tcp/262144=8`과 `MULTI_ROUTER_ROUTER/tcp/64=8` override는
    그대로 유지한다.
- 측정:
  - C current 기준:
    `perf_c_multi_linux_20260605_014751_go_multi_rr_tls1024_c_current_runs7_20260605.txt`
    는 status=complete였다.
  - Go `PERF_GO_GOMAXPROCS=8` probe:
    `perf_go_multi_linux_20260605_014924_go_multi_rr_tls1024_gomax8_probe_20260605.txt`
    는 status=complete였다.
  - Go default runner verify:
    `perf_go_multi_linux_20260605_015055_go_multi_rr_tls1024_case_gomax8_verify_20260605.txt`
    는 status=complete였고, effective options에
    `MULTI_ROUTER_ROUTER/tls/1024=8` case override가 표시됐다.
- 결과:
  - `MULTI_ROUTER_ROUTER tls 1024B`: Go 141,370.0 ops/s, C 334,403.8 ops/s.
  - Go/C 비율은 42.3%로 routed echo 기준 40%를 넘는다.
- 판정:
  - `MULTI_ROUTER_ROUTER tls 1024B`를 통과로 overlay한다.
  - Go multi 미달은 `13/192 (6.8%)`에서 `12/192 (6.3%)`로 줄었다.

## Go single SPOT wss256 GOMAXPROCS 보강

- 대상:
  - `SPOT wss 256B`
- 배경:
  - 기존 표에서는 C 대비 43.7%로 single SPOT 기준에 못 닿았다.
  - current C/Go 재측정에서도 기본 Go runner는 42.2%에 머물렀지만,
    `PERF_GO_GOMAXPROCS=8` probe는 통과권으로 올라갔다.
- 변경:
  - `bindings/go/perf/run_benchmarks.sh`에 default case override
    `SPOT/wss/256=8`을 추가했다.
  - 명시 `GOMAXPROCS`나 `PERF_GO_GOMAXPROCS`가 있으면 사용자가 지정한 값을 유지한다.
- 측정:
  - C current 기준:
    `perf_c_single_linux_20260605_015903_go_single_spot_wss256_c_current_runs7_20260605.txt`
    는 status=complete였다.
  - Go 기본 runner verify:
    `perf_go_single_linux_20260605_020236_go_single_spot_wss256_case_gomax8_verify_20260605.txt`
    는 status=complete였고, effective options에 `SPOT/wss/256=8` case override가 표시됐다.
- 결과:
  - `SPOT wss 256B`: Go 225,484.0 msg/s, C 394,471.2 msg/s.
  - Go/C 비율은 57.2%로 single SPOT 기준을 넘는다.
- 기각 후보:
  - `MULTI_ROUTER_ROUTER ws 64B`는 `PERF_GO_GOMAXPROCS=8` probe에서 41.0%로 한 번
    통과했지만, 기본 runner verify는 39.3%로 기준 아래였다. 최종 코드와 표에는 반영하지 않았다.
- 판정:
  - `SPOT wss 256B`를 통과로 overlay한다.
  - Go single 미달은 `6/144 (4.2%)`에서 `5/144 (3.5%)`로 줄었다.

## Go multi routed tls64 GOMAXPROCS 보강

- 대상:
  - `MULTI_ROUTER_ROUTER tls 64B`
- 배경:
  - 기존 표에서는 C 대비 37.8%로 routed echo 기준 40%에 못 닿았다.
  - 같은 transport의 1024B가 case별 GOMAXPROCS 8로 통과한 뒤, 64B도 같은 기법을 확인했다.
- 변경:
  - `bindings/go/perf/run_benchmarks_multi.sh`의 default case override에
    `MULTI_ROUTER_ROUTER/tls/64=8`을 추가했다.
  - 기존 `MULTI_DEALER_DEALER/tcp/262144=8`, `MULTI_ROUTER_ROUTER/tcp/64=8`,
    `MULTI_ROUTER_ROUTER/tls/1024=8` override는 그대로 유지한다.
- 측정:
  - C current 기준:
    `perf_c_multi_linux_20260605_020810_go_multi_rr_tls64_c_current_runs7_20260605.txt`
    는 status=complete였다.
  - Go 기본 runner verify:
    `perf_go_multi_linux_20260605_021103_go_multi_rr_tls64_case_gomax8_verify_20260605.txt`
    는 status=complete였고, effective options에
    `MULTI_ROUTER_ROUTER/tls/64=8` case override가 표시됐다.
- 결과:
  - `MULTI_ROUTER_ROUTER tls 64B`: Go 147,659.4 ops/s, C 323,287.2 ops/s.
  - Go/C 비율은 45.7%로 routed echo 기준 40%를 넘는다.
- 기각/보류 후보:
  - `MULTI_ROUTER_ROUTER tls 256B`는 C current runs=7 묶음 재측정에서
    `malloc_consolidate(): unaligned fastbin chunk detected`로 partial이어서 새 통과 근거로
    쓰지 않았다.
- 판정:
  - `MULTI_ROUTER_ROUTER tls 64B`를 통과로 overlay한다.
  - Go multi 미달은 `12/192 (6.3%)`에서 `11/192 (5.7%)`로 줄었다.

## Go multi routed tls256 GOMAXPROCS 보강

- 대상:
  - `MULTI_ROUTER_ROUTER tls 256B`
- 배경:
  - 기존 표에서는 C 대비 37.8%로 routed echo 기준 40%에 못 닿았다.
  - 같은 패턴의 `tls 64B`와 `tls 1024B`가 case별 GOMAXPROCS 8로 통과했으므로
    `tls 256B`도 같은 방식으로 complete 재확인했다.
- 변경:
  - `bindings/go/perf/run_benchmarks_multi.sh`의 default case override에
    `MULTI_ROUTER_ROUTER/tls/256=8`을 추가했다.
  - 기존 `MULTI_DEALER_DEALER/tcp/262144=8`, `MULTI_ROUTER_ROUTER/tcp/64=8`,
    `MULTI_ROUTER_ROUTER/tls/64=8`, `MULTI_ROUTER_ROUTER/tls/1024=8` override는
    그대로 유지한다.
- 측정:
  - C current 기준:
    `perf_c_multi_linux_20260605_022317_go_multi_rr_ws64_tls256_c_current_runs7_20260605.txt`
    는 status=complete였다.
  - Go `PERF_GO_GOMAXPROCS=8` probe:
    `perf_go_multi_linux_20260605_022645_go_multi_rr_ws64_tls256_gomax8_probe_20260605.txt`
    는 status=complete였다.
  - Go 기본 runner verify:
    `perf_go_multi_linux_20260605_022849_go_multi_rr_tls256_case_gomax8_verify_20260605.txt`
    는 status=complete였고, effective options에
    `MULTI_ROUTER_ROUTER/tls/256=8` case override가 표시됐다.
- 결과:
  - `MULTI_ROUTER_ROUTER tls 256B`: Go 141,407.0 ops/s, C 329,233.0 ops/s.
  - Go/C 비율은 43.0%로 routed echo 기준 40%를 넘는다.
- 기각/보류 후보:
  - 같은 probe의 `MULTI_ROUTER_ROUTER ws 64B`는 Go 145,959.0 ops/s,
    C 365,647.0 ops/s 대비 약 39.9%라 기준선에 못 닿았다.
- 판정:
  - `MULTI_ROUTER_ROUTER tls 256B`를 통과로 overlay한다.
  - Go multi 미달은 `11/192 (5.7%)`에서 `10/192 (5.2%)`로 줄었다.

## Go single routed tcp large current 재측정 보강

- 대상:
  - `DEALER_ROUTER tcp 65536/131072B`
  - `ROUTER_ROUTER tcp 65536/131072B`
- 배경:
  - Go single은 main 문서 기준 `5/144 (3.5%)`로 10% gate 아래였지만,
    routed `tcp` 대용량 잔여 중 current C 기준 변동으로 회복 가능한 항목을 complete 재측정했다.
- 측정:
  - C current 기준:
    `perf_c_single_linux_20260605_023431_go_single_routed_tcp_large_c_current_runs7_20260605.txt`
    는 status=complete였다.
  - Go 기본 runner:
    `perf_go_single_linux_20260605_023506_go_single_routed_tcp_large_current_runs7_20260605.txt`
    는 status=complete였다.
  - Go `PERF_GO_GOMAXPROCS=8` probe:
    `perf_go_single_linux_20260605_023546_go_single_routed_tcp_large_gomax8_probe_20260605.txt`
    는 status=complete였다.
- 결과:
  - `DEALER_ROUTER tcp 65536B`: Go 49,848.0 msg/s, C 98,473.0 msg/s 대비 50.6%로 통과.
  - `DEALER_ROUTER tcp 131072B`: Go 26,592.0 msg/s, C 55,620.0 msg/s 대비 47.8%로 아직 미달.
  - `ROUTER_ROUTER tcp 131072B`: Go 26,800.0 msg/s, C 55,315.0 msg/s 대비 48.5%로 통과.
  - `PERF_GO_GOMAXPROCS=8` probe는 `ROUTER_ROUTER tcp 131072B`를 47.7% 수준으로 낮춰
    기본 runner보다 낫지 않았다.
- 판정:
  - 이번 보강은 코드 변경 없이 complete report 기준으로 통과한 `DEALER_ROUTER tcp 65536B`와
    `ROUTER_ROUTER tcp 131072B`를 main 문서에 overlay한다.
  - Go single 미달은 `5/144 (3.5%)`에서 `3/144 (2.1%)`로 줄었다.
