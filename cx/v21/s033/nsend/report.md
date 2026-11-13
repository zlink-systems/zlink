현재 main의 RUNS=3 중앙값에서 send-saturation 4KiB는 F÷B 0.6370이다. 이전 0.597은 이번 중앙값에서 재현되지 않았다. request-serial 1KiB만 0.6 미만이며 메시지당 over budget by 0.973 µs다. 이 셀은 ±0.02 경계에도 속한다. request-serial 4KiB는 0.6248로 경계 상한 0.62보다 조금 높다.

공개 binding Message.copy/data/close를 사용해 수신 managed copy를 없애는 최소 후보도 시험했다. RUNS=5에서 send 4KiB는 F÷B 0.4505, over budget by 5.972 µs/msg로 회귀해 기각했다. 실험 runtime·test·bench 변경을 모두 원복하고 diff와 측정 자료만 보존했다.

| 셀 | F msg/s 중앙값 [최소–최대] | B msg/s 중앙값 [최소–최대] | F÷B | B µs/msg | F 예산 µs/msg | F µs/msg | 예산 초과 또는 여유 | 판정 |
|---|---:|---:|---:|---:|---:|---:|---|---|
| send-saturation 1024 | 78,564.9 [76,768.1–79,025.8] | 120,804.2 [116,506.6–122,458.0] | 0.6503 | 8.278 | 13.796 | 12.728 | 여유 1.068 µs/msg | 0.62 초과 |
| send-saturation 4096 | 59,038.3 [56,231.9–59,178.9] | 92,677.4 [91,019.6–93,427.6] | 0.6370 | 10.790 | 17.984 | 16.938 | 여유 1.045 µs/msg | 0.62 초과 |
| request-backpressure 1024 | 18,720.0 [18,483.2–18,886.4] | 28,134.8 [28,117.5–28,218.2] | 0.6654 | 35.543 | 59.239 | 53.419 | 여유 5.820 µs/msg | 0.62 초과 |
| request-backpressure 4096 | 19,489.0 [19,220.4–19,924.4] | 25,257.8 [25,158.8–25,476.2] | 0.7716 | 39.592 | 65.986 | 51.311 | 여유 14.675 µs/msg | 0.62 초과 |
| request-serial 1024 | 5,727.6 [5,560.5–5,789.9] | 9,599.6 [8,982.7–9,748.9] | 0.5967 | 104.171 | 173.618 | 174.592 | over budget by 0.973 µs/msg | 0.6 미달, ±0.02 경계 |
| request-serial 4096 | 5,336.1 [5,315.9–5,489.9] | 8,540.5 [8,481.6–8,606.1] | 0.6248 | 117.089 | 195.149 | 187.405 | 여유 7.744 µs/msg | 0.62 초과 |

표의 F÷B는 F 처리량 중앙값을 B 처리량 중앙값으로 나눈 값이다. 각각의 최소–최대 범위는 유효한 세 회차 전체다. 예산은 `(1,000,000 / B 중앙값) / 0.6`, F 시간은 `1,000,000 / F 중앙값`이다. 이 시간은 처리량의 역수이며 CPU 시간과 다르다. 경계는 [0.58, 0.62]로 판정한다. serial 1KiB의 작은 초과를 안정적인 큰 결함으로 확대 해석하지 않는다. 원본과 교체 근거는 baseline-summary.json과 baseline-replacement.json에 있다.

회차별 비율의 중앙값도 함께 제시한다. request-serial 1KiB는 회차별 비율 중앙값 0.6031과 중앙값 처리량끼리의 비율 0.5967이 기준의 양쪽에 있다. 예산 초과 0.973 µs/msg는 표에 명시한 처리량 중앙값 기준 판정이며, 세 회차 모두 0.6 미만이었다는 뜻은 아니다.

| 셀 | 회차별 F÷B | 회차별 비율 중앙값 [최소–최대] |
|---|---|---|
| send-saturation 1024 | 0.6542, 0.6743, 0.6269 | 0.6542 [0.6269–0.6743] |
| send-saturation 4096 | 0.6370, 0.6019, 0.6502 | 0.6370 [0.6019–0.6502] |
| request-backpressure 1024 | 0.6550, 0.6713, 0.6658 | 0.6658 [0.6550–0.6713] |
| request-backpressure 4096 | 0.7716, 0.7821, 0.7640 | 0.7716 [0.7640–0.7821] |
| request-serial 1024 | 0.6031, 0.5704, 0.6376 | 0.6031 [0.5704–0.6376] |
| request-serial 4096 | 0.6200, 0.6268, 0.6428 | 0.6268 [0.6200–0.6428] |

2026-11-08 KST에 수행한 Node 측정은 main `d961e51e231234e8404d74164d2a27c0cf2fce41`의 소스를 사용한다. 기존 네 언어 CPU pin 패치는 보존한다. Framework와 binding은 같은 Node 22.23.3과 `@zlink-systems/zlink` 1.23.0 package를 사용한다. package-lock이 같은 사본의 의존성을 복사하고 현재 소스를 TypeScript로 빌드했다. 설치된 Core runtime은 package provenance의 `core/v1.23.0`, revision `79567d379162e67476ba13e18cfe0f69925e63c9`이며 두 구현이 같은 파일을 적재한다. libzlink.so.0 SHA-256은 `6f496304ef6e1287f57234567231bbd2b76ad2392f99e816001417d01215bf01`이다. Core를 별도로 재빌드한 수치는 아니다. Protobuf serializer의 4096-byte 왕복 검사도 통과했다.

source는 CPU 0–7, target은 CPU 8–15에 고정한다. 이 머신에서 각 그룹은 SMT를 포함한 logical CPU 8개, physical core 4개다. 각 셀은 별도 프로세스로 시작한다. 처리량 표의 warmup은 10초, active는 20초다. 처리량은 기존 bench-core.js:251–257 정의를 따른다. send는 active 종료 경계에서 조회한 target.received, request는 source.completedAtClose를 20초로 나눈다. drain 뒤 최종 completed는 유실 검사에 쓰고 처리량 분자로 바꾸지 않는다. CPU 표본은 지연 표본 배열이 채워지는 초기 구간을 피하도록 active 시작 5초 뒤부터 수집한다. 비계측 CPU 창은 약 14초, Inspector와 perf의 표본 창은 약 8초다. 정정 진단과 프로토타입의 추가 CPU·진단 실행은 active 30초를 사용하고 target 수신 205000건 뒤 표본을 시작한다. 처리량이 낮아도 200000건 latency 표본 배열 채우기 비용이 CPU 분해에 섞이지 않도록 한다. 처리량 RUNS=5의 20초 조건은 바꾸지 않는다.

모든 측정은 `flock -o /root/jobs/fx/cx/measure.lock` 안에서 실행한다. 빌드와 검사는 `measure-shared.sh`를 사용한다. 셸의 background job과 강제 종료는 사용하지 않는다. Python 실행기가 자식 프로세스를 소유하고 SIGTERM 또는 SIGINT로 종료·회수한다. 각 셀의 affinity와 실제로 적재한 native library 경로는 `commands.json`에 남긴다.

첫 baseline의 3회차 후반에는 잠금 없이 실행된 외부 Java·javac가 여러 CPU를 사용했다. 09:07:37 KST에 Java 약 4.19 CPU, javac 약 4.56 CPU가 관측됐다. 이 구간에서 request-serial의 두 구현 처리량이 함께 크게 내려갔다. 3회차 전체와 그 뒤 CPU 캡처는 제외 기록으로 보존한다. 처리량을 보고 특정 셀만 고르지 않고 3회차 전체를 교체한다. `contamination.json`, `baseline/excluded/run3-unlocked-build/`, `excluded-cpu-unlocked-build/`가 원본이다. 재개 실행기는 외부 userspace 프로세스가 1초간 0.5 CPU를 넘거나 두 번 연속 0.25 CPU를 넘으면 자기 측정을 중단한다. 12 GiB memory 조건도 감시한다.

send-saturation 4KiB의 호출 경로는 다음과 같다. 표에서 F는 `framework/languages/node/packages/framework/src`, B는 `bindings/node/src`, N은 `framework/bench/grpc/node`를 뜻한다. Framework의 application worker는 async task이며 별도 OS thread가 아니다.

| 단계 | 위치 | thread와 turn |
|---|---|---|
| payload 생성과 logical send stream | N/client/bench-core.js:386 `sendWorkers` | source JS main. 각 stream은 `setImmediate` 뒤 다음 메시지를 시작한다. |
| public node-direct send | N/client/framework-transport.js:61; F/runtime/channels/channel-clients.ts:202, :498 | 같은 source JS main. `sendToNode(...).submit()`까지 동기 호출한다. |
| 대상·codec 선택과 channel envelope | F/runtime/channels/channel-transports.ts:372; F/runtime/channels/channel-envelope.ts:156, :477; N/shared/framework-protobuf.js:19 | 같은 source JS main. schema-backed Protobuf payload와 channel header를 만든다. |
| canonical multipart application frame | F/runtime/backend/node/node-raw-mesh-backend.ts:740, :2950; F/runtime/foundation/service-wire-m6a-codec.ts:212, :256 | 같은 source JS main. header·protobuf를 최종 M6A frame에 쓴다. |
| 논리 peer admission과 제출 | F/runtime/foundation/raw-service-mesh-runtime.ts:641, :2124; F/runtime/backend/node/node-raw-binding-port.ts:329 | 같은 source JS main. logical Ready peer를 확인하고 binding send를 호출한다. |
| binding send | F/runtime/backend/node/node-backend-adapter-support.ts:133; B/zlink/runtime/sockets/router_socket.ts:61; B/zlink/runtime/messaging/completion_owner.ts:274 | 같은 source JS main/N-API. 정상 수락은 즉시 결과를 돌려준다. backpressure의 보존·재제출·completion은 binding이 소유한다. |
| native frame과 TCP 진행 | bindings/node/native/src/addon_core.cc:901, :919 | submit 진입은 source JS main이다. TCP 진행은 `ZLINKbg/IO/*`다. |
| libuv readiness와 수신 | F/runtime/backend/node/node-event-loop-poller.ts:64; F/runtime/backend/node/node-raw-mesh-backend.ts:200, :1764; F/runtime/foundation/raw-service-mesh-runtime.ts:1021, :1075 | target JS main의 libuv callback. binding Poller와 raw receive pump를 같은 thread에서 실행한다. |
| binding receive와 payload 보존 | B/zlink/runtime/sockets/socket_operations.ts:223; F/runtime/backend/node/node-raw-binding-port.ts:394, :535, :577 | 같은 target JS main/N-API. reusable `Received`에서 routing 정보와 payload를 보존한다. |
| M6A admission과 mailbox | F/runtime/foundation/raw-service-mesh-runtime.ts:1224, :1414, :1500; F/runtime/foundation/service-mailbox.ts:103 | 같은 target JS main. peer·command를 검증하고 기존 application permit와 mailbox에 전달한다. |
| application worker와 claim | F/runtime/backend/mesh-dispatch-pump.ts:161, :400, :526; F/runtime/backend/node/node-raw-mesh-backend.ts:2495, :2577 | 첫 handler는 receive stack을 벗어난 Promise continuation에서 시작한다. 같은 JS main이며 OS thread handoff가 아니다. 배치 fairness 경계는 기존 macrotask yield를 사용한다. |
| node route dispatch와 handler admission | F/runtime/spots/spot-node-runtime-manager.ts:964; F/runtime/host/index.ts:3145, :3194; F/runtime/channels/channel-dispatchers.ts:774, :888 | 같은 target JS main/application turn. 기존 host admission을 거쳐 node send handler를 선택한다. |
| typed decode와 handler | F/runtime/channels/channel-dispatch-pipeline.ts:100, :349; F/runtime/channels/channel-envelope.ts:413; framework/languages/node/packages/framework-codec-protobuf/src/framework.ts:97; N/zlink-framework-server/main.js:44 | 같은 target JS main. handler가 수신 건수와 metric header를 기록한다. async handler의 terminal과 record release는 후속 microtask에서 처리한다. |

binding 비교 source는 ROUTER 8개를 사용한다. Framework source는 RouteMesh 하나에 logical stream 8개를 제출한다. binding target은 두 socket을 `setImmediate` pump에서 검사하며 마지막 payload part만 읽는다. Framework는 service header와 application frame을 모두 읽는다. 이 차이는 실험의 실제 경로이며, 두 프로세스의 총 CPU 차이를 모두 Framework bookkeeping으로 해석하지 않는다.

구조 후보를 적용하기 전의 소유 결정은 다음과 같다. 변경 분류는 A, 기존 binding ownership 계약을 사용하는 내부 적응이다. wire, 결과 목록, deadline, handler turn과 isolation 계약을 바꾸지 않는다.

| 결정 | 규범·소유 계층 | 현재 구현과 대조 | 후보 뒤 소유자 |
|---|---|---|---|
| 수신 bytes를 다음 receive·close 뒤에도 보존 | binding `Message.copy/data/close`; bindings/doc/spec/node/README.ko.md:660, :690; Framework 00-foundation/05-message-model.ko.md §8 | Node `NodeRawReceivedRecord`가 각 part의 `data()`를 읽어 managed copy를 보존한다. binding contracts/messaging/message.ts:142는 native payload를 reference count로 공유하는 copy를 제공한다. | binding이 native ref와 Buffer view 수명을 소유한다. Framework는 기존 Buffer 배열만 보관한다. |
| binding receive 저장 표현 선택 | binding message_materializer.ts:92 routedReceivedPrefersManagedBuffer | baseline은 원래 Received의 모든 part에서 data를 읽어 managed 경로가 된다. 후보는 public copy의 data만 읽어 원래 Received가 native 경로에 남는다. | 같은 binding 함수가 선택한다. Framework에 role flag나 두 번째 선택기를 만들지 않는다. |
| canonical wire 해석 | Framework wire spec과 `service-wire-v1.schema.json` | Node service-wire-m6a-codec.ts:350; C++ service_wire_codec.cpp:3383; .NET ZLinkApplicationPayloadEnvelopeCodec.cs:211; Java/Kotlin 공용 ZLinkServiceM6AWireCodec.java:499 | 같은 Framework codec이다. 바이트와 검증은 그대로다. |
| decoded byte 결과를 encoded payload와 분리 | Framework serializer 계약과 기존 isolation test | framework-codec-protobuf/src/framework.ts:97; test/contract/stream-connector-codecs.test.js:252 | 같은 serializer다. `ZLinkEncodedPayload.data()`의 독립 복사는 유지한다. |
| Core 수신 HWM charge와 payload 보유 수명의 분리 | bindings/doc/spec/README.ko.md:1144 Receive ownership | HWM charge는 Core dequeue에서 끝난다. 결과 payload를 더 오래 보유해도 별도 charge·capacity·allowance 상태를 만들지 않는다. | 같은 Core·binding이다. 참조 공유는 payload 수명에만 적용한다. |
| 실제 send 수락·backpressure·재제출 | binding | CompletionOwner.submitSend:274; Framework raw runtime은 logical peer admission과 결과 투영을 담당한다. | 같은 binding이다. 변경하지 않는다. |
| handler admission·실행·record release | Framework handler-turn/execution-gate spec §1–2와 message model §8 | mesh-dispatch-pump.ts:400, :526; host/index.ts:3194 | 같은 host와 pump다. queue·gate·yield를 추가하거나 우회하지 않는다. |

교차언어 대조 위치는 `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:3383`, `framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkApplicationPayloadEnvelopeCodec.cs:211`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6AWireCodec.java:499`다. Kotlin은 이 Java 공용 runtime을 사용한다. C++은 retained native message를 바탕으로 parts를 읽는다. .NET은 `TryDecodeFrameworkMultipartView(Message frame, ...)`에서 native span과 owner를 보존한다. Java/Kotlin 공용 구현은 `decodeFrameworkMultipartFrameView(Message frame)`이 frame 소유권을 받아 view를 만든다. Node의 후보도 공개 binding ref-count copy와 Buffer view를 사용한다. 사용자 동작의 언어별 예외를 만들지 않는다.

send-saturation 4KiB의 비계측 창에서 Framework source main은 중앙값 16.336 µs/msg, target main은 16.579 µs/msg다. source main 점유는 0.930–0.982 CPU, target은 0.974–0.997 CPU였다. binding은 source main 10.532 µs/msg, target main 10.641 µs/msg다. target event loop가 우선 제한하고 source도 거의 한 CPU를 사용한다. target만 빨라지면 source가 다음 제한점이 된다. Core I/O thread는 별도다. 이 포화 속도 표를 같은 속도의 총 CPU 비교로 사용하지 않는다.

Inspector CPU 표본의 exclusive self 시간을 같은 캡처의 /proc task schedstat CPU에 비례시킨 분해는 다음과 같다. 단위는 µs/msg다. 각 열은 합이 main CPU가 된다. Inspector가 이름 없는 native leaf로 기록한 시간은 stack의 가장 가까운 확인 가능한 호출자에 배정한다. 나머지 native 함수와 V8 builtin의 소유자를 확정하지 못한 부분은 기타에 남긴다. allocation 행은 allocator API 내부 시간이며 그 안의 복사가 완전히 분리된 값은 아니다. 별도 perf stack으로 실제 memcpy 성분을 보완한다. 이 값은 단계별 stopwatch도, Framework에서 binding 값을 뺀 순수 추가 비용도 아니다. 별도 캡처에서 계측하므로 위 비계측 시간과 조금 다르다.

| CPU 구분 | F source | F target | B source | B target |
|---|---:|---:|---:|---:|
| Framework bookkeeping | 1.601 | 3.329 | 0.000 | 0.000 |
| encode/serialize | 1.416 | 1.845 | 0.117 | 0.296 |
| Buffer copy API | 0.797 | 1.791 | 0.825 | 2.266 |
| allocation API | 2.127 | 3.173 | 1.757 | 0.011 |
| binding call | 5.612 | 2.812 | 4.583 | 5.921 |
| Promise/microtask | 1.573 | 1.515 | 0.216 | 0.012 |
| handoff/event loop | 0.254 | 0.012 | 0.227 | 0.108 |
| GC | 0.194 | 0.583 | 0.924 | 1.505 |
| benchmark | 1.306 | 0.189 | 0.763 | 0.122 |
| other | 2.088 | 2.149 | 1.650 | 0.995 |
| 합계 | 16.968 | 17.398 | 11.062 | 11.237 |

Inspector의 API 경계 표와 별도로 perf의 memcpy·allocator leaf를 분리했다. GC stack 아래 allocator는 GC에만 배정한다. 이 표는 다른 캡처에서 얻은 표본이며 Inspector 행에 더하지 않는다. native receive 안의 memcpy는 Inspector에서는 recv 또는 allocator API 내부 시간에 포함되지만 여기서는 copy다. 따라서 API 경계 표의 copy 행을 모든 실제 byte 복사 CPU의 합으로 읽으면 안 된다.

| perf leaf 분류 µs/msg | F source | F target | B source | B target |
|---|---:|---:|---:|---:|
| copy | 2.018 | 1.758 | 1.249 | 0.595 |
| allocation | 1.522 | 1.038 | 1.351 | 0.735 |
| GC | 0.224 | 0.543 | 0.720 | 0.955 |

perf cpu-clock 199Hz native stack은 F source에서 protobuf bytes 복사 0.779 µs/msg, M6A 소유 복사 0.496 µs/msg, binding send 복사 0.602 µs/msg를 확인했다. F target은 receive managed 복사 1.192 µs/msg와 serializer isolation memcpy 0.507 µs/msg다. 후자는 typed-array 생성 전체 1.425 µs/msg의 일부다. 같은 표본의 값끼리 포함 관계가 있으므로 더하지 않는다. copy-stack-summary.json의 target receive 값은 1,464개 main 표본 중 101개다. 단일 8초 표본의 오차와 unwind가 끝난 stack을 감안해야 한다. perf 전체 분해와 미분류 leaf는 perf-summary.json에 보존했다.

별도 진단 실행의 API별 건수와 bytes는 다음과 같다. source 분모는 메시지마다 한 번 실행되는 benchmark yield의 global setImmediate 호출 수이며 target 분모는 성공한 Router.recv다. capture 시작·종료 경계에 걸친 logical stream 때문에 소수 메시지의 차이가 생길 수 있어 건수는 근삿값이다. source와 target은 약간 다른 8초 창이다. 계측 자체가 느리게 만들므로 이 실행의 처리량이나 CPU를 용량 판정에 쓰지 않는다. bytes는 JS API에 넘긴 길이이며 실제 allocator 예약 크기나 동시 live bytes가 아니다. GC duration은 PerformanceEntry의 관측 시간이며 CPU 시간과 동일시하지 않는다. Buffer view는 복사 bytes에 넣지 않는다.

| 메시지당 API 작업 | F source | F target | B source | B target |
|---|---:|---:|---:|---:|
| Buffer.allocUnsafe 요청 B | 12876.131 | 4398.988 | 8195.000 | 0.000 |
| Buffer.prototype.copy B | 0.000 | 0.000 | 0.000 | 0.000 |
| Uint8Array.set 복사 B | 8414.976 | 0.000 | 4096.000 | 0.000 |
| Buffer.from 복사 B | 20.122 | 20.000 | 24.000 | 0.000 |
| Uint8Array 생성 복사 B | 0.000 | 4099.444 | 0.000 | 0.000 |
| Buffer.from view B | 0.000 | 4099.455 | 0.000 | 0.000 |
| Message.data 노출 B | 0.000 | 4398.975 | 0.000 | 4099.000 |
| Promise 생성 수 | 5.140 | 9.103 | 2.000 | 0.000 |
| Promise continuation before 수 | 3.013 | 8.058 | 1.000 | 0.000 |
| global setImmediate 호출 수 | 1.000 | 0.000 | 1.000 | 0.907 |
| async_hooks Immediate 생성 수 | 1.000 | 0.016 | 1.000 | 0.907 |
| Router.recv 시도 수 | 0.125 | 1.003 | 0.000 | 2.813 |
| GC pause 관측 duration µs | 0.176 | 0.555 | 0.841 | 1.199 |

Framework source는 4096B benchmark payload를 4099B Protobuf bytes로 만들고, multipart 두 부분을 합쳐 4319B를 M6A frame에 복사한다. Buffer를 binding에 제출하면 addon_core.cc:919가 native frame으로 약 4399B를 다시 옮긴다. 마지막 값은 service header와 application frame의 합이다. Buffer.allocUnsafe 12876B에는 64KiB payload slab을 나눈 평균과 Protobuf allocator 요청 등이 들어간다. native frame 할당은 이 Buffer API 카운터에 포함되지 않는다. binding source는 같은 benchmark payload·Protobuf 복사를 수행하므로 공통 작업을 Framework 전용 비용으로 세지 않는다.

Framework target은 두 part의 약 4399B managed receive 저장소와 decoded 결과 격리를 위한 약 4099B Uint8Array를 각각 만든다. Buffer.from 약 20B는 routing·header 문자열 등이며 4099B view를 복사로 세지 않는다. binding target도 data()에서 4099B를 읽지만 managed 역할과 native 역할 선택이 다르다. 모든 part를 관측한 Received만 managed receive로 전환한다(message_materializer.ts:92). raw benchmark는 마지막 payload part만 읽으므로 Framework와 binding의 recv 함수 총 시간을 동일한 의미의 작업으로 빼지 않는다.

Promise before는 반응 실행 횟수다. OS thread handoff 횟수나 서로 다른 macrotask 횟수가 아니다. source의 Framework는 약 3.013회, binding은 1회다. Framework adapter의 async await와 benchmark await가 같은 admission 결과를 이어받는다. target의 Framework는 약 8.058회다. receive pump·mailbox claim·handler completion은 같은 main thread의 후속 microtask에서 진행한다. target의 Immediate 생성은 약 0.016회/msg이며 배치 경계에 해당한다. binding target의 busy pump는 별도로 표에 표시했다. 이러한 binding benchmark pump 비용도 비교 CPU에 포함한다.

V8 heap sampling 32768B, collected object 포함 설정에서 source는 F 약 3855.6B/msg, B 약 2532.2B/msg이고 target은 F 약 7896.2B/msg, B 약 1597.2B/msg였다. 이 값은 표본 추정 JS heap 할당량이다. Buffer backing store와 native zlink frame 전체를 포함하지 않으며 위 Buffer 요청 bytes와 합산하지 않는다. F target의 큰 할당 지점은 dispatchMesh 약 389.9B/msg, dispatchClaimRecords 386.9B/msg, decodeMultipartRecord 332.4B/msg, receiveOne 316.3B/msg다. 객체·Promise 할당을 포함하며 크기가 큰 payload 복사만으로 target 비용을 설명할 수 없다.

같은 rate의 총 CPU는 Framework source를 제한하지 않고, 각 회차 Framework의 비계측 CPU 창 target 수신 rate에 binding source만 맞춰 세 쌍을 수집했다. binding pacing은 기존 logical stream 8개를 유지하고 128-message 묶음의 시작을 timer로 늦춘다. 실제 target rate 차이는 각 쌍에서 +12.51, +5.36, −7.21 msg/s였다. source·target 모든 thread의 user·kernel tick을 합해 같은 창의 target 수신 수로 나눈 값이다. tick은 100Hz이며 main 열은 별도로 schedstat nanosecond를 사용한다. 단위는 CPU µs/msg다.

| 회차 | 구현 | target msg/s | source user | source kernel | target user | target kernel | 합계 µs/msg | source main | target main |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | F | 57,031.4 | 18.355 | 6.738 | 20.140 | 3.057 | 48.290 | 15.876 | 17.515 |
| 1 | B | 57,043.9 | 11.977 | 8.329 | 18.376 | 7.769 | 46.452 | 10.346 | 17.566 |
| 2 | F | 60,181.0 | 18.220 | 8.714 | 18.918 | 3.228 | 49.080 | 16.245 | 16.563 |
| 2 | B | 60,186.4 | 12.047 | 8.236 | 17.664 | 7.587 | 45.534 | 10.332 | 16.638 |
| 3 | F | 59,537.8 | 18.569 | 9.978 | 19.239 | 4.433 | 52.219 | 16.536 | 16.745 |
| 3 | B | 59,530.6 | 12.194 | 8.293 | 17.898 | 7.684 | 46.068 | 10.461 | 16.817 |

총 CPU 중앙값 [최소–최대]는 F 49.080 [48.290–52.219] µs/msg, B 46.068 [45.534–46.452] µs/msg다. 같은 회차 F−B 차이의 중앙값은 3.547 [1.839–6.151] µs/msg다. 서로 다른 중앙값을 뺀 값과 paired 차이의 중앙값을 구분한다. Raw binding target의 busy pump가 낮은 rate에서도 CPU를 사용하므로 B main은 약 16.638–17.566 µs/msg다. 포화 rate에서 측정한 B target 약 10.6 µs/msg를 이 rate의 값으로 대신 쓰지 않는다. Core I/O·GC worker와 kernel도 총 CPU에 포함한다. 여러 ROUTER 연결과 한 RouteMesh의 실제 실행 형태가 다르므로 이 총 차이를 모두 Framework bookkeeping으로 분류하지 않는다.

Framework-only 단계의 예산 판단은 다음과 같다. self는 exclusive다. inclusive라고 적힌 행은 중첩되므로 다른 행과 더하지 않는다. native receive 복사는 binding이 실행하지만 Framework의 저장소 선택 때문에 발생하는 추가 작업이므로 후보에 넣었다.

| 위치와 작업 | source µs/msg | target µs/msg | 예산 판단과 제거 범위 |
|---|---:|---:|---|
| channel-clients.ts:498 submit, channel-transports.ts:372 submitNode, node-raw-mesh-backend.ts:740 sendToNode의 self 합 | 0.608 | — | public admission과 logical peer 결정이다. binding backpressure 결정과 구분해 유지한다. |
| channel-envelope.ts:124 encodeChannelEnvelopeParts, :471 encodePayload의 self 합 | 0.244 | — | typed 계약을 보존한다. serializer를 우회하지 않는다. |
| service-wire-m6a-codec.ts:256 writeMultipartPayload | 0.491 | — | 새 M6A frame의 소유 복사다. native writable Message.allocate로 최종 frame에 직접 쓸 대안이 있다. 이번 target 후보에서는 유지한다. |
| node-backend-adapter-support.ts:133 submitBindingSend self | 0.153 | — | 실제 수락은 binding이 소유한다. 이 행을 없애도 native send 자체의 비용은 남는다. |
| service-mailbox.ts:103 tryEnqueue self | — | 0.169 | 기존 application admission과 fairness를 유지한다. 새 gate나 queue를 추가하지 않는다. |
| mesh-dispatch-pump.ts:568 terminal 관측과 :526 claim의 self 합 | — | 0.464 | terminal과 permit release를 유지한다. 독립 admission을 덧붙이지 않는다. |
| channel-envelope.ts:1038 canonical reader consume self | — | 0.307 | canonical header token 검사를 유지한다. |
| channel-envelope.ts:963 canonical channel header parse self | — | 0.198 | wire 검증을 유지한다. |
| service-mailbox.ts:113 timestamp와 channel-dispatchers.ts:818 fallback timestamp의 self 합 | — | 0.310 | 첫 UTC 수신 사실이 두 번 생성된다. 두 번째 clock의 self 0.146 µs/msg는 없어져야 한다. 이번 storage 프로토타입에서는 바꾸지 않았다. |
| node-raw-binding-port.ts:535 record와 binding recv의 native managed memcpy | — | 1.192 | 첫 whole-frame materialization 제거 후보다. Message.copy/data/close의 공개 ownership을 사용한다. 추가 handle·finalizer 비용을 실측해야 한다. |
| ZLinkEncodedPayload.ts:24 data 전체, protobuf serializer deserialize에서 호출 | — | 1.425 | isolation 계약 때문에 유지한다. view로 바꾸는 것은 허용하지 않는다. |
| 채널 decode→handler 전체 inclusive | — | 2.490 | decodeChannelPayload의 하위 비용을 포함한다. 위 data 비용과 더하지 않는다. |

현재 send 4KiB의 처리량 시간 16.938 µs/msg는 예산 17.984 µs/msg 안에 1.045 µs/msg 여유가 있다. 따라서 이 셀을 0.6으로 만들기 위해 반드시 없애야 하는 추가 비용은 현재 중앙값 기준 0이다. request-serial 1KiB는 0.973 µs/msg를 줄여야 한다. serial은 한 request의 왕복과 event-loop turn을 기다리므로 send target의 CPU 절감을 그대로 0.973 µs의 왕복 절감으로 예측하지 않는다. 영향을 받는 여섯 셀을 함께 다시 측정한다.

두 구조 대안을 비교했다. 첫 대안은 source M6A를 binding Message.allocate의 writable storage에 직접 쓰고 native Message를 제출하는 방법이다. frame 소유 형식을 codec·raw frame·port까지 전달해야 하며 현재 target 제한을 직접 줄이지 않는다. 두 번째 대안은 target에서 Message.copy로 native 참조를 공유하고 data Buffer를 보존한 뒤 임시 handle을 즉시 닫는 방법이다. 공개 binding 기능만으로 수신 수명 복사를 줄이며 한 runtime 파일을 바꾼다. 두 번째를 프로토타입으로 선택했다. 1.192 µs/msg는 managed memcpy 성분의 관측값이며 최종 절감량이 아니다. 새 N-API ref-copy·Buffer finalizer 비용은 여기에 포함되지 않았다. target만 빨라져도 source main 약 16.336 µs/msg가 다음 제한점이므로 큰 처리량 증가를 예상할 근거가 없다.

중복 작업 점검 결과는 네 항목이다. target native→managed 약 4399B와 serializer isolation 약 4099B는 서로 다른 수명 경계의 복사다(node-raw-binding-port.ts:565, ZLinkEncodedPayload.ts:24). 첫 경계만 프로토타입 제거 대상이다. source M6A result.set과 binding native init copy는 service-wire-m6a-codec.ts:265와 addon_core.cc:919에 있으며 남겼다. source adapter의 async await와 bench await는 client/framework-transport.js:65와 client/bench-core.js:420에 있으며 남겼다. mailbox 수신 시각이 일반 record 변환에서 누락되어 channel dispatcher가 다시 시각을 만드는 중복은 service-mailbox.ts:113, node-raw-mesh-backend.ts:2674, channel-dispatchers.ts:818에 있으며 남겼다. message model의 UTC 수신 시각은 queue 지연으로 바뀌면 안 된다. 기존 receiveIngressLifecycle helper는 시각을 전달하지만 release closure도 만들므로 이 프로토타입에 함께 섞지 않았다. 새 이슈 등록과 spec 편집은 수행하지 않았다.

수정 전/후 수신 수명 결정 소유자는 binding 1곳→1곳이고 serializer isolation 소유자는 1곳→1곳이다. target의 전체 payload materialization 경계는 실험에서 2곳→1곳으로 줄었다. 새 queue, lock, cache, flag 또는 persistent state는 없다. 임시 Message handle은 binding ref-count 공유 기능으로 만들고 finally에서 닫는다. byte 복사 제거와 결정 위치 감소를 같은 숫자로 주장하지 않는다.

이전 nprof의 Node CPU·native 표본 수집 방법을 재사용했다. nseg와 nseg2의 .NET segment 수치는 Node 비용으로 전용하지 않았다. nfix의 ArrayBuffer.slice 대체는 기존 측정에서 회귀했으므로 복사 함수 교체를 반복하지 않았다. 이번 후보는 binding의 수신 저장소 소유권을 사용한다.

참조 공유 후보는 native 호출을 무료로 만들지 않는다. binding message.ts:142의 copy는 native copy와 refCount를 호출하고 data는 native external Buffer를 만든다. 각 part의 임시 handle을 close하며 원래 reusable Received의 native handle도 다음 receive에서 닫는다. 두 part에서 copy·refCount·data·임시 close만 8회의 N-API 진입이 추가되고, 원본 handle close도 native 경로로 바뀐다. addon_message_values.h:126의 external Buffer finalizer가 frame 수명을 보존한다. 이 비용이 1.192 µs/msg 복사 절감보다 크면 후보를 기각해야 한다.

검증에서는 TypeScript build와 관련 Node contract 36개가 통과했다. 기존 작은 payload 수명 검사에 4KiB payload 16건을 추가했고, 다음 receive·no-data receive·역순 delayed reply·host close 뒤에도 전체 payload를 검사한다. 변경 전 runtime도 이 강화된 수명 검사 2개를 통과했다. 보존 bytes를 고의로 빈 Buffer로 만드는 음성 검사에서는 새 4KiB 검사가 `ERR_ASSERTION`으로 실패했다. 이 손상은 검사 뒤 원복했으며 측정에는 들어가지 않는다. serializer isolation 검사 1개와 benchmark 검사 16개도 통과했다. Prettier와 git diff --check도 통과했다. 출력은 prototype-checks.log, prototype-checks-final.log, negative-storage-check.log에 있다.

serializer isolation 검사의 첫 시도는 import 단계에서 실패했다. TypeScript 생성 CommonJS index가 ESM package의 stream-connector/dist 안에 있었고, 정상 build:browser가 생성하는 dist/package.json이 빠져 있었다. scripts/prepare_browser_build.mjs와 기존 codec bundle 빌드 명령으로 생성물을 갱신해 해결했다. source나 assertion은 이 실패 때문에 바꾸지 않았다. 갱신한 Protobuf server bundle은 baseline bundle과 SHA-256이 같음을 확인했다. Binding JavaScript 88개를 현재 TypeScript에서 emit한 결과와 비교했고 모두 일치했다. package의 native/src 전체도 현재 binding source와 일치했다(binding-source-check.json).

byte 진단에는 함수 이름을 보존한 diag-corrected 자료를 사용한다. 최초 hook은 Uint8Array.set의 이름을 바꿔 protobufjs의 feature check가 copy fallback을 선택했다. corrected 자료의 probe는 setName이 set이고 protobuf writer가 writeBytesBuffer_set임을 확인한다. 기존 처리량·Inspector CPU·perf·heap은 이 preload 없이 실행했으므로 영향을 받지 않았다. 첫 진단 원본도 diag에 보존한다. corrected 자료의 후처리에서 source 분모 카운터 이름 누락으로 한 번 중단됐으며, 공통 benchmark yield 카운터로 정상 집계했다. 원본 측정은 정상 종료했고 처리량 자료를 다시 고르지 않았다.

참고로 양쪽을 약 53420 msg/s로 제한한 첫 동일 속도 실험도 남겼다. 실제 target rate 차이가 각각 10.9 msg/s와 94.8 msg/s였던 두 쌍은 다음과 같다. CPU는 /proc 각 task의 user·kernel tick을 합하고 같은 창의 target 수신 수로 나눴다. tick은 100Hz다. 두 프로세스의 모든 thread를 포함하며 main thread만의 CPU가 아니다.

| 참고 회차 | 구현 | target msg/s | source user | source kernel | target user | target kernel | 합계 µs/msg |
|---|---|---:|---:|---:|---:|---:|---:|
| 1 | F | 53,411.1 | 18.396 | 13.032 | 20.526 | 9.145 | 61.099 |
| 1 | B | 53,422.1 | 12.105 | 8.314 | 19.422 | 7.862 | 47.703 |
| 2 | F | 53,514.6 | 17.801 | 12.510 | 20.553 | 8.641 | 59.504 |
| 2 | B | 53,419.8 | 12.321 | 8.280 | 19.325 | 8.068 | 47.994 |

참고 두 쌍의 F−B 총 CPU 차이는 각각 13.396 µs/msg와 11.510 µs/msg다. 세 번째 참고 쌍은 target rate가 F 52760.9 msg/s와 B 53429.8 msg/s로 달라 동일 속도 결론에 쓰지 않는다. 원본은 same-rate에 모두 보존했다. 이 참고 실험은 Framework도 pace했으므로 제한하지 않은 Framework rate에 binding만 맞추는 후속 비교와 구분한다. 다른 rate·pacing의 CPU 값을 포화 상태 단계 예산에 직접 대입하지 않는다.

후속 동일 rate 비교의 첫 시도는 13:06:33 KST 외부 codex process 0.885 CPU가 binding warmup과 겹쳐 전체 묶음을 제외했다(excluded-matched-rate-foreign-cli). 재개 자료의 세 번째 binding active 구간도 13:10:31 KST 외부 Python 0.994 CPU가 관측돼 중단했다. 정상 종료한 1·2회차는 유지하고 세 번째 쌍 전체를 교체했다. matched-rate-baseline/excluded와 matched-rate-baseline-replacement.json이 근거다. runtime 오류나 target count 불일치로 제외한 것이 아니다. 재개 전 외부 0.25 CPU 초과가 없는 상태와 MemAvailable 12 GiB 이상을 5초 연속 확인했다.

참조 공유 후보의 RUNS=5 결과는 다음과 같다. 각 회차에서 Framework와 binding을 번갈아 실행했고 source 0–7, target 8–15, warmup 10초·active 20초 조건을 유지했다. 정상적인 5회 결과만 집계했다.

| 셀 | F msg/s 중앙값 [최소–최대] | B msg/s 중앙값 [최소–최대] | F÷B | B µs/msg | F 예산 µs/msg | F µs/msg | 예산 초과 또는 여유 | 판정 |
|---|---:|---:|---:|---:|---:|---:|---|---|
| send-saturation 1024 | 51,077.7 [50,273.6–52,380.8] | 123,350.9 [116,848.0–123,588.4] | 0.4141 | 8.107 | 13.512 | 19.578 | over budget by 6.066 µs/msg | 0.6 미달 |
| send-saturation 4096 | 41,733.7 [40,767.9–43,278.2] | 92,645.8 [92,292.1–93,042.4] | 0.4505 | 10.794 | 17.990 | 23.961 | over budget by 5.972 µs/msg | 0.6 미달 |
| request-backpressure 1024 | 18,540.8 [18,380.8–18,931.2] | 27,985.0 [27,482.3–28,912.1] | 0.6625 | 35.733 | 59.556 | 53.935 | 여유 5.621 µs/msg | 0.62 초과 |
| request-backpressure 4096 | 18,105.5 [17,524.5–19,317.6] | 24,972.4 [24,748.1–25,452.8] | 0.7250 | 40.044 | 66.740 | 55.232 | 여유 11.508 µs/msg | 0.62 초과 |
| request-serial 1024 | 5,073.4 [5,044.7–5,096.8] | 9,667.3 [8,811.2–9,786.7] | 0.5248 | 103.441 | 172.402 | 197.106 | over budget by 24.704 µs/msg | 0.6 미달 |
| request-serial 4096 | 4,879.8 [4,821.9–5,080.1] | 8,446.1 [7,872.1–8,635.2] | 0.5778 | 118.397 | 197.329 | 204.926 | over budget by 7.598 µs/msg | 0.6 미달 |

후보는 기각한다. send 4KiB의 처리량 시간은 원본 16.938 µs/msg에서 후보 23.961 µs/msg로 7.023 µs/msg 증가했다. Binding 중앙값은 원본 92677.4 msg/s, 후보 대조 92645.8 msg/s로 비슷하다. 후보의 동일 세션 예산 17.990 µs/msg를 기준으로 over budget by 5.972 µs/msg다. send 1KiB도 over budget by 6.066 µs/msg다. request-backpressure는 두 크기 모두 0.6 위지만 send와 serial의 회귀를 상쇄하지 않는다. 경계였던 serial 1KiB는 후보에서 over budget by 24.704 µs/msg다.

| send 4KiB 비계측 main CPU | 원본 중앙값 [최소–최대] | 후보 중앙값 [최소–최대] |
|---|---:|---:|
| source | 16.336 [16.329–16.590] µs/msg | 15.688 [15.297–16.054] µs/msg |
| target | 16.579 [16.567–17.347] µs/msg | 23.672 [22.850–24.105] µs/msg |

후보의 target main이 제한점이다. 단위 CPU를 새 binding 참조 공유 경로와 GC·메모리 정리에 더 사용하며 source는 기다릴 수 있다. 위 비계측 창에는 초기 latency 표본 수집의 일부가 포함될 수 있어, 세부 CPU 분해는 205000건 수신 뒤 시작한 별도 캡처로 확인한다.

후보 3회차 active 중 외부 Node 2.806 CPU가 관측돼 그 회차 전체를 제외·교체했다. 4회차 warmup도 같은 외부 작업의 Node 2.604 CPU가 관측돼 교체했다. 유효한 앞선 회차는 그대로 유지했다. prototype/excluded와 prototype-replacement.json에 원본과 근거를 보존했다. 결과가 낮거나 높다는 이유로 회차를 선택하지 않았다.

후보의 target CPU 분해는 다음과 같다. 원본과 같은 Inspector 분류·schedstat 보정법이며 각 열의 합이 main CPU다. 서로 다른 포화 rate의 캡처이므로 차이를 보편적인 고정 비용으로 보지 않는다.

| target Inspector API 영역 µs/msg | 원본 | 참조 공유 후보 |
|---|---:|---:|
| Framework bookkeeping | 3.329 | 3.907 |
| encode/serialize | 1.845 | 2.050 |
| Buffer copy | 1.791 | 2.502 |
| allocation | 3.173 | 0.191 |
| binding call | 2.812 | 9.504 |
| Promise/microtask | 1.515 | 1.366 |
| handoff/event loop | 0.012 | 0.027 |
| GC | 0.583 | 2.797 |
| benchmark | 0.189 | 0.250 |
| other | 2.149 | 3.082 |
| main 합계 | 17.398 | 25.676 |

Target main은 원본 캡처 17.398 µs/msg에서 후보 25.676 µs/msg로 증가했다. allocation API는 3.173→0.191 µs/msg로 감소했지만 binding call은 2.812→9.504 µs/msg, GC는 0.583→2.797 µs/msg로 증가했다. 남겨 둔 ZLinkEncodedPayload.data의 self도 1.425→2.026 µs/msg다. managed memcpy 1.192 µs/msg를 지웠다고 같은 크기의 순절감이 생기는 구조가 아니었다. Native copy·refCount·data·close와 external Buffer 정리 비용이 추가되는 경로다. 임시 copy가 Frame reference를 공유하므로 큰 payload deep copy를 다시 하는 구현은 아니다.

| target 별도 진단의 메시지당 작업 | 원본 | 후보 |
|---|---:|---:|
| Buffer.allocUnsafe 요청 B | 4398.988 | 0.000 |
| serializer Uint8Array 생성 복사 B | 4099.444 | 4099.295 |
| Message.copy 호출 | 0.000 | 2.000 |
| Message.data 호출 | 2.000 | 2.000 |
| Message.close 호출 | 2.000 | 4.000 |
| Promise 생성 | 9.103 | 9.096 |
| Promise continuation before | 8.058 | 8.056 |
| Immediate 생성 | 0.016 | 0.016 |
| GC 관측 duration µs | 0.555 | 2.669 |

약 4399B/msg managed receive materialization은 제거됐고 약 4099B/msg isolation 복사는 유지됐다. Message.copy는 약 2회/msg, close는 약 2→4회/msg다. Promise continuation과 Immediate 생성은 거의 그대로라 새 queue나 turn 증가로 설명하지 않는다. GC 관측 duration은 계측된 실행의 값이며 순수 CPU 수치와 다르다. Source 진단의 payload 생성 카운터와 benchmark yield 카운터는 메시지당 약 1회로 맞았다. CPU profile의 새 binding data native wrapper inclusive는 약 1.995 µs/msg, copy native wrapper inclusive는 약 1.446 µs/msg이며 전체 recv·record constructor와 중첩되므로 더하지 않는다.

리팩토링 점검은 중복 작업 4건을 기록했다. 참조 공유 후보에서는 target materialization 1건을 제거했으나 성능 회귀로 기각한다. 최종 runtime에는 제거를 채택하지 않으므로 발견 4건, 채택한 수정 0건, 남은 4건이다. 소유 결정 위치 수는 수신 lifetime binding 1→1, serializer isolation 1→1이다. 전체 payload materialization 경계 수는 실험에서 2→1이었고 원복 후에는 2→2다. 새 queue·lock·cache·flag·Framework persistent state는 0개다.

소유 계층은 binding의 Message payload reference·Buffer lifetime이며 Framework는 기존 record 배열을 보관한다.

spec 근거는 bindings/doc/spec/node/README.ko.md:660의 ref-share·Buffer GC 수명과 bindings/doc/spec/README.ko.md:1144의 dequeue charge 분리다.

교차언어 대조에서는 C++·.NET·Java/Kotlin의 native frame owner와 view 보존을 확인했고 observable wire·payload lifetime 계약을 바꾸지 않았다.

변경 분류는 A, 기존 binding 계약을 사용하는 내부 적응 실험이다. 성능 결과에 따라 runtime 변경을 채택하지 않는다.

최종 검증에서 원본 36개 실행과 후보 60개 실행, 동일 rate 비교 6개 실행의 오류·abandoned·rejected는 모두 0이었다. 모든 source·target 최종 건수가 일치했고 CPU affinity와 기록한 runtime variant hash가 맞았다. 전체 유효 자료의 관측 MemAvailable은 12 GiB 이상이었다. 검증 결과는 validation-summary.json과 final-validation.txt에 있다. 기능 test의 남은 실패는 없다. 성능 기준의 남은 항목은 원본 serial 1KiB의 over budget by 0.973 µs/msg이며, 회차별 비율 변동과 함께 해석한다. receivedAt 중복 생성도 위치와 계약 근거만 기록하고 수정하지 않았다.

실험 변경 파일은 runtime의 node-raw-binding-port.ts, 수신 수명 contract test, benchmark client의 bench-core.js와 main.js였다. 네 파일의 변경은 전부 되돌렸다. 원본 runtime source·JS·declaration·build cache SHA-256 복원을 runtime-restored.json으로 확인했다. git status에는 시작 전의 cpp·dotnet·java·node run_local.sh CPU-pin 변경 네 개만 남는다. bindings·core·spec은 수정하지 않았고 commit·GitHub 작업도 수행하지 않았다. 측정 프로세스는 모두 종료·회수했다. native perf map 4개를 artifact에 보존하고 사용한 /tmp map 32개를 정리했다.

재현 도구는 run.py, collect.py, guard.py, profile-summary.py, perf-summary.py, diag.cjs, diag-summary.py, same-rate-analysis.py와 validate-artifacts.py다. 원복한 worktree에서 다시 실행하려면 먼저 measurement.diff를 적용해야 한다. 이 patch가 rate pacing과 git metadata 조회 제거를 제공하며, run.py도 이 조건을 확인한다. 후보는 prototype.diff를 추가 적용하고 measure-shared.sh로 build-prototype.sh를 실행한다. 이후 `flock -o /root/jobs/fx/cx/measure.lock python3 /root/jobs/fx/cx/v21/s033/nsend/run.py --out 새출력이름 --runs 5`로 재측정할 수 있다. 기존 batch driver는 고정 output 이름을 사용하므로 그대로 재실행해 이전 자료를 덮어쓰지 않아야 한다.
