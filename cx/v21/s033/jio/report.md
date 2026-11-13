| 역할과 I/O thread | Framework CPU µs/요청 | binding paced CPU µs/요청 | 차이 µs | TCP send 차이 µs | TCP receive 차이 µs | epoll 차이 µs | eventfd/wake 차이 µs | Core userspace 차이 µs | 기타 syscall 차이 µs |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| target `ZLINKbg/IO/1` | 14.232 | 7.187 | +7.045 | +6.093 | 0.000 | +0.276 | 0.000 | +0.781 | -0.106 |
| source `ZLINKbg/IO/2` | 13.776 | 6.063 | +7.713 | 0.000 | +4.780 | +0.546 | -0.050 | +1.825 | +0.611 |

이번 Framework 무제한 run은 51,911 req/s, binding paced run은 51,912.7 req/s였다. 두 run의 처리율 차이는 0.003%다. 이전 보고서의 +5.75 µs와 +6.6 µs는 각각 +7.045 µs와 +7.713 µs로 재현되지는 않았다. 이번 perf 표본의 stage 분해에서는 target 증가분의 대부분이 TCP send 6.093 µs이고, source 증가분은 TCP receive 4.780 µs와 Core userspace 1.825 µs가 크다. 아래 표의 perf CPU는 active 중 10초 schedstat delta를 완료 요청 수로 나눈 값이고, CPU stage는 같은 thread perf stack 표본 비율로 배분했다.

| 실행 | 역할과 I/O thread | CPU µs/요청 | sendto 호출/요청; 메시지/호출; 평균 B/호출; EAGAIN/요청 | recvfrom 호출/요청; 메시지/호출; 평균 B/호출; EAGAIN/요청 | epoll wait/요청; 준비된 반환/요청; 빈 반환/요청 | eventfd read/write/요청 | 자발적/비자발적 context switch/요청 |
|---|---|---:|---|---|---|---|---|
| Framework | source `IO/0` | 13.583 | 0.532; 1.860; 2,670; 0.000 | — | 0.630; 0.097; 0.533 | 0.000/0.000 | 0.285/0.005 |
| Framework | source `IO/2` | 13.776 | — | 0.759; 1.799; 2,595; 0.208 | 0.629; 0.365; 0.265 | 0.000/0.035 | 0.392/0.010 |
| Framework | target `IO/0` | 10.377 | — | 0.746; 1.967; 2,823; 0.242 | 0.504; 0.295; 0.209 | 0.000/0.198 | 0.412/0.003 |
| Framework | target `IO/1` | 14.232 | 0.619; 1.601; 2,311; 0.000 | — | 0.744; 0.124; 0.620 | 0.000/0.000 | 0.288/0.003 |
| binding paced | source `IO/0` | 7.799 | 0.952; 1.034; 1,294; 0.000 | — | 1.286; 0.332; 0.955 | 0.000/0.000 | 0.284/0.001 |
| binding paced | source `IO/2` | 6.063 | — | 1.187; 1.197; 1,499; 0.364 | 0.823; 0.433; 0.390 | 0.000/0.501 | 0.383/0.000 |
| binding paced | target `IO/0` | 8.284 | — | 1.221; 1.081; 1,354; 0.311 | 1.031; 0.552; 0.479 | 0.000/0.000 | 0.601/0.000 |
| binding paced | target `IO/1` | 7.187 | 0.836; 1.177; 1,473; 0.000 | — | 1.094; 0.253; 0.842 | 0.000/0.000 | 0.348/0.001 |

나머지 I/O thread의 CPU는 각 실행에서 0.004 µs/요청 미만이었다. TCP 메시지/호출은 BPF 반환 byte 합을 해당 방향에서 strace로 decode한 record 크기로 나눠 추정했고, 평균 byte/호출은 성공 호출 기준이다. eventfd read는 두 실행 모두 관찰되지 않았다. EAGAIN은 TCP recvfrom에서 Framework source `IO/2` 0.208, target `IO/0` 0.242, binding source `IO/2` 0.364, target `IO/0` 0.311회/요청이었으며 sendto에서는 관찰되지 않았다. Framework는 sendto/recvfrom 호출과 epoll wait가 binding보다 적고 호출당 byte가 더 컸다. 따라서 호출 수, EAGAIN, eventfd, context switch 증가가 CPU 차이를 설명하지 않는다. TCP send/receive stack의 CPU와 Core userspace가 차이의 대부분을 차지한다.

perf의 process별 hot I/O thread 합산 CPU는 Framework source 27.366, target 24.616 µs/요청이고 binding source 13.868, target 15.477 µs/요청이었다. perf window 처리율은 각각 52,762와 51,911 req/s였다. BPF 8초 window는 Framework 54,311 req/s, binding 47,555 req/s였다. 따라서 syscall·eventfd별 per-request 값은 BPF window에서 수집한 값이며, binding 계측 구간은 pacing 목표보다 8.4% 낮아 tracing perturbation이 있다.

## Wire record

Framework 요청 record는 `[13 B service header, 1,398 B application frame]`이고 ZMP framing 24 B를 더해 1,435 B였다. Framework reply는 `[21 B reply header, 1,398 B application frame]`으로 전체 1,443 B였다. binding 요청과 reply record는 각각 `[201 B metadata JSON, 1,027 B application part]`이고 ZMP framing 24 B를 더해 각각 1,252 B였다. 따라서 literal TCP record 차이는 요청 +183 B, reply +191 B였다. 두 경로의 ZMP framing은 요청·reply 각각 24 B로 같았다.

Framework request service header 13 B는 service prefix 5 B와 `correlation` U64 8 B다. reply header 21 B는 prefix 5 B, `correlation` U64 8 B, `terminalResult` U32 4 B, `failureCode` U32 4 B다.

Framework application frame의 75 B wrapper는 `version` U8 1 B, `bodyLength` U32 4 B, text8 `packetName=ZLinkFrameworkMultipart` 24 B, text8 `contentType=application/x-zlink-multipart` 30 B, `payloadLength` U32 4 B, inner part count U32 4 B, 두 part length U32 각 4 B다. inner parts는 Framework JSON header 296 B와 binding과 같은 1,027 B application part였다.

binding의 201 B metadata JSON에는 `kind`, `channelName=bench`, `messageName=BenchPayload`, `contentType=application/x-protobuf`, `correlationId=null`, `deadline=null`, `topic=null`, `errorCode=null`, `errorMessage=null`, `source=null`이 있었다. Framework inner JSON header는 같은 공통 필드에 `formatMarker=242` 19 B, `flowId=null` 14 B, `flowOrigin=null` 18 B, `metadata={}` 14 B가 추가됐고, `correlationId` 값은 32자리 hex 문자열이었다. 그 JSON header는 binding metadata보다 실제로 95 B 컸다. 기존 175 B 정규화 차이는 service header 13 B, wrapper 75 B, 추가 JSON header 87 B의 합이다. 추가 JSON 95 B 중 8 B correlation slot은 binding ZMP request sequence에 대응시켜 한 번만 셌다. literal raw record 비교에는 JSON correlation 표현까지 포함되므로 요청 +183 B가 측정됐다. reply에는 8 B terminal/failure 필드가 더 있어 literal 차이가 +191 B였다.

## Mesh thread ②

Framework source JFR은 `settings=profile`로 active 60초 동안 기록했다. `zlink-jvm-raw-mesh-bench`에서 481개의 execution sample을 얻었다. 각 sample은 아래 다섯 범주 중 stack에서 가장 가까운 해당 frame 하나에만 배정했다. CPU 값은 이 sample 비율을 이전 보고서의 11.92 µs/요청에 적용한 환산이다. 이번 run의 perf 10초 schedstat에서는 같은 mesh thread가 12.762 µs/요청이었고, BPF 8초 계측 구간에서는 14.212 µs/요청이었다.

- `ZLinkServiceOperationRegistry.take`: 48/481 sample, 10.0%, 11.92 µs 기준 1.190 µs다. binding은 같은 요청의 완료를 `NativeDealerSocket.request()`와 `CompletionOwner.capture()`의 `pending.get(token)`으로 찾는다. 키는 Framework service correlation이 아니라 binding completion token이다.
- `ZLinkJavaRawMeshNode.decodeRequestReply`: 44/481 sample, 9.1%, 1.090 µs다. binding `CompletionOwner.readResult()`는 native request 결과와 reply `Message[]`를 전달하지만 Framework의 21 B service reply header, correlation, terminal 필드를 해석하지 않는다.
- `ZLinkServiceM6AWireCodec.decodeMultipartView`: 18/481 sample, 3.7%, 0.446 µs다. binding request completion은 transport multipart 결과를 `Message[]`로 받지만 `ZLinkFrameworkMultipart`의 version, 길이, part count와 inner part 길이는 binding API에서 해석하지 않는다.
- completion handoff: 89/481 sample, 18.5%, 2.206 µs다. binding도 `CompletionOwner.capture()`에서 pending state를 완료하고 `completePublicSettlements()`와 `Pending.completeFuture()`로 `RequestOperation` completion을 전달한다.
- other: 282/481 sample, 58.6%, 6.988 µs다. `Message.closeAll`, FFM downcall, `ConcurrentHashMap` 작업, `CompletableFuture` 처리 등이 포함되며, binding 대응은 `CompletionOwner.readResult()`가 native reply parts를 `Message[]`로 만드는 public `NativeDealerSocket.request()` 경로다.

## 실행과 변경

각 조건은 `/root/jobs/fx/cx/measure.lock` 배타 lock 안에서 condition B, request-backpressure, 1,024 B, RUNS=1, warmup 30초, active 60초로 실행했다. Framework는 3,114,820건 완료, binding은 3,114,767건 완료했고 errors와 server errors는 모두 0이었다. Framework 처리율 51,911 req/s를 `ZLINK_JAVA_BENCH_REQUEST_RATE`에 넣어 binding을 pacing했다. BPF와 perf의 counter exit status는 수집기가 허용하는 종료 코드였고 BPF 및 uprobe return code는 0이었다. 측정 중 MemAvailable은 12 GiB 아래로 내려가지 않았다.

`/root/jobs/fx/cx/measure-shared.sh taskset -c 0-7 bash java/build.sh`를 `framework/bench/grpc`에서 실행해 성공했다. 두 bench 파일에 `ZLINK_JAVA_BENCH_REQUEST_RATE` 양의 정수 검증과 누적 목표시각 pacing을 추가했다. pacing은 `zlink-java`의 raw `request-backpressure`만 대상으로 한다. 별도 test는 실행하지 않았다.

직접 수정한 worktree 파일은 `framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchDrivers.java`와 `framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchOptions.java`다.
