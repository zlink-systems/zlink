# Go REQREP 재개 기록

- 일자: 2026-09-08
- branch: `main`
- Core source: `release`
- Core package: `/home/hep7/.cache/zlink/core-pinned/0.17.2`
- Core runtime: `lib/libzlink.so.0.17.2`
- Core runtime SHA-256: `72d508f73a5d261ff608c7ac49b5b55e7277e18e15fe4b5ac51affc0468012d3`
- 범위: `bindings/go/perf/`의 single/multi REQREP 러너와 이 기록만 변경

## 결과

Go single/multi REQREP에서 애플리케이션이 정하던 미완료 request 상한을 제거하고 C 기준 turn 구조로 되돌렸다. Core 0.17.2에서 single REQREP 2종, multi REQREP 2종과 multi 전 크기 5회 판정이 모두 `status: complete`로 끝났다. `MULTI_DEALER_DEALER` 회귀 smoke도 완료됐다.

수정 전/후 규칙 수: `고정 상한 + socket별 outstanding gate + cap까지 채우기`의 세 규칙을 없애고, `turn당 제출 후 completion 진행` 한 규칙만 남겼다.

## 상한 제거와 turn 구조

### Single REQREP

- `bindings/go/perf/single/perf_reqrep.go`에서 기본값 `64`, `PERF_SINGLE_REQREP_MAX_OUTSTANDING`, 하한 보정과 고정 worker 수를 제거했다.
- requester turn마다 blocking public request terminal을 goroutine 한 개에서 한 번 시작한다. Go의 blocking terminal + goroutine 관례와 `runtime.LockOSThread()` 사용은 유지했다.
- requester thread는 `POLLCOMPLETION`만 등록한 poller로 reply와 WRITABLE 진행을 구동한다. 각 turn에서 이미 끝난 completion을 drain하고, outstanding reply 수를 다음 제출의 gate로 사용하지 않는다.
- completion 채널은 single turn 크기인 `1`만 버퍼링한다. 이 값은 outstanding 상한이 아니라 turn에서 생산할 수 있는 완료 전달 한 건의 저장 공간이다.

### Multi REQREP

- `bindings/go/perf/multi/perf_multi_socket_reqrep.go`에서 기본값 `64`, `PERF_MULTI_REQREP_MAX_OUTSTANDING`, socket별 outstanding 배열과 `outstanding[index] < maxOutstanding` gate를 제거했다.
- 각 turn은 requester socket마다 blocking public request terminal을 goroutine으로 한 건씩 시작한 뒤, 완료된 결과를 drain하고 completion-only poller를 한 번 진행한다. 이전 reply 완료 여부는 다음 turn 제출 조건이 아니다.
- completion 채널은 `len(clients)`로 바꿨다. 한 turn의 최대 제출 수만 버퍼링하며 미완료 request 총량을 제한하지 않는다. `totalOutstanding`은 active 종료 뒤 completion drain의 수명 확인에만 사용한다.
- binding terminal은 `BACKPRESSURED + WRITABLE token`을 받으면 같은 logical request를 내부에 보관했다가 해당 token에서 재개한다. 러너는 이를 catch하거나 새 요청으로 재시도하지 않는다. request timeout만 정상 terminal completion으로 처리하고, 러너에 반환된 submit 오류와 그 밖의 request 오류는 모두 fatal이다.

두 러너 모두 `POLLOUT`을 등록하지 않았고 timeout, sleep, client 수, 메시지 크기 기본값을 변경하지 않았다. 새 outstanding 상한 숫자나 상한 환경 변수도 넣지 않았다.

## Core 0.17.1 대비

Core 0.17.1에서는 같은 socket의 두 번째 동시 multipart request가 single에서 `EAGAIN`, multi에서 `EINVAL`로 끝났지만, Core 0.17.2는 동시 multipart request를 받아 binding이 admission backpressure와 completion을 진행하므로 같은 러너 구조가 정상 완료된다.

## 검증

모든 benchmark 실행 전에 `bash scripts/perf/wait-for-idle-perf.sh`가 성공한 것을 확인했고 한 번에 하나만 실행했다. 측정 시작 load는 모두 5 이하였다.

| 항목 | 조건 | 결과 |
|---|---|---|
| Go perf test | `ZLINK_CORE_SOURCE=release`, pinned 0.17.2, `go test ./perf/...` | 통과 |
| Go perf vet | 같은 Core, `go vet ./perf/...` | 통과 |
| Single REQREP smoke | 2종, tcp, 64 B, duration 1, runs 1 | `perf_go_single_linux_20260908_082728_go172-reqrep-single-final-smoke.txt`, success 2, fail 0, `status: complete` |
| Multi REQREP smoke | 2종, tcp, 64 B, duration 1, runs 1 | `perf_go_multi_linux_20260908_083025_go172-reqrep-multi-final-smoke.txt`, success 2, fail 0, `status: complete` |
| Multi REQREP 판정 | 2종, tcp, 64/256/1024/4096/65536 B, duration 5, runs 5 | `perf_go_multi_linux_20260908_084420_go172-reqrep-multi-final-5x5.txt`, success 50, fail 0, RESULT 250/250, `status: complete` |
| Multi 회귀 smoke | `MULTI_DEALER_DEALER`, tcp, 64 B, duration 1, runs 1 | `perf_go_multi_linux_20260908_085148_go172-dealer-dealer-final-regression-smoke.txt`, success 1, fail 0, `status: complete` |

보고서 경로:

- Single: `bindings/go/perf/results/single/report/`
- Multi: `bindings/go/perf/results/multi/report/`

Core, Framework, Go binding library, 정책·스펙·계획서와 `decisions.ko.md`는 수정하지 않았다. commit과 push도 수행하지 않았다.
