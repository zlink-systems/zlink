# Java L5~L7 request-backpressure 진단

## 판정

판정은 **bench configuration/call-shape fault**다. L5~L7 bench의 **admission 단계 전달 누락**이 원인이다. Framework runtime 결함이나 Framework 전용 in-flight 제한으로 분류하지 않는다. Main bench는 `admitted()`를 기다린 뒤 같은 stream의 다음 request를 제출하지만, ladder는 reply future만 반환해 이 대기를 생략한다. 고정 window 추가나 timeout·capacity 증가는 필요하지 않다.

초기 소스는 `/root/worktree/zlink-v21-s033/lad-java-l57p`, branch `exp/4350-java-ladder-l57p`, commit `ee7e5c3343bda3bf2069395b66fbe39fc7eb5507`이다. 최초 `git status --short`는 비어 있었다. 이 보고서는 실패 재현의 결과이며 처리량 성능 판정을 하지 않는다.

## 호출 형태와 오류 발생 지점

Bench client 경로는 `framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/`이다. Production Java 경로는 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/`이다. [네 경로 비교](call-shape.md)에 binding/L4, L5~L7, main의 호출 형태를 기록했다.

| 결정 | 소유자·근거 | 이 bench에서의 값·동작 |
|---|---|---|
| 다음 request 제출 | bench `BenchDrivers.java:583-600,610-621` | `BenchRequestOperation`일 때 admission을 기다린다. 단순 `BenchOperation`이면 기다리지 않는다. Submit thread 8개다. |
| Ladder의 반환 단계 | bench `FrameworkStack.java:177-191` (초기 commit) | `ladderFrameworkRequest()`가 `BenchOperation`으로 reply만 반환한다. `admitted()`를 driver에 전달하지 않는다. |
| Main의 반환 단계 | bench `FrameworkStack.java:93-118` | `BenchRequestOperation.Submission(submitted.admitted(), replyCompletion)`을 반환한다. |
| Binding/L4의 제출 진행 | bench `RawStack.java:162-193`, `BenchDrivers.java:262-308` | binding의 `BACKPRESSURED` 결과이면 `submission.admitted()`를 기다린다. Framework만의 별도 무제한 경로가 아니다. |
| Capacity 오류 문자열 | production `runtime/binding/ZLinkJavaRawMeshNode.java:4763-4784` | typed `BACKPRESSURED` 실패를 `runtime/internal/backend/ZLinkBackendRequestResult.java:46`의 매핑으로 `ZLinkFrameworkException(UNAVAILABLE, "Operation failed because submission capacity is unavailable.")`로 투영한다. |
| Binding의 token 유무 판정 | `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:168-213,842-856` | `BACKPRESSURED`이며 completion ID가 nonzero이면 admission 대기 stage를 반환한다. ID가 0이면 `ZlinkSubmitException`으로 실패한다. |
| Token 없는 실제 제한 | `core/src/api/socket/socket_completion_queue_internal.hpp:26-29`, `socket_completion_queue_internal.cpp:98-112,236-250` | socket당 unified completion reservation **65,536개**, Core 고정 상수다. Bench override는 없다. Pending REQUEST, 아직 dequeue하지 않은 completion, WRITABLE 대기 token이 공유한다. |
| Request의 slot 확보 | `core/src/api/socket/socket_request_reply_pending_api.cpp:309-324`, `socket_request_reply_submit_api.cpp:555-562,608-609` | wire 제출 전에 slot을 확보한다. 포화면 `EAGAIN`, `BACKPRESSURED`, completion ID 0으로 끝난다. |
| Pair admission window | `core/src/runtime/core/transport_pair_policy.hpp:14-18`, `pipe_receive.cpp:640-670` | pair당 correlation count 16,384와 weighted work 32 MiB가 별도 존재한다. 포화에는 WRITABLE token을 주어 기다린다(`socket_request_reply_submit_api.cpp:269-287`). Socket slot 포화와 구분한다. Binding/L4/main도 같은 Core 정책을 사용한다. |
| Framework registry | production `runtime/internal/service/ZLinkServiceOperationRegistry.java:39,698-741` | `ConcurrentHashMap`에 entry를 등록한다. 이 경로에는 registry size 또는 Framework in-flight 상한에 따른 capacity 오류가 없다. |
| Application queue capacity | production `runtime/configuration/ZLinkInboundDispatchRegistration.java:21-24`; `runtime/internal/dispatch/ZLinkApplicationJobQueue.java:508-553` | 수동 override 없이 BALANCED 자동 계산(유효 processor당 128 jobs)이다. 송신 Core completion slot과 다른 자원이다. Bench HTTP stats는 queue depth·Core slot 사용량을 노출하지 않는다. |

`--request-window=100`은 runner metadata에 남지만 request-backpressure에는 적용하지 않는다. `BenchOptions.java:39-44`가 기본값을 정하며, `BenchDrivers.java:205-211`은 window 경로와 backpressure 경로를 분리한다. 따라서 문제는 window 100과 Framework 제한의 충돌이 아니라 **admission을 무시한 연속 제출**이다. Binding/L4에 제한이 없다는 가정은 틀리다.

## Timeout과 스펙

요청 timeout은 **30,000 ms**다(`BenchOptions.java:43`, `FrameworkStack.java:88-90,179-182`). `runtime/channels/ZLinkChannelSocketRegistry.java:509-532`는 진입 시각부터 경과한 시간을 차감한다. `runtime/internal/service/ZLinkServiceOperationRegistry.java:341-365`는 절대 deadline을 정하고 owner turn 대기 시간을 포함하며, `:617-635`는 기한이 지난 entry를 timeout으로 완료한다. 문자열 `service operation timed out`은 `runtime/channels/ZLinkChannelCallRuntime.java:98-103`에서 생성한다.

[Framework submit/completion spec](../../../../../../worktree/zlink-v21-s033/lad-java-l57p/framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md)은 다음을 정한다.

| spec file:line | caller가 보는 동작 |
|---|---|
| `01-execution/01-submit-and-completion.ko.md:61-71` | request의 `admitted`는 source-local admission 경계를 나타낸다. 다음 제출을 늦추려면 이 단계를 기다린다. |
| 같은 파일 `:182-197` | local capacity와 token이 있는 Core backpressure는 기다린다. **token 없는 Core completion slot 포화는 즉시 `Unavailable`**이다. `Backpressured` 자체를 public terminal로 공개하지 않는다. |
| 같은 파일 `:361-369` | 진입부터 admission·owner turn 대기·reply 전체가 request timeout에 포함된다. 시간이 지나면 `DeadlineExceeded`다. Timeout 후 physical request가 늦게 전달될 수 있다. |
| 같은 파일 `:525-541` | completion dispatcher는 무제한이다. 진행 중인 송신 호출의 제한은 Core가 소유한다. 수신 application queue pressure와 별개다. |
| `core/doc/spec/core/socket/README.ko.md:1132-1138,1219-1224` | socket당 65,536 reservation과 slot이 차면 ID 0을 반환하는 계약을 명시한다. |

Bench 패턴의 기준은 `framework/bench/grpc/README.ko.md:95-101`이다. Reply 수에는 application 상한을 두지 않되 **admission backpressure에서 기다리고 재개**한다. 오류 매핑은 스펙에 맞지만 ladder의 driver 연결은 이 bench 패턴과 다르다.

## Timeout 관측

최초 진단은 `BENCH_FLOW_TRACE=1`로 기존 Normal message-flow를 켜고 source/target file log를 보존했다. [trace-analysis.json](raw/trace-analysis.json)은 원본 로그의 line 번호와 flow ID를 기록한다. Source의 `sent`부터 `reply_received outcome=failed`까지 30.001초 걸린 flow `01a1d6a0-ebdd-7410-9f64-ad8f90f8b90d`가 있다(source.log:2616 → :304508). 0.002초 뒤 실패한 flow도 있다(:90731 → :90733). 이 두 시간대는 30초 deadline과 즉시 capacity 거절의 코드 경로에 부합한다.

[진단 timeline](raw/diag-L5_7/request-backpressure-1024/warm/run1/zlink-framework-java-request-backpressure-1024/stats-timeline.jsonl)에는 source의 **동시 currentInFlight 74,245개**가 기록됐다. 같은 표본에서 submitted 166,788, completed 12,035, errors 80,509였고 target은 17,125개를 수신했다. 제출이 처리보다 빠르고 논리적 요청 대기가 30초를 넘는다. 이는 source owner/admission/reply 경로 전체의 대기이며 target handler 실행 시간 하나로 분리한 값은 아니다. Trace를 끈 4K counted 실행에서도 target의 관측 p99 도착 지연이 **32,327.174341 ms**였다([timeline-peaks.json](raw/timeline-peaks.json)의 baseline-L5_7/4096/counted active 행). 이는 `BenchServerMetrics.java:46-57`이 request header의 제출 시각부터 handler의 metrics 기록까지 잰 값으로, 30,000 ms deadline을 넘는다.

`peak_in_flight`는 `BenchDrivers.java:914-918`에서 stream별 peak를 합산하므로 실제 동시 최대의 상계다. 따라서 slot 한도 초과의 실측 근거는 합산 peak만이 아니라 위 `currentInFlight` 표본이다. Logical in-flight에는 아직 Core slot을 얻지 않은 Framework 작업도 포함된다. Core slot 사용량 자체와 owner/application queue depth는 `/bench/stats`에 노출되지 않아 측정하지 않았다.

4K counted의 집계도 같은 경계를 보인다([results.json](raw/baseline-L5_7/request-backpressure-4096/counted/run1/zlink-framework-java-request-backpressure-4096/results.json)). `submitted − received = 165,553 − 72,221 = 93,332`는 capacity 오류 수와 같고, `received − completed = 72,221 − 34,279 = 37,942`는 timeout 수와 같다. Target의 오류는 0이다. Timeout은 target이 request를 받지 않았다는 뜻이 아니다.

기존 trace의 실패 event에는 `errorType`/`errorMessage`가 없었다. 개별 flow의 오류 종류는 추정하지 않고, 오류 종류별 정확한 건수는 같은 run의 `results.warmup.json`과 source의 `client_error` 집계를 사용했다. Trace-on 실행은 계측 부하가 있으므로 처리량 비교에서 제외한다. 이후 대조는 모두 기본 trace 설정으로 실행했다.

## 재현과 검증

모든 raw는 `jl57bp/raw/`에 보존한다. 아래 표에서 `warm` 디렉터리는 버린 선행 실행이고, `counted` 디렉터리는 뒤의 진단 실행이다. 각 실행 내부의 10초 warmup은 `results.warmup.json`, 5초 active는 `results.json`에 따로 기록된다. **이번 1K active 오류는 재현되지 않았고, 내부 warmup에서 두 오류 종류가 재현됐다.** 4K는 내부 warmup과 active에서 모두 재현됐다. 따라서 1K active에서 항상 실패한다고 주장하지 않는다.

<!-- RESULT_TABLES -->

전체 선행 warm·counted 실행의 phase별 수치는 [results-summary.json](raw/results-summary.json)에 있다. 오류가 난 실행은 throughput gate의 유효 셀로 사용하지 않는다. Main과 ladder는 payload 크기·submit thread 수·deadline·CPU 조건이 같고 serializer는 각각 protobuf/typed JSON이다. 수정 후에는 JSON을 유지한 채 admission 전달만 바꾸므로 serializer 변경을 원인 제거로 사용하지 않는다.

초기 소스에서 실행한 명령은 다음과 같다. `run-group.sh`는 source `taskset -c 0-5`, target `taskset -c 8-13`, `chrt -o 0`, warmup 10초·active 5초, submit concurrency 8, request timeout/drain bound 30,000 ms를 적용한다. 셀이 바뀔 때 별도의 discarded warm run을 먼저 실행한다.

```bash
cd /root/worktree/zlink-v21-s033/lad-java-l57p
JAVA_HOME=/usr/lib/jvm/java-25-openjdk-amd64 \
PATH=/usr/lib/jvm/java-25-openjdk-amd64/bin:$PATH \
framework/bench/grpc/java/gradlew --no-daemon --max-workers=1 assemble installDist

JOB=/root/jobs/fx/cx/v21/s033/jl57bp
flock -x /root/jobs/fx/cx/measure.lock \
  timeout --signal=TERM --kill-after=10s 580s \
  bash "$JOB/tools/run-group.sh" diag-L5_7 1024 on
for mode in baseline-L5_7 baseline-main; do
  flock -x /root/jobs/fx/cx/measure.lock \
    timeout --signal=TERM --kill-after=10s 580s \
    bash "$JOB/tools/run-pair-locked.sh" "$mode"
done
```

모든 runner는 `/root/jobs/.../jl57bp/tools/java/`에 복사한 runner를 사용했다. `scheduling.txt`에 source/target의 모든 thread policy와 `Cpus_allowed_list`를 시작·mid-active에 기록했다. `stats-timeline.jsonl`은 0.5초 간격의 공개 `/bench/stats` 응답이다. `artifacts.txt`는 해당 worktree 설치 JAR의 SHA-256과 HEAD·dirty 상태·merge base를 기록한다. [build-baseline.log](raw/build-baseline.log)는 초기 설치의 로컬 빌드 성공 기록이다.

초기 build script는 load-average 제한으로 실행을 거부했다([build.log](raw/build.log)). 이후 같은 JDK와 Gradle `assemble installDist`를 직접 실행했다. 빌드는 measure.lock 밖에서 수행했고 quiet guard의 정지 정책을 따랐다. 첫 직접 실행에는 wrapper에 `-p`를 중복 전달해 실패했으며([build-gradle.log](raw/build-gradle.log)), 위 명령으로 바로잡은 최종 빌드는 67개 task가 up-to-date로 성공했다. 실패한 두 명령은 측정을 시작하지 않았다.

원인만 재현하는 public 호출 형태는 다음과 같다. 같은 source의 application thread 8개가 미완료 reply를 보관하면서 이 호출을 반복한다. `main`은 아래 반환값에 `request.admitted()`도 함께 담아 driver가 admission을 기다리게 한다.

```java
var request = route.requestToNode(meshName, serverRid, typedPayload)
        .timeout(Duration.ofMillis(30_000))
        .submit(BenchTypedPayload.class);
return request.thenAccept(this::validateReply).toCompletableFuture();
```

원본 설치는 [baseline-installs.tar](raw/baseline-installs.tar)와 해당 [SHA-256](raw/baseline-installs.tar.sha256)에 보존한다. 최종 수정 뒤 초기 실패를 다시 실행할 때는 이 archive를 별도 디렉터리에 풀고 복사본 runner의 `BENCH_HOME_OVERRIDE`를 그 디렉터리로 지정한다. Archive에는 원래의 client/server 설치와 의존 JAR가 들어 있으며 production 소스를 되돌릴 필요가 없다. 기존 raw 출력 디렉터리는 runner가 재사용하지 않으므로 새 `OUTPUT_DIR`과 `RUN_STAMP`를 지정한다.

수정 뒤에도 원본 binary로 재현하려면 다음 명령을 사용한다. `REPRO`는 현재 worktree 안의 별도 설치 경로다. 이 명령은 설명용이며 이 작업에서는 위 초기 commit 명령으로 재현했다. `OUTPUT_DIR`은 매번 새 이름이어야 한다.

```bash
ROOT=/root/worktree/zlink-v21-s033/lad-java-l57p
JOB=/root/jobs/fx/cx/v21/s033/jl57bp
REPRO="$ROOT/.jl57bp-original-installs"
mkdir -p "$REPRO"
tar -xf "$JOB/raw/baseline-installs.tar" -C "$REPRO"
for size in 1024 4096; do
  for kind in warm counted; do
    stamp="repro-$size-$kind-$(date -u +%Y%m%dT%H%M%SZ)"
    flock -x /root/jobs/fx/cx/measure.lock \
      timeout --signal=TERM --kill-after=10s 580s env \
      BENCH_HOME_OVERRIDE="$REPRO" ZLINK_BENCH_LADDER_L5_7=1 \
      SKIP_BUILD=1 RUNS=1 IMPLEMENTATIONS=zlink-framework-java \
      PATTERNS=request-backpressure PAYLOAD_SIZES="$size" \
      WARMUP_SECONDS=10 DURATION_SECONDS=5 SOURCE_CPUS=0-5 TARGET_CPUS=8-13 \
      OUTPUT_DIR="$JOB/raw/$stamp" RUN_STAMP="$stamp" \
      bash "$JOB/tools/java/run_local_binding.sh"
  done
done
```

Main 원본 대조에서는 위 환경의 `ZLINK_BENCH_LADDER_L5_7`을 제거하고 ladder 환경 변수도 모두 unset한다. 설치 archive는 동일하며 main의 public `requestToNode` 경로를 실행한다.
