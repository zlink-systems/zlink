# Java send 프로파일 결과

## 1. 어떻게 쟀나

2026-09-13에 JDK Temurin 25.0.4.1로 Java benchmark source 프로세스만
프로파일했다. perf 판정 실행은 하지 않았다. 실행 전
`bash scripts/perf/perf-ticket.sh status`에서 실행 중·대기 작업이 없음을 확인했다.

`framework/bench/grpc/java/run_local.sh`는 source를 다음 launcher로 `setsid` 실행한다.

```text
client/build/install/bench-client/bin/bench-client
```

이 Gradle launcher는 `DEFAULT_JVM_OPTS`, `JAVA_OPTS`, `BENCH_CLIENT_OPTS`를 JVM
인수로 전달한다. 하지만 `StartFlightRecording`을 launcher 시점에 주면 source 준비와
20초 warmup까지 섞인다. 따라서 runner와 같은 target/source/HTTP trigger 경로를 직접
실행하고, warmup 완료 뒤 source PID에 JFR을 시작했다. 이 방식은 wire, 29-byte
measurement header, benchmark header를 변경하지 않는다.

두 구현 모두 조건은 다음과 같다.

| 항목 | 값 |
|---|---:|
| scenario | `send-saturation` |
| payload | 1 KiB |
| warmup | 20 s |
| active | 15 s |
| send concurrency | 8 |
| JFR | active 시작 직전부터 `settings=profile`, `duration=12s` |

재현에 사용한 핵심 명령은 다음과 같다. target와 source 인수는
`run_local.sh`의 해당 implementation 분기와 동일하다. framework는 5252--5255,
raw는 5245--5249를 사용했다.

```bash
BENCH="$PWD/framework/bench/grpc/java"
source "$BENCH/runner_common.sh"
select_java_home
export PATH="$JAVA_HOME/bin:$PATH"

# implementation별 runner와 같은 target를 setsid로 시작하고 wait_for_stats 한다.
# framework target:
setsid "$BENCH/zlink-framework-server/build/install/bench-zlink-framework-server/bin/bench-zlink-framework-server" \
  --endpoint tcp://127.0.0.1:5254 --metrics-url http://127.0.0.1:5255 &
# raw target: bench-zlink-raw-server --endpoint tcp://127.0.0.1:5247 \
#   --command-endpoint tcp://127.0.0.1:5248 --metrics-url http://127.0.0.1:5249

# implementation별 runner의 source_args 그대로 source를 시작한다.
setsid "$BENCH/client/build/install/bench-client/bin/bench-client" \
  --implementation zlink-framework-java --scenario send-saturation \
  --payload-size 1024 --request-window 100 --send-concurrency 8 \
  --latency-sample-limit 200000 --warmup-seconds 20 --drain-bound-ms 30000 \
  --request-timeout-ms 30000 --route-ready-ms 30000 \
  --trigger-url http://127.0.0.1:5252 --stats-url http://127.0.0.1:5253 \
  --target-endpoint tcp://127.0.0.1:5254 --target-stats-url http://127.0.0.1:5255 \
  --run-id profile-zlink-framework-java \
  --cell-id zlink-framework-java-send-saturation-1024 --raw-socket router \
  --output /tmp/zlink-java-send-profile-20260913/zlink-framework-java-out \
  --report-file report.txt &

# wait_for_stats, trigger warmup 20000, wait_for_idle 뒤에 실행한다.
jcmd "$SOURCE_PID" JFR.start name=framework-send settings=profile \
  filename=/tmp/zlink-java-send-profile-20260913/zlink-framework-java-active-12s.jfr \
  duration=12s
# 동일한 trigger로 active 15000을 시작하고 wait_for_idle 한다.
```

raw는 위 source 명령에서 implementation과 URL을 runner의 raw 분기로 바꾸고
`--target-command-endpoint tcp://127.0.0.1:5248`을 추가했다. JFR 파일은 다음에 남아 있다.

```text
/tmp/zlink-java-send-profile-20260913/zlink-framework-java-active-12s.jfr
/tmp/zlink-java-send-profile-20260913/zlink-java-active-12s.jfr
```

집계에는 다음을 사용했다. ExecutionSample의 비중은 stack의 leaf frame 수 / 전체
ExecutionSample 수다. ObjectAllocationSample의 `weight`는 JFR의 추정 할당량이다.

```bash
jfr print --json --events jdk.ExecutionSample "$FILE" | jq '...'
jfr print --json --events jdk.ObjectAllocationSample "$FILE" | jq '...'
```

| source | ExecutionSample 구간 | ExecutionSample | ObjectAllocationSample |
|---|---|---:|---:|
| `zlink-framework-java` | 04:42:43.325--04:42:55.056 | 276 | 3,434 |
| `zlink-java` | 04:43:21.916--04:43:33.872 | 791 | 3,536 |

직접 실행의 source 결과는 framework 906,280 completed (60.419 KMSG/s), raw
11,071,622 completed (738.108 KMSG/s)였다. 이것은 JFR 중 active 결과와 아래
message당 추정량의 분모를 확인하기 위한 관측값일 뿐, `perf-ticket.sh submit`으로
수집한 throughput 판정값이 아니다.

## 2. 시간이 어디로 가나

다음은 JFR `jdk.ExecutionSample`의 leaf frame이다. 각각은 측정된 표본의 비중이며,
호출 횟수나 코드 독해로 계산한 비용이 아니다.

### `zlink-framework-java` (276 표본)

| leaf frame | 표본 | 비중 |
|---|---:|---:|
| `java.nio.MappedByteBuffer.limit:339` | 25 | 9.06% |
| `java.util.concurrent.ForkJoinPool.deactivate:2071` | 17 | 6.16% |
| `ZLinkChannelEnvelope$HeaderWriter.encode:694` | 9 | 3.26% |
| `Message.releaseOwnedResources:1076` | 9 | 3.26% |
| `SegmentFactories.makeNativeSegmentUnchecked:70` | 9 | 3.26% |
| `ForkJoinPool.signalWork:1899` | 8 | 2.90% |
| `Sink$ChainedReference.<init>:251` | 7 | 2.54% |
| `Spliterator.getExactSizeIfKnown:414` | 7 | 2.54% |
| `ThreadLocalMap.getEntryAfterMiss:513` | 7 | 2.54% |
| `ZLinkServiceM6AWireCodec.writeFrameworkMultipartFrame:363` | 5 | 1.81% |

나머지 179개 표본(64.86%)은 위 leaf가 아닌 프레임에 분산됐다. framework 표본에는
이름 없는 virtual thread가 214개 있었다. 따라서 위 표는 한 framework 메서드의
독점 CPU 비율이 아니라 source JVM 전체 runnable 표본의 분포다.

### `zlink-java` raw (791 표본)

| leaf frame | 표본 | 비중 |
|---|---:|---:|
| `Message.from:285` | 311 | 39.32% |
| `RawStack$2.submitRaw:82` | 300 | 37.93% |
| `CodedOutputStream$ArrayEncoder.writeUInt32NoTag:1358` | 44 | 5.56% |
| `ConcurrentHashMap.get:958` | 24 | 3.03% |
| generated `LambdaForm` invoke | 18 | 2.28% |
| foreign downcall stub invoke | 17 | 2.15% |
| `CompletionOwner.submitPartsAttemptLocked:353` | 7 | 0.88% |

raw 표본은 `bench-phase-active` thread가 783개였다. framework처럼 표본이 많은
virtual-thread scheduler/stream/foreign-memory 경로로 퍼지지 않고, raw payload 생성과
submit frame에 집중됐다.

## 3. 할당이 어디서 나나

다음 표는 ObjectAllocationSample leaf allocation site의 `weight` 합계다. `weight`는
실제 heap accounting이 아니라 JFR 표본으로 추정한 값이다.

### `zlink-framework-java` (총 추정 16.86 GB)

| object class와 leaf allocation site | 추정량 | 총량 비중 |
|---|---:|---:|
| `ThreadLocalMap` @ `ThreadLocal.createMap:299` | 3.018 GB | 17.90% |
| `ForkJoinTask$AdaptedRunnableAction` @ `ForkJoinTask.adapt:1423` | 2.883 GB | 17.10% |
| `byte[]` @ `Arrays.copyOf:3535` | 1.467 GB | 8.70% |
| `ReferencePipeline$2` @ `ReferencePipeline.filter:184` | 1.287 GB | 7.63% |
| `ImmutableCollections$MapN$MapNIterator` @ `MapN$1.iterator:1562` | 1.116 GB | 6.62% |
| `DirectByteBufferR` @ `DirectByteBufferR.duplicate:282` | 978 MB | 5.80% |
| `byte[]` @ `AbstractMessageLite.toByteArray:47` | 747 MB | 4.43% |
| `byte[]` @ `BenchMetricHeader.createPayload:45` | 631 MB | 3.74% |

JFR stack에 application frame이 있는 표본만 다시 보면 다음 위치가 확인된다. 같은
할당 표본은 stack의 여러 frame에 나타나므로 이 행들은 서로 더하면 안 된다.

| stack에 있는 framework frame | 연결된 추정 weight | 근거 allocation 표본 |
|---|---:|---|
| `ZLinkStateLane.callWithCurrent:197` | 3.018 GB | `ThreadLocalMap` 39개 |
| `RouteSendCall.submitNode:191`, `ZLinkJavaRawSpotNode.classifyNodeSendTarget:299` | 1.287 GB | `ReferencePipeline$2` 105개 |
| `ZLinkProtobufMessageSerializer.serialize:25` | 1.461 GB | serializer stack 506개 |
| `ZLinkChannelEnvelope.encode:243` / `encodeHeader:226` | 1.150 GB | header-encode stack 43개 |
| `ZLinkServiceM6AWireCodec.writeMultipart:426` | 1.022 GB | `DirectByteBufferR` 76개 |

### `zlink-java` raw (총 추정 31.40 GB)

| object class와 leaf allocation site | 추정량 | 총량 비중 |
|---|---:|---:|
| `byte[]` @ `ByteString.copyFrom:460` | 10.852 GB | 34.56% |
| `byte[]` @ `BenchMetricHeader.createPayload:45` | 10.660 GB | 33.95% |
| `byte[]` @ `AbstractMessageLite.toByteArray:47` | 9.493 GB | 30.23% |
| `CompletionOwner$SendSubmissionValue` @ `CompletionOwner.submitSend:119` | 176 MB | 0.56% |
| `CompletableFuture` @ `newIncompleteFuture:2688` | 117 MB | 0.37% |

raw의 절대 추정량이 더 큰 것은 이 녹화에서 처리한 메시지 수가 훨씬 많기 때문이다.
12초 구간을 active throughput으로 정규화하면 framework는 약 725,024 message와
22.71 KiB/message, raw는 약 8,857,298 message와 3.46 KiB/message였다. 이는
JFR 추정 allocation weight의 정규화이며 throughput 판정 수치가 아니다. 이 표본의
framework/raw message당 추정 할당량 비는 6.56배다.

## 4. raw와 무엇이 다른가

raw는 ExecutionSample의 77.24%가 `Message.from:285` 또는 bench raw submit의
`RawStack$2.submitRaw:82` leaf에 있었다. framework의 최대 leaf는
`MappedByteBuffer.limit:339` 9.06%였고, 하나의 leaf로 집중되지 않았다.

framework에서 raw 표본에는 나타나지 않은, 큰 allocation stack은 다음 세 종류다.

| framework-only 관측 | JFR 근거 | raw와의 차이 |
|---|---|---|
| state-lane current context | `ThreadLocalMap` 3.018 GB, `ZLinkStateLane.callWithCurrent:197` | raw 상위 allocation 표에 없음 |
| node peer 분류 | `ReferencePipeline$2` 1.287 GB; `submitNode:191` → `classifyNodeSendTarget:299` stack | raw는 peer 분류 stream 표본이 없음 |
| framework envelope와 framework multipart | header stack 1.150 GB, `DirectByteBufferR` multipart stack 1.022 GB | raw 상위 allocation 표에는 이 framework frame이 없음 |

protobuf payload와 benchmark payload byte array는 두 구현에 모두 있다. raw에서는 이 세
`byte[]` 항목이 총 추정량의 98.74%이지만, framework에서는 위의 state-lane, peer 분류,
envelope/multipart 표본과 함께 분산된다. 이 JFR은 8배 throughput 차이를 단일 frame에
귀속하지 않는다. 표본이 보여 주는 차이는 framework send 경로에 raw에 없는 추가 allocation
및 scheduler/stream frame이 존재한다는 점이다.

## 5. 고칠 후보

아래는 §2--§4의 JFR 표본에 application `파일:줄`이 직접 있는 위치만 적었다. 구현은 하지
않았다. weight는 §3의 해당 stack과 연결된 추정 allocation weight다.

| 후보 | 파일:줄 | 표본 근거 |
|---|---|---|
| state lane의 current-context 경로 | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/execution/ZLinkStateLane.java:197` | `ThreadLocalMap` 3.018 GB (17.90%), 39 allocation samples; leaf ExecutionSample에도 `ThreadLocalMap` 7개 (2.54%) |
| 매 send의 node peer 분류 | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelRouteCalls.java:191`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawSpotNode.java:299` | `ReferencePipeline$2` 1.287 GB (7.63%), 105 allocation samples; stream leaf ExecutionSample 7개 (2.54%) |
| protobuf serializer에서 encoded payload를 만드는 경로 | `framework/languages/java/zlink-framework-codec-protobuf/src/main/java/systems/zlink/framework/codecs/protobuf/ZLinkProtobufMessageSerializer.java:25` | serializer stack 1.461 GB, 506 allocation samples; `AbstractMessageLite.toByteArray` leaf 747 MB (4.43%) |
| channel envelope header encode | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkChannelEnvelope.java:226`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkChannelEnvelope.java:704` | header stack 1.150 GB, 43 allocation samples; `HeaderWriter.encode:694` ExecutionSample 9개 (3.26%) |
| framework multipart를 native message로 쓰는 경로 | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6AWireCodec.java:426` | `DirectByteBufferR` stack 1.022 GB, 76 allocation samples; `writeFrameworkMultipartFrame:363` ExecutionSample 5개 (1.81%) |

## 6. 확인 못 한 것

- `ForkJoinTask$AdaptedRunnableAction` 2.883 GB (17.10%)는 virtual-thread timeout
  expiration의 JDK stack까지만 있고 application `파일:줄`이 표본에 없었다. 후보로
  귀속하지 않았다.
- source A만 녹화했다. target B, native library 내부, kernel/network 시간은 이 JFR에 없다.
- ObjectAllocationSample은 통계적 추정이고 source HTTP control request/JFR attach의 작은
  구간도 포함한다. 따라서 GB 값을 정확한 per-message allocation accounting으로 읽을 수 없다.
- framework ExecutionSample은 276개로 분산돼 있다. 이 표본만으로 scheduler frame, stream
  frame, foreign-memory frame 중 어느 하나가 throughput 8배 차이의 단독 원인이라고 말할 수 없다.
