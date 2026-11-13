현재 main에서 C++·.NET·Java Framework target의 메시지당 수신 비용을 같은 전달률로 비교한다. 요청 셀은 `request-backpressure 1024 B`이며 target의 응답 인코딩·제출도 계산한다.

계측 준비 중이다. 아직 3회씩의 CPU 계측과 별도 할당 계측을 모두 완료하지 않았으므로 stage별 수치와 언어별 우선순위를 제시하지 않는다.
지정한 릴리스 gate 판정 파일이 아직 없다. 측정 잠금을 요청하지 않았으며, 공유 잠금을 사용하는 지정 빌드 wrapper의 실행을 기다리고 있다.

(1) 빌드와 비교 조건

소스 commit은 `c9408ba50e2cf17bdd555b755ba112304572c22f`이다. fetch 후 `origin/main`과 같고 detached worktree의 시작 상태에는 변경 사항이 없었다. 저장소 파일은 수정하지 않는다. 계측 코드는 JOB 아래에만 둔다.

빌드는 아직 시작하지 못했다. C++은 `cpp-build-slot.sh`를 외부 flock 없이 사용하고, Release `-O3 -DNDEBUG -fno-omit-frame-pointer`, 병렬 1개, process-group RSS 25 GiB 상한으로 준비했다. .NET은 Release와 DebugSymbols=false를, Java는 JDK 25와 javac debug=false를 사용한다.

사용할 Core 1.23.0 후보의 SHA-256은 `6f496304ef6e1287f57234567231bbd2b76ad2392f99e816001417d01215bf01`이다. 아직 실행한 프로세스가 없으므로 적재 일치 검증은 완료되지 않았다.

source CPU는 0–7, target CPU는 8–15다. 수집 중 모든 thread의 affinity를 매초 확인한다. 각 계측은 gate 판정 파일이 생긴 뒤 `flock -o /root/jobs/fx/cx/measure.lock` 안에서 하나씩 실행하며, 잠금 inode의 소유자와 MemAvailable 12 GiB 이상을 확인한다.

CPU 계측은 30초 warmup 뒤 active 안의 22초 구간에 target `perf record -e cpu-clock:u -F 4999 --call-graph fp -k 1`을 사용한다. 각 회차 stage의 user 표본 period 비율을 같은 구간 `/proc/PID/stat`의 실측 user CPU에 정규화한다. 세 회차의 CPU 시간을 합한 뒤 target 수신 건수의 합으로 나눈다. 반올림 전 stage 합은 실측 user CPU와 같다. system CPU는 같은 counter의 별도 행이며 user stage에 더하지 않는다.

.NET JIT은 DOTNET_PerfMapEnabled=1로 생성한 주소 범위 map으로 해석한다. Java JIT은 JDK 25 내장 Compiler.perfmap과 DumpPerfMapAtExit를 사용하고 PreserveFramePointer를 켠다. 외부 perf-map-agent나 async-profiler는 사용하지 않는다. map 생성은 CPU 구간 밖에서 실행한다. 최적화로 인라인된 코드와 caller가 끊긴 stack은 독립 stage로 복원할 수 없으며 확인할 수 없는 owner는 미귀속으로 남긴다.

(2) stage별 비용과 별도 할당 계측

send-saturation 1024 B. CPU 단위는 user µs/message이며 alloc은 allocation request/message다.

계측 미완료.

request-backpressure 1024 B. CPU 단위는 user µs/message이며 alloc은 allocation request/message다.

계측 미완료.

할당 표의 C++ 값은 malloc/calloc/realloc/정렬 할당과 operator new를 중복 없이 센 호출 수다. in-place realloc도 한 요청이다. .NET·Java 열은 native 할당 요청에 managed heap 객체를 더한다. 이 둘의 수는 물리 메모리 페이지 할당 수가 아니다. native stack은 검증한 frame pointer만 따라가며 끊긴 부분은 미귀속으로 남긴다.

.NET은 별도 EventPipe GCAllocationTick의 AllocationAmount64/ObjectSize를 합한 객체 수 추정이다. 약 100 KiB 단위의 allocation-context 사건이므로 작은 객체·대형 객체와 refill을 일으킨 호출 경로의 표본 편향이 있다. 표본 사건의 stack에 전체 가중치를 귀속하므로 stage별 건수는 정확한 전수 계수가 아니다. Java는 별도 -XX:-UseTLAB 실행에서 JFR ObjectAllocationOutsideTLAB를 세며 Java heap 사건은 전수 계수를 목표로 한다. 이 설정이 생성한 부하는 timing 실행에 포함하지 않는다. JFR data-loss 검사 결과와 native boundary 오차를 원시 분석에 기록한다.

C++ counting build는 저장소를 수정하지 않고 JOB의 task.hpp·std_function.h overlay에서 계측 호출을 추가한다. 실제 coroutine frame operator new, task_shared_state 생성이 들어간 별도 heap block, 기본·null·copy·move·callable std::function 생성을 구분한다. shared state 객체 여러 개가 한 allocation 안에 있으면 한 번만 센다. std::function 열에는 힙을 사용하지 않는 생성과 계측 때문에 최적화로 제거되지 않는 생성도 들어간다. 생성 건수를 CPU 시간이나 힙 할당 수로 대신 해석하지 않는다. 원본·overlay hash와 diff는 raw에 있다.

(3) 각 stage의 소유 파일과 행

아래 경로와 행은 위 main commit 기준이다. 확인에 사용한 소스 사본은 raw/source에 보존했다. generic allocator·lock·container는 소유자가 아니며 stack leaf에서 caller 방향으로 만나는 첫 명시적 stage에 한 번만 귀속한다. 공통 Core의 비동기 IO에는 Framework caller가 남지 않으므로 명시적인 read/write 방향을 우선하고 방향이 없는 Core stack은 target receive 경계로 둔다. send 셀의 소량 control send는 미귀속에 포함한다.

| stage | C++ | .NET | Java |
| --- | --- | --- | --- |
| receive boundary | `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:473`; `bindings/cpp/src/Runtime/Native/native_receive.hpp:177` | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:6332`; `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.ReceiveCore.cs:29` | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawServicePort.java:560`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawServicePort.java:621` |
| route classification | `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:3606`; `framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp:203`; `framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp:348` | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:7061`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkMeshNodeRouteDispatcher.cs:384` | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:5962`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkMeshApplicationDispatcher.java:198` |
| header/wire decode | `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:1232`; `framework/languages/cpp/framework/src/runtime/channels/route_handler_registry.hpp:77` | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs:377`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkApplicationPayloadEnvelopeCodec.cs:211` | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkChannelEnvelope.java:638`; `framework/languages/java/zlink-framework-codec-protobuf/src/main/java/systems/zlink/framework/codecs/protobuf/ZLinkProtobufMessageSerializer.java:75` |
| mailbox enqueue·claim | `framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:79`; `framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:172`; `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:739` | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkMeshOwnedMailbox.cs:76`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkMeshOwnedMailbox.cs:104` | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:5676`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:5775` |
| dispatch·invoke | `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:8098`; `framework/languages/cpp/framework/src/runtime/channels/route_handler_invoker.cpp:10` | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:16`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Handlers/ZLinkHandlerInvocationEngine.cs:7` | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkHandlerDispatch.java:16`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelHandlerInvoker.java:66` |
| handler | `framework/bench/grpc/cpp/framework/bench_framework_cpp_server.cpp:23`; `framework/bench/grpc/cpp/framework/bench_framework_cpp_server.cpp:34` | `framework/bench/grpc/dotnet/ZLinkServer/Program.cs:62`; `framework/bench/grpc/dotnet/ZLinkServer/Program.cs:76` | `framework/bench/grpc/java/zlink-framework-server/src/main/java/systems/zlink/bench/withgrpc/frameworkserver/BenchEchoHandler.java:28`; `framework/bench/grpc/java/zlink-framework-server/src/main/java/systems/zlink/bench/withgrpc/frameworkserver/BenchCommandHandler.java:24` |
| completion·permit | `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:247`; `framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:383`; `framework/languages/cpp/framework/src/runtime/dispatch/application_job_queue.hpp:157` | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Dispatch/ZLinkApplicationJobQueue.cs:1291`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Dispatch/ZLinkApplicationJobQueue.cs:1177` | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/dispatch/ZLinkApplicationJobQueue.java:713`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkMeshApplicationDispatcher.java:763` |
| reply submit | `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:510`; `framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp:427` | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:7433`; `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:243` | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawServicePort.java:524`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:6240` |
| metrics/bench | `framework/bench/grpc/cpp/common/bench_stats_server.hpp:45`; `framework/bench/grpc/cpp/common/bench_stats_server.hpp:81` | `framework/bench/grpc/dotnet/Shared/BenchServerMetrics.cs:30` | `framework/bench/grpc/java/shared/src/main/java/systems/zlink/bench/withgrpc/shared/BenchServerMetrics.java:45` |
| unattributed | 확인한 소유자 없음 | 확인한 소유자 없음 | 확인한 소유자 없음 |

Java의 node 경로는 ZLinkServiceMailbox를 사용하지 않고 receive Rotation에 보관한 batch를 application dispatcher로 넘긴다. 따라서 mailbox 행은 이 batch의 enqueue·claim에 해당하는 작업을 대응한다. 별도 service mailbox가 없다는 구조 차이도 CPU 차이의 일부다.

(4) C++이 가장 낮은 언어보다 비싼 stage

새 timing 결과가 없으므로 gap이나 다음 C++ 작업의 순서를 정하지 않았다. 과거 csprof2·ctgt·jcost2의 수치를 새 rate의 수치로 재사용하지 않는다.

(5) 수신 경로의 중복 작업 점검

- C++ 활성 경로: retained-byte 합계를 enqueue와 claim에서 같은 parts에 대해 각각 계산한다. `framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:88`; `framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:196`
- C++ 활성 경로: try_claim 또는 try_claim_owner가 owner map을 조회한 뒤 claim_owner_locked가 같은 owner를 다시 조회한다. 같은 mutex 아래의 반복 조회다. `framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:119`; `framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:172`
- C++ 활성 경로: request 수신에서 일반 service header를 읽은 뒤 decode_node_request_header가 같은 prefix를 다시 검증한다. `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:3648`; `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:4162`; `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:509`
- C++ 활성 경로: request handler는 역직렬화된 protobuf request를 반환하면서 별도 protobuf 값을 복사한다. .NET과 Java handler는 같은 request 객체 참조를 반환한다. `framework/bench/grpc/cpp/framework/bench_framework_cpp_server.cpp:26`
- C++ 조건부 또는 중복이 아닌 항목: flow tracing을 켜면 enqueue_received의 envelope decode와 host dispatch의 decode가 반복된다. 이번 계측에서는 tracing을 끄므로 이 비용을 절감량으로 더하지 않는다. `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:3290`; `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:2317`
- .NET 활성 경로: ProcessReceived가 infrastructure 분류를 위해 magic/version을 확인한 뒤 application의 TryDecodePrefix가 같은 필드를 다시 검증한다. `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:7061`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkServiceWireCodec.cs:2846`
- .NET 조건부 또는 중복이 아닌 항목: mailbox는 PayloadBytes/PendingBytes를 한 번 보관해 claim에서 다시 합산하지 않는다. 일반 receive와 application payload 길이는 서로 다른 예산이라 같은 결정의 중복으로 세지 않는다. `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkMeshOwnedMailbox.cs:168`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkMeshOwnedMailbox.cs:104`
- Java 활성 경로: claimRecord의 infrastructure 판별과 decodeHeader가 같은 service prefix를 읽는다. request의 decodeNodeRequestHeader는 decodeHeaderFields를 다시 호출하므로 request에서는 세 번째 prefix 확인이 있다. `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:5962`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:5883`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6AWireCodec.java:579`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6AWireCodec.java:242`
- Java 조건부 또는 중복이 아닌 항목: claimRecord에서 얻은 header bytes는 ClaimedRecord.head로 dispatch에 전달한다. 이 경로는 같은 native header를 다시 toByteArray하지 않는다. application 본문은 multipart view로 전달한다. `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawServicePort.java:784`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:5936`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:5467`

점검 결과는 활성 후보 C++ 4개, .NET 1개, Java 1개다. 이 작업에서 수정한 제품 코드는 0개이며 새 이슈로 이관하지 않는다. 동일 수신 경로의 규칙 수를 바꾸지 않았다.

검증 완료 여부: CPU 0/18회, 할당 0/6회. 측정의 완료를 뜻하는 문장은 이 횟수와 각 raw 검사를 모두 확인한 뒤 작성한다.

