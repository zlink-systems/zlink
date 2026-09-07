# Go request 동시 제출 진단

## 1. 판정

관측 범위는 **(b) 단일 part request는 동시에 제출할 수 있지만 multipart request는
실패한다**에 해당한다. 이 차이를 Go binding 결함인 (c)로 고칠 수는 없다. Core와 binding
스펙이 다음 요구를 동시에 두고 있으나, Go의 public request terminal이 이를 만족할 방법을
정의하지 않았기 때문이다.

- Core socket API는 기본적으로 thread-safe하며 `send`를 여러 thread에서 동시에 호출할 수 있다
  (`core/doc/spec/core/socket/README.ko.md:44-58`). Multipart 일반 계약도 여러 thread가 각자
  독립된 message를 보낼 수 있다고 한다. 단, multipart message 하나의 part는 thread 사이에
  나누지 않는다(`core/doc/spec/core/02-message.ko.md:95-110`).
- Core는 `MORE`부터 `FINAL`까지 socket별 transaction state 하나로 보호한다
  (`core/doc/spec/core/02-message.ko.md:416-430`). Part send 계약도 `MORE`를 socket-local
  sequence에 staging한다고 정의한다
  (`core/doc/spec/core/socket/README.ko.md:921-947`).
- Request `MORE`에는 timeout, context와 completion output을 전달하지 않는다
  (`core/include/zlink/socket/api.h:253-270`,
  `core/doc/spec/core/socket/README.ko.md:1055-1063`). 따라서 `MORE` 경합 실패에는 binding이
  기다릴 exact WRITABLE token이 없다.
- Part API를 사용하는 binding은 송신 경로에 자체 lock이나 gate를 둘 수 없다
  (`bindings/doc/spec/README.ko.md:1346-1353`). Go 스펙은 같은 socket에 동시에 제출한
  multipart record의 part가 섞이지 않아야 한다고 정한다
  (`bindings/doc/spec/go/README.ko.md:137-145`).
- C++ binding 스펙만 application이 같은 socket의 concurrent multipart submit을 직렬화해야
  한다고 명시한다(`bindings/doc/spec/cpp/README.ko.md:455-461`). Core 공통 request 계약과
  Go binding 계약에는 이 application 책임과 거절 result가 없다.

고정 Core 구현은 열린 sequence의 owner와 다른 thread가 진입하면 `EINVAL`로 거절한다
(`core/src/api/socket/part_helper_api.cpp:169-186`). 다른 complete-record admission과 첫 `MORE`가
경합하면 `DONTWAIT` 호출을 `EAGAIN`으로 바꾼다
(`core/src/api/socket/part_helper_api.cpp:86-108`). 이 파일은 `core/v0.17.1`부터 현재 HEAD까지
변경되지 않았으며, 고정 binary에서 두 결과를 모두 재현했다.

결론적으로 Core는 part 삽입을 막아 record 원자성은 지키지만, concurrent multipart request를
성공시키지는 않는다. 동시에 다음 사항이 정해져 있지 않다.

- Core request `MORE` 경합의 public result와 기존 sequence 보존·폐기 규칙
- `MORE`에서 반환한 token 없는 `BACKPRESSURED(EAGAIN)`을 고수준 binding이 처리하는 방법
- socket 단위 lock/gate 없이 blocking Go terminal 여러 개로 같은 socket의 multipart request를
  여러 건 outstanding 상태로 만드는 방법

따라서 변경 분류는 **D — spec gap**이다. `bindings/go/internal/native`와 Go perf runner는
수정하지 않았다.

## 2. 고정 Core와 재현 조건

모든 실행은 다음 고정 Core를 사용했다. Core를 빌드하거나 `--core-version`을 사용하지 않았다.

```text
ZLINK_CORE_SOURCE=release
ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.1
lib/libzlink.so.0.17.1 sha256=a3e00fd269b2a1c8d66371ac7ae7efd3f6dd25e6ab842484b847352258ea39a2
```

Public Go API만 사용하는 최소 재현은 `/tmp/zlink-go-request-repro/main.go`에 두었다. 같은 DEALER
socket에서 OS thread에 고정한 caller 2개가 각각 100회 request를 제출하고 ROUTER가 즉시
reply한다.

| part 수 | 성공 | 실패 | 실패 분류 |
|---:|---:|---:|---|
| 1 | 200 | 0 | 없음 |
| 2 | 188 | 12 | `SubmitBackpressured(EAGAIN)` 10, `SubmitInvalidArgument(EINVAL)` 2 |

`PERF_SINGLE_REQREP_MAX_OUTSTANDING=2`, tcp, 64 B, 1초, 1회 측정 결과도 같은 경계를 보였다.

| `PERF_PART_COUNT` | 결과 | 처리량 |
|---:|---|---:|
| 1 | complete, RESULT 5/5 | 17,744 ops/s |
| 2 | `SubmitInvalidArgument(EINVAL)`, RESULT 없음 | 유효 결과 없음 |

Multi는 requester socket 1개(`--clients 1`)와
`PERF_MULTI_REQREP_MAX_OUTSTANDING=2`로 확인했다.

| `PERF_PART_COUNT` | 결과 | 처리량 |
|---:|---|---:|
| 1 | complete, RESULT 5/5 | 15,365 ops/s |
| 2 | client가 완료되지 않아 runner가 exit 141, RESULT 없음 | 유효 결과 없음 |

현재 dirty multi runner는 public terminal의 token 없는 `SubmitBackpressured`를 새 request 제출로
처리한다(`bindings/go/perf/multi/perf_multi_socket_reqrep.go:251-265`). 이 경로에서는 `MORE`를
재개할 exact token이 없으므로 실행이 완료되지 않는다. 이를 timeout, 반복 제출 또는 error 무시로
우회하지 않았다.

## 3. 다른 언어와 C 기준 러너 대조

다른 러너도 socket 하나에 reply를 기다리는 request를 여러 건 유지한다. 차이는 outstanding
request 수가 아니라 native part sequence의 실행 thread와 순서다.

| 경로 | socket당 outstanding | native `MORE` → `FINAL` 실행 |
|---|---|---|
| C multi 기준 | runner cap 없음. Core가 backpressure를 반환할 때까지 증가하며 socket당 completion reservation hard ceiling은 SEND와 합쳐 65,536개다. | benchmark thread 하나가 socket마다 request 한 건씩 순차 제출한다. 한 helper 호출이 `MORE`와 `FINAL`을 모두 끝낸 뒤 다음 request로 간다(`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:218-340,527-603`, `bindings/c/perf/common/perf_zlink_part_helpers.hpp:156-206`). |
| C++ multi | 기본 cap 64 | detached coroutine을 application ready queue thread에서 시작하며 `begin_request()`가 native submit을 끝낸 뒤 처음 suspend한다. 같은 socket의 native sequence는 순차 실행된다(`bindings/cpp/perf/multi/common/perf_multi_reqrep.hpp:354-435,465-499`). |
| Rust single·multi | 기본 cap 64 | Future는 생성 thread 하나에서 순차 poll된다. `ConcurrentTasks`도 Future poll과 slot 변경을 생성 application thread로 제한한다(`bindings/rust/perf/multi/src/perf_common.rs:131-165,195-228`, `bindings/rust/perf/multi/src/perf_multi_socket_reqrep.rs:260-310`). |
| .NET multi | 기본 cap 64 | `Async()` 호출이 native submit을 동기 실행하고, `CompletionOwner.RequestAsync()`는 `_submitSync`를 잡은 채 전체 part를 제출한다(`bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:155-210,535-578`). |
| Go single·multi | 기본 cap 64 | public `Submit(context.Context)`가 admission과 reply completion을 함께 기다린다. 여러 outstanding을 만들려면 여러 goroutine이 같은 socket의 native part sequence에 동시에 진입한다. |

.NET의 `_submitSync`는 현재 실패를 막지만 socket 단위 송신 lock이므로 공통 binding 스펙
`bindings/doc/spec/README.ko.md:1346-1353`과 충돌한다. 다른 언어가 정상 측정된 사실을 Go에도
같은 lock을 추가할 근거로 사용할 수 없다.

## 4. Go binding과 backpressure 판별

Go `submitCompletionRequest`는 operation별 `entry.attemptMu`만 잡고 builder part를 `MORE`부터
`FINAL`까지 제출한다(`bindings/go/internal/native/dealer_router_request.go:287-388`). 이 lock은
같은 operation의 initial attempt와 WRITABLE retry 경합을 막으며, 다른 request를 socket 단위로
직렬화하지 않는다. 이 구조는 binding 자체 lock/gate 금지 조항에 맞는다.

공통 결과 투영은 `BACKPRESSURED(EAGAIN)`과 nonzero 대기 token이 함께 있을 때만 payload를
보관하고 WRITABLE 뒤 같은 operation을 재개한다. Token 없는 submit failure는 terminal로 끝낸다
(`bindings/doc/spec/README.ko.md:1326-1344`). Go public contract도 exact WRITABLE token 뒤의 재개를
명시한다(`bindings/go/contracts/sockets.go:41-45`).

따라서 이번 single의 `SubmitBackpressured(EAGAIN)`은 Go가 재개해야 할 token을 놓친 결과가 아니다.
Core가 `MORE` 단계에서 ID 0으로 반환한 실패이므로 현재 binding이 caller에게 terminal error로
전달하는 것은 결과 투영에 맞는다. 문제는 request `MORE` 경합을 backpressure로 분류하면서 token을
제공하지 않는 Core 동작과, 이 경합을 다루지 않는 Core request 스펙 사이에 있다.

## 5. 검토한 해법

| 해법 | 판정 |
|---|---|
| socket 단위 mutex나 submission queue로 native part sequence 직렬화 | binding 송신 lock/gate 금지 위반 |
| token 없는 `MORE` `EAGAIN`을 즉시 또는 timer로 재제출 | exact WRITABLE token 계약과 busy-retry 금지 위반 |
| Go public terminal을 admission과 reply wait로 분리 | public API signature와 비동기 terminal 스펙 결정 필요 |
| outstanding마다 별도 socket 사용 | single benchmark topology와 multi client 수 의미를 바꾸므로 C 기준과 다른 측정 |
| runner가 terminal error를 무시하고 새 request 제출 | 실패 은폐이며 logical request를 이어서 재개하지 않음 |

설명해야 할 규칙 수는 변경 전과 후가 같다. 우회 규칙, 상태, timer 또는 gate를 추가하지 않았다.

## 6. 검증

| 검증 | 결과 |
|---|---|
| `go test ./perf/...` | 통과 |
| `go test ./...` | 기존 dirty `perf/internal/perfcommon/monotonic.go`의 직접 cgo 사용을 `boundary_test.go:148`이 거부해 실패. 나머지 package는 통과 |
| `go vet ./...` | 통과 |
| raw contract·hot-path guard | 통과 |
| `git diff --check` | 문서 작성 후 별도 확인 |

전체 테스트 실패는 이번 진단에서 만든 변경이 아니다. 작업 시작 전부터 있던 untracked
`bindings/go/perf/internal/perfcommon/monotonic.go`가 `import "C"`를 사용하며, sample/perf가 public
binding contract만 사용해야 한다는 기존 boundary test와 충돌한다.

## 7. 감독자 판단 필요 사항

다음 중 하나를 스펙에서 먼저 선택해야 구현을 진행할 수 있다.

1. Core가 같은 socket의 독립된 multipart message를 여러 thread에서 성공적으로 제출하도록
   transaction identity 또는 동등한 내부 계약을 제공한다.
2. Concurrent multipart는 application 책임으로 직렬화한다고 Core·모든 binding 스펙에 명시하고,
   Go처럼 admission과 reply wait가 합쳐진 terminal에서 여러 outstanding을 만드는 표면을 별도로
   정한다.
3. Binding의 socket 단위 native part sequence gate를 허용하도록 공통 lock/gate 금지 조항을
   바꾼다. 이 선택은 .NET의 현재 `_submitSync`와 일치하지만 기존 공통 계약을 변경한다.

이 결정 전에는 Go binding 또는 perf runner 수정으로 유효한 2-part concurrent request 측정을
만들 수 없다. Core 수정, 스펙 수정, public API 변경, commit과 push는 수행하지 않았다.
