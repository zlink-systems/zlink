# Go binding R3 수정 결과 — F-R3-1, F-R3-12, F-R3-17

세 원인의 구현과 회귀 테스트를 수정했다. 새 테스트 4개는 각각 5회 통과했고,
관련 race 검사·vet·guard와 샘플 7개도 통과했다. **전체 gate의 0 failures 조건은
미충족이다.** 기존 request 테스트의 첫 DATA 준비 단계가 5회 모두 실패하며,
변경 전 Go 구현과 binding을 거치지 않는 공개 Core C API에서도 재현된다.
상세 근거는 아래 BLOCKERS에 기록했다.

작업 branch는 `main`, 비교 기준 HEAD는 `8f42b700a2bf4c385bf4ddfb9ed61744fbd80c5a`다.
commit·push는 하지 않았다. 변경은 `bindings/go/`의 14개 파일과 이 보고서뿐이다.
기존 다른 binding·Framework 변경, Core, 보호된 spec 문서는 수정하지 않았다.
`bindings/AGENTS.md`와 Go 하위 `AGENTS.md`는 없었으며 루트 지침과 문서 지침을 적용했다.

## F-R3-1 — WRITABLE RID 재검증 제거

- 소유 계층: Core는 submit RID echo를 보장하고, Go completion owner는 socket-local
  context/token으로 waiter를 찾는다.
- Spec 조항: `core/doc/spec/core/socket/README.ko.md:989–994`의 part send,
  `:1072–1077`의 REQUEST DONTWAIT, `:1150–1164`의 completion record·종료 결과 표.
  Binding의 재판정 금지는 D-109의 공통 README 반영 결정과 R3 진단을 따른다.
  `bindings/doc/spec/async-coroutine-policy.ko.md` §5의 ReplyToken owner 검사는 별개로 유지한다.
- 원인 위치(변경 전): `bindings/go/internal/native/completion_owner.go:683–689`,
  `:781–804`; `dealer_router_request.go:126–134`, `:163–171`.
  Context/token 조회 뒤 SEND와 REQUEST가 target RID를 다시 비교해 EPROTO로 바꿨다.
- 수정: 두 `matchesTarget` 메서드와 그 호출, 비교에만 쓰던 `routingIDFromC` 변환과
  임시 RID 저장을 제거했다. Completion kind/context/token과 결과 오류 검사는 유지했다.
  Core 계약에 맞는 record의 제출 대상과 결과는 동일하다.
- 교차언어 대조: C의 `bindings/c/include/zlink/socket/api.h:225–244`는 Core raw API를
  그대로 노출하며 RID를 재판정하는 language runtime이 없다. R3 기준 나머지 고수준
  일곱 언어는 같은 결함이므로 적합한 reference로 취급하지 않았다.
- 변경 분류: **B — 기존 결함**, review 분류 `lower-layer-reverification`.
- 수정 전/후 규칙 수: **2 → 1**. Go 범위의 Core echo 보장 + binding 재판정에서
  Core 보장 하나로 줄였다. 전체 캠페인의 일곱 언어 제거 완료를 주장하지 않는다.
- 회귀 테스트: `completion_writable_rid_linux_test.go:14`
  `TestWritableDeliversByContextAndToken`. SEND/REQUEST 각각 정상 echo와 다른 echo를
  주입하고, 동일 context/token의 waiter 성공·entry 제거·payload 해제를 검사한다.
  다른 echo 주입은 소유권 회귀 검사용이며 잘못된 Core record를 공개 사용 계약으로
  추가한 것이 아니다. 변경 전에는 SEND/REQUEST 모두 EPROTO로 실패했다.
- 검증: 새 테스트 5회 통과. 전체 gate의 남은 실패는 아래 공통 BLOCKER이며 이 RID
  테스트는 통과했다.
- Diff 분리: `completion_owner.go`의 RID 필드·변환·비교 삭제 hunk,
  `dealer_router_request.go`의 두 `matchesTarget` 삭제 hunk,
  `completion_writable_rid_linux_test.go`, `completiontest/fixture_linux.{go,c}`.
  F-R3-12 적용 전 상태까지 포함한 패치는 `/tmp/zlink-go-r3/F-R3-1.patch`다.

## F-R3-12 — NO_DATA 뒤에만 재제출

- 소유 계층: socket당 하나인 binding completion owner가 drain과 WRITABLE 재제출
  순서를 소유한다. Core가 정한 NO_DATA 경계를 따른다.
- Spec 조항: `core/doc/spec/core/socket/README.ko.md:989–994`, `:1072–1077`,
  `:1175–1182`; `bindings/doc/spec/async-execution-model.ko.md` §4–6;
  `bindings/doc/spec/async-coroutine-policy.ko.md` §3–4. D-B117과 D-109의 판정을 적용했다.
- 원인 위치(변경 전): `completion_owner.go:598–633`의 drain에서 capture를 호출하고,
  `:732`, `:848`에서 즉시 재제출했다. `dealer_router_request.go:395–397`의 early
  WRITABLE 처리도 submit goroutine에서 재제출할 수 있었다.
- 수정: capture는 재제출 필요 여부를 반환하고, owner의 현재 drain에 속한 local slice가
  entry를 모은다. `NO_DATA`에서만 기존 SEND 제출과 이동한 REQUEST 제출 코드를 실행한다.
  재제출이 만든 새 completion은 다음 drain에서 처리한다.
- Submit/capture 합류: 초기 REQUEST DONTWAIT 제출과 token publish가 기존 entry의
  `attemptMu`를 공유하게 했다. `requestWritableRecord`, `earlyRequestWritable`,
  `publishRequestWait`, submit goroutine의 별도 capture 경로를 제거했다. Native
  completion은 기존 한 close 지점에서 닫는다. Socket 전체 제출 잠금은 추가하지 않았다.
- 대안 비교: early WRITABLE을 submit goroutine에 남기고 별도 drain 완료 상태로
  재제출을 조율하는 방법도 검토했다. Entry의 기존 잠금으로 publish와 capture를
  합류시키는 방법은 별도 상태와 두 번째 재제출 주체를 없애므로 이를 선택했다.
- Cancellation: capture 뒤 NO_DATA 전 caller가 취소되면 재제출 시 기존 settled/publicDone
  상태를 확인해 native 제출 없이 payload와 entry를 정리한다. 새 timer·poller·retry
  budget은 없다.
- 교차언어 대조: `.NET`의
  `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:349–400`은 `_retries`를
  모아 NO_DATA에서 실행한다. Go도 같은 경계를 사용한다. Go의 submit goroutine 경합은
  기존 per-entry 잠금으로 합류시켜 early-record 상태를 제거했다.
- 변경 분류: **B — 기존 결함**, review 분류 `spec-impl-drift`.
- 수정 전/후 규칙 수: **2 → 1**. Capture 중 즉시 재제출과 계약의 drain 뒤 재제출을
  현재 owner의 NO_DATA 뒤 재제출 하나로 통합했다.
- 회귀 테스트: `completion_drain_order_linux_test.go:16`
  `TestWritableResubmitsOnlyAfterNoData`는 WRITABLE 다음에 REQUEST completion을 넣고,
  native 호출 순서가 SEND는 `WQNS`, REQUEST는 `WQNR`인지 검사한다.
  W=WRITABLE, Q=REQUEST completion, N=NO_DATA, S/R=재제출이다. 첫 drain의 처리 건수도
  정확히 2이며, REQUEST 재제출이 즉시 만든 completion은 다음 drain의 1건이어야 한다.
  변경 전에는 `WSQN`, `WRQQN`으로 실패했다.
- 추가 회귀 테스트: 같은 파일 `:62`의 `TestWritableRetryCancellationDuringDrain`은
  두 번째 completion의 publish/capture 합류에서 drain을 멈추고 caller를 취소한다.
  `WQN` 순서, 취소 결과 유지, entry·payload 정리를 검사한다. 변경 전에는 재제출이
  이미 발생해 실패했다.
- 검증: 두 새 테스트 각각 5회 통과. RID 테스트와 기존 connect-before-bind·혼합
  SEND/REQUEST 테스트를 포함한 race 검사도 통과했다. 전체 gate의 남은 실패는 공통
  BLOCKER다.
- Diff 분리: F-R3-1 뒤 `completion_owner.go`의 나머지 drain·capture·REQUEST attempt
  hunk, `dealer_router_request.go`의 초기 REQUEST 합류 hunk와
  `completion_drain_order_linux_test.go`. 패치는 `/tmp/zlink-go-r3/F-R3-12.patch`다.
  Test fixture는 F-R3-1 패치의 것을 재사용한다.

## F-R3-17 — HWM을 uint64 전체 범위로 투영

- 소유 계층: binding 공통 HWM 투영 계약과 Go public signature.
- Spec 조항: `bindings/doc/spec/README.ko.md:1368–1379`의 HWM 언어 투영 표,
  `bindings/doc/spec/go/README.ko.md:88–91`, `:105–109`의 uint64 전체 범위 설명.
  공개 signature 변경은 이번 작업의 D-109/D-111 사전 승인 범위다.
- 원인 위치(변경 전): `contracts/sockets.go:16–35`의 public alias를 통해
  `internal/native/connection_socket.go:21–35`, `socket_options.go:97–110`의 `int`
  setter/getter가 노출됐다. `socket_core.go:139–154`는 음수 및 MaxInt 초과 값을
  거부했다. STREAM 위임도 `socket_types.go:440–453`에서 같은 제한을 가졌다.
- 수정: connection socket, CommonSocketOptions, STREAM의 SEND/RECEIVE HWM setter와
  getter를 모두 `uint64`로 바꿨다. `setHwmOption/getHwmOption`을 삭제하고 기존 8-byte
  `setUint64Option/getUint64Option`을 직접 사용한다. 공개 코드 주석도 맞췄다.
  Lifecycle 테스트의 int 변수와 perf 공통 interface·호출부는 새 타입에 맞춰 변환했다.
  Go 샘플에는 해당 HWM 호출이 없어 소스 변경이 필요하지 않았고 7개 실행으로 검증했다.
- 교차언어 대조: Rust
  `bindings/rust/src/runtime/sockets/socket/socket_inner_runtime.rs:237–250`은 `u64`,
  .NET `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketOptions.cs:53–56`은
  `SocketOptionKey<ulong>`과 `UInt64`를 사용한다. Go의 signed int 제한만 제거했다.
- 변경 분류: **B — 기존 결함**, review 분류 `parity-gap`. 승인된 public signature
  변경이며 ABI 크기 변경으로 분류하지 않는다.
- 수정 전/후 규칙 수: **2 → 1**. 공통 uint64 범위와 Go int 범위의 충돌을 uint64
  범위 하나로 통합했다.
- 회귀 테스트: `hwm_range_test.go:32`의
  `TestHighWaterMarkRoundTripsFullUint64Range`. 8개 socket alias와 CommonSocketOptions의
  setter/getter signature를 컴파일로 검사한다. Router 직접 API와 CommonOptions를
  양방향으로 교차 조회하고 STREAM도 조회한다. SEND/RECEIVE 각각 `0`, `MaxInt64`,
  `MaxInt64+1`, `MaxUint64`를 정확히 왕복한다. 이전 구현 overlay는 `int` signature로
  컴파일 단계에서 실패했다.
- 검증: 새 테스트 5회 통과. Root package의 전체 테스트도 gate에서 통과했다.
  전체 gate의 남은 실패는 공통 BLOCKER다.
- Diff 분리: `contracts/sockets.go`, `internal/native/connection_socket.go`,
  `socket_core.go`, `socket_options.go`, `socket_types.go`, `lifecycle_test.go`,
  `perf/internal/perfcommon/common.go`, 새 `hwm_range_test.go`의 전체 변경.
  패치는 `/tmp/zlink-go-r3/F-R3-17.patch`이며 앞의 completion 수정과 독립적이다.

## Gate와 실행 근거

Linux amd64, Go 1.25.12에서 실행했다. `tests/run_tests.sh` 자체에는 local Core 선택
기능이 없으므로 `ZLINK_CORE_SOURCE=local`과 아래 compiler/linker 환경을 명시했다.

```bash
export ZLINK_CORE_SOURCE=local
export CGO_CFLAGS=-I/home/hep7/project/zlink/core/include
export CGO_LDFLAGS='-L/home/hep7/project/zlink/core/build-dev/lib -Wl,-rpath,/home/hep7/project/zlink/core/build-dev/lib'
export LD_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib
cd /home/hep7/project/zlink/bindings/go
flock /tmp/zlink-samples-gate.lock bash tests/run_tests.sh
```

`core/build-dev/lib/libzlink.so.0.17.0`과 Go packaged library의 SHA-256은 둘 다
`64567f1715b3f1527afbc1c290e2b262d02d722768e160227a6f9815bdd4bb43`였다.
Core rebuild나 package 수정은 하지 않았다.

| 검증 | 결과 | 로그 |
|---|---|---|
| 새 테스트 4개, `go test . ./internal/native -run '^Test(HighWaterMarkRoundTripsFullUint64Range\|WritableDeliversByContextAndToken\|WritableResubmitsOnlyAfterNoData\|WritableRetryCancellationDuringDrain)$' -count=5 -v` | 5회씩 통과 | `/tmp/zlink-go-r3/regressions-5x.log` |
| `bash tests/run_tests.sh` 전체 gate, 1회 | 실패: 기존 request 테스트 1개, 하위 5건 | `/tmp/zlink-go-r3/gate.log` |
| `go vet ./...` | 통과 | `/tmp/zlink-go-r3/vet.log` |
| script의 raw/hot-path guard 명령 | 통과 | `/tmp/zlink-go-r3/guards.log` |
| 새 completion 테스트 + 기존 connect-before-bind·혼합 token 테스트, `go test -race`, 1회 | 통과 | `/tmp/zlink-go-r3/race.log` |
| `flock /tmp/zlink-samples-gate.lock bash samples/run_samples.sh` | 7 pass, 0 fail | `/tmp/zlink-go-r3/samples.log` |
| `git diff --check -- bindings/go` | 통과 | 출력 없음 |

전체 script는 `set -e`로 첫 `go test ./...` 실패 뒤 종료했다. 따라서 뒤의 vet·guard와
동일한 sample script를 각각 실행해 결과를 확보했다. 샘플 실행 전체에 flock을 유지했다.
Gate가 포함한 perf package의 일반 unit test와 컴파일은 수행했으며 benchmark는 실행하지
않았다. WRITABLE native fixture는 Linux linker wrapping을 사용하는 테스트 전용 package다.
Production binding과 샘플은 이 package를 import하지 않는다.

원인별 패치 3개를 별도의 `/tmp/zlink-go-r3/split-check`에 F-R3-1 → F-R3-12 → F-R3-17
순서로 적용해 14개 Go 파일이 현재 결과와 byte 단위로 같은지 확인했다. 보고서는 각 패치에
중복 포함하지 않았다.

## BLOCKERS

**전체 gate 0 failures를 막는 기존 Core admission/wake 동작이 남아 있다.**

- 실패 위치: `bindings/go/internal/native/request_writable_retry_test.go:60`.
  `TestPublicRequestRetriesExactPacketAfterWritable/run-0`부터 `run-4`까지 첫
  `route-prime` DATA의 `router.Recv()`가 `(false, nil)`을 반환한다. REQUEST 재제출
  검증에 도달하기 전이다. 해당 assertion·HWM·timeout은 바꾸지 않았다.
- 원래 completion 파일 2개를 HEAD 내용으로 돌린 Go overlay와 새 native fixture를
  제외한 테스트 빌드에서도 `run-0`이 같은 위치에서 실패했다.
  `/tmp/zlink-go-r3/request-prime-baseline.log`와 `baseline-overlay.json`에 보존했다.
- 공개 Go API 재현: `/tmp/zlink-go-r3/request-prime-repro.go`.
  Auto HWM을 끄고 DEALER SNDHWM=1, ROUTER RCVHWM=1, inproc bind/connect 후 11-byte
  `route-prime`을 제출한다. Receive가 끝나도 Submit은 대기 중이며 caller cancellation으로
  정리된다. 로그: `/tmp/zlink-go-r3/request-prime-repro.log`.
- Binding을 거치지 않는 공개 C API 재현:
  `/tmp/zlink-go-r3/request-prime-repro.c`. 동일 조건에서 단일 completion poller를 사용하고
  DONTWAIT 제출 및 NO_DATA drain 계약을 지킨다. 현재 local Core의 결과는 다음과 같다.

```text
send attempt 0: result=1 errno=11 id=1
prime recv: result=201 errno=11
completion readiness: count=0 errno=0
```

Native 제출은 `BACKPRESSURED/EAGAIN`과 token 1을 반환했고, 기존 receive 기본 대기 및
1초의 completion readiness 관찰에서 DATA나 WRITABLE이 나오지 않았다. 로그는
`/tmp/zlink-go-r3/request-prime-native.log`다. 다음 명령으로 재현할 수 있다.

```bash
cc -Icore/include /tmp/zlink-go-r3/request-prime-repro.c \
  -Lcore/build-dev/lib -Wl,-rpath,/home/hep7/project/zlink/core/build-dev/lib \
  -lzlink -pthread -o /tmp/zlink-go-r3/request-prime-repro
/tmp/zlink-go-r3/request-prime-repro
```

이는 Go의 RID 재검증이나 drain 내부 재제출을 거치기 전의 Core admission/wake 경계다.
정확한 Core 내부 원인은 이번 범위에서 확정하지 않았다. Core 담당자가 이 공개 API
재현을 조사해야 한다. `core/**` 변경은 금지된 범위이므로 보상 코드·추가 재시도·timeout
증가·fixture 완화를 넣지 않았다. Core 원인 해결 후 Go 전체 gate 재검증이 필요하다.
