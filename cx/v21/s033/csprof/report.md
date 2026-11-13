# C++ send-saturation 1 KiB target CPU 분해

## 방법과 실행 목록

기준 C++ 실행은 `csendb/build-main`의 main `a0056577c4`이며, 빌드 옵션은 `-O3 -DNDEBUG -g -fno-omit-frame-pointer`다. 대상 소스는 `csendb/main-source`다. 기존 `csendb/scripts/measure.py`, `collect.py`와 `bench.diff`의 paced 설정 및 CPU pin을 재사용했다. 저장소와 기존 바이너리는 수정하지 않았다.

C++ Framework pair와 binding pair를 각각 3회 측정했다. 각 회차는 136,889 msg/s, active 20초, perf capture 약 12초였다. 측정 명령 전체를 `flock -o /root/jobs/fx/cx/measure.lock` 안에서 실행했다. Source는 CPU 0–7, target은 CPU 8–15였다. collector가 양쪽 프로세스의 모든 thread affinity를 확인했고 모두 지정 CPU 집합 안에 있었다. `perf record`는 양쪽 프로세스의 모든 thread에 `cpu-clock -F 199 --call-graph dwarf,16384`를 사용했다. lost sample은 0이었다. 원시 결과, 명령, affinity 전후 값은 `cpp-framework-r{1,2,3}/`와 `cpp-binding-r{1,2,3}/`에 있다. perf 표와 합계는 `analysis/cpp/`에 있다.

C++의 프로파일 표본 비율은 회차별 단계 분류 비율의 중앙값으로 계산하고, 기존 비프로파일 paced CPU 중앙값에 정규화했다. 기준 총 CPU는 main 22.051, binding 4.948 µs/message다. 아래 분류의 중앙값을 따로 취해 합이 기준과 소수점 아래에서 완전히 같지는 않다. 표는 profiler 표본에서 직접 판정할 수 있는 단계의 추정치이며, 계측을 위해 코드를 바꾸지 않았다.

.NET 벤치는 `ratio040` commit `475305bebc`에서 읽고, 산출물은 `csprof/dotnet-bin/`에만 빌드했다. `DOTNET_PerfMapEnabled=1`을 사용했다. 처음 수집한 perf 파일은 clock 설정 때문에 JIT 주소를 해석하지 못해 보존만 하고, 복사한 수집 스크립트에 `perf record -k 1`을 추가해 Framework와 binding pair를 각 3회 다시 프로파일했다. 최종 심볼 해석 파일은 `analysis/dotnet/`, JIT map 복사본은 `dotnet-jitmaps/`에 있다. 원시 결과는 `dotnet-framework-jit/`와 `dotnet-binding-jit/`에 있다.

.NET 벤치에는 `CSEND_PACE` 또는 정해진 msg/s 설정이 없다. 따라서 136,889 msg/s로 고정할 수 없었고, 아래 .NET 결과는 자연 처리량이다. 심볼 해석된 세 회차의 Framework 중앙 처리량은 181,643 msg/s, binding은 192,625 msg/s, F÷B는 0.943이었다. 별도로 먼저 실행한 미해석 profile batch에서는 각각 약 178,497 및 216,386 msg/s로 F÷B 0.825였다. 사용자 제공 .NET F÷B 약 0.831에 가까운 batch의 JIT method 이름은 해석되지 않았으므로 두 batch의 rate와 심볼을 섞어 같은-rate 결과라고 주장하지 않는다. 표본은 심볼 해석된 재실행의 stage share를 사용하며, CPU/message 값은 그 재실행 중 수집한 CPU counter 중앙값으로 환산했다. .NET은 unprofiled control을 별도로 실행하지 않았다.

## C++ target 단계별 비용

모든 수치는 µs/message다. `초과`는 Framework에서 같은 단계의 binding 비용을 뺀 값이다.

| 단계 | Framework main | binding | Framework 초과 | 주요 소유 코드 또는 근거 |
|---|---:|---:|---:|---|
| Core/binding receive | 1.524 | 0.496 | +1.028 | `raw_mesh_node_owner.cpp:3602`, `raw_route_port.cpp:459`, `native_receive.hpp:177` |
| wire decode | 0.259 | 0.000 | +0.259 | `envelope_codec.cpp:1196`; raw binding은 Framework envelope를 decode하지 않음 |
| envelope/route classification | 0.635 | 0.090 | +0.545 | `route_packet_dispatcher.cpp:203`, `mesh_record_dispatcher.cpp:32` |
| mailbox enqueue/claim | 1.004 | 0.000 | +1.004 | `raw_mesh_node_owner.cpp:3286`, `service_mailbox.hpp:146` |
| dispatch/coroutine | 0.173 | 0.000 | +0.173 | `public_host_runtime.cpp:7503,8108`, `route_handler_invoker.cpp:10` |
| handler | 0.292 | 0.045 | +0.247 | Framework handler·benchmark metric 처리; raw pair의 command loop와 `server_metrics_t::record` 표본 |
| completion/terminal | 0.136 | 0.007 | +0.129 | `application_job_queue.hpp:157`, `mesh_node_host_service.cpp:247` |
| allocation | 0.791 | 0.057 | +0.734 | 프로세스 표본의 `malloc`, `_int_malloc`, `free`, `cfree` |
| lock/futex/wakeup | 0.803 | 0.113 | +0.690 | `pthread_mutex_lock`, 커널 spinlock 및 syscall 표본 |
| 기타·미분류 | 7.430 | 1.511 | 약 +5.919 | libc·kernel·vdso·표본에 잡힌 다른 Framework/표준 라이브러리 코드 |
| 기준 CPU 중앙값 | **13.176** | **2.353** | **+10.823** | 직접 측정한 process user+system CPU |

Framework target의 thread별 표본 비율을 같은 비프로파일 CPU 기준에 정규화했다. Framework는 IO thread 중앙값 33.51% 또는 4.414 µs, `bench_cpp_frame` thread 66.49% 또는 8.762 µs였다. 별도 worker thread는 관찰되지 않았고, worker 역할 비용은 0이다. 메인 thread가 mailbox/drain/dispatch를 함께 수행했다. binding target은 IO thread 51.96% 또는 1.222 µs, `bench_cpp_zlink` main thread 48.04% 또는 1.131 µs다. 이 pair에서도 별도 worker thread는 없었다.

## C++ source 단계별 비용

| 단계 | Framework main | binding | Framework 초과 | 주요 소유 코드 또는 근거 |
|---|---:|---:|---:|---|
| submit | 0.578 | 0.081 | +0.497 | `mesh_node_runtime.cpp:4252`, `channel_runtime.cpp:1441` 및 send call 경로 |
| encode | 0.549 | 0.061 | +0.488 | `envelope_codec.cpp:357,452,528,1316`, `service_wire_codec.cpp:3224` |
| binding/Core call | 0.935 | 0.258 | +0.677 | Framework send의 binding 진입과 native message 제출 |
| allocation | 0.549 | 0.271 | +0.278 | malloc/free 표본 |
| lock/wakeup | 0.714 | 0.271 | +0.443 | mutex, syscall, kernel 표본 |
| 기타·미분류 | 5.557 | 1.644 | 약 +3.913 | CLR/OS 및 분류되지 않은 sender 표본 |
| 기준 CPU 중앙값 | **8.875** | **2.595** | **+6.280** | 직접 측정한 process user+system CPU |

source stage는 프로파일러 share를 기존 비프로파일 main 및 binding source CPU 중앙값에 각각 정규화했다. 각 단계의 초과량 합은 반올림 및 별도 중앙값 때문에 전체 차이 6.280과 약간 다를 수 있다.

## Framework target의 상위 15개 main-source 함수

아래는 Framework 실행 파일 안에서 Framework·binding 소스에 속하는 함수의 self sample 비율이다. 비율은 첫 profile 회차 기준이다. libc, kernel, vdso 및 표준 라이브러리 함수는 main-source 함수가 아니므로 목록에서 제외했다.

| self 표본 | 함수 | main-source 위치 |
|---:|---|---|
| 1.82% | `raw_mesh_node_owner_t::receive_next` | `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:3602` |
| 1.31% | `service_mailbox_record_t` move constructor | `framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.hpp:146` |
| 1.19% | `route_packet_dispatcher_t::dispatch_send` | `framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp:203` |
| 0.94% | `raw_mesh_node_owner_t::enqueue_received` | `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:3286` |
| 0.88% | `route_handler_invoker_t::invoke` | `framework/languages/cpp/framework/src/runtime/channels/route_handler_invoker.cpp:10` |
| 0.78% | `public_host_runtime_t::dispatch_application_claim` | `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:8108` |
| 0.72% | `recv_whole_envelope` | `bindings/cpp/src/Runtime/Native/native_receive.hpp:177` |
| 0.66% | `message_t` move constructor | `bindings/cpp/src/Runtime/Messaging/message.cpp:72` |
| 0.56% | `envelope_header_t` move constructor | `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.hpp:145` |
| 0.56% | `envelope_header_t` destructor | `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.hpp:145` |
| 0.50% | canonical header reader `string` | `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:625` |
| 0.47% | `mesh_record_dispatcher_t::dispatch` | `framework/languages/cpp/framework/src/runtime/mesh/mesh_record_dispatcher.cpp:32` |
| 0.47% | `public_host_runtime_t::dispatch_ready_impl` | `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:7503` |
| 0.44% | `raw_route_port_t::receive_if_ready` | `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:459` |
| 0.44% | `application_dispatch_terminal_owner_t` move constructor | `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:247` |

그 다음 표본은 `application_job_queue_t::permit_t::~permit_t` 0.44% (`application_job_queue.hpp:157`), `decode_header_bytes` 0.38% (`envelope_codec.cpp:1196`), `header_sax_t::finish` 0.34% (`envelope_codec.cpp:938`)였다. `service_mailbox_record_t`는 aggregate type이라 move 생성 코드가 한 줄의 명시적 함수 정의로 존재하지 않는다. 위 위치는 해당 생성 코드의 소유 type 선언이다.

## C++와 .NET 단계 비교

.NET은 같은 CPU pin과 동일한 실행 경로를 썼지만 고정 전송률이 없었다. `.NET Framework` target CPU/message 중앙값은 14.090, source 18.781 µs였다. `.NET binding` target은 6.687, source 27.747 µs였다. Framework target stage share는 심볼 해석된 세 profile에서 반복된 함수 표본을 같은 단계로 묶어 CPU/message 중앙값에 곱한 추정치다. 함수가 inline되거나 여러 책임을 한 함수에서 처리한 경우 stage 경계를 나눌 수 없다. 괄호 안은 해당 책임을 가진 .NET 코드 위치다.

| target 단계 | C++ main | .NET Framework 추정 | .NET binding 추정 | 분류 및 .NET 위치 |
|---|---:|---:|---:|---|
| Core/binding receive | 1.524 | 약 0.16 | 약 0.20 | 같은 단계, C++ 비용이 더 큼(+1.36 vs .NET Framework). `SocketKernel.ReceiveCore.cs:29`, raw target의 `Program.cs:119`, `libzlink` receive |
| wire decode | 0.259 | 약 0.39 | 0 | Comparable: 같은 단계이나 .NET이 약 0.13 높고, Framework `ProcessReceived`의 route 파싱과 분리되지 않음. raw binding에는 Framework envelope decode가 없음. `ZLinkManagedMeshNode.cs:7061` |
| envelope/route classification | 0.635 | 약 0.52 | 약 0.12, command loop와 결합 | 같은 단계, C++ 비용이 더 큼(+0.12 vs .NET Framework). `ZLinkMeshNodeRouteDispatcher.cs:384,538`, raw `Program.cs:119` |
| mailbox enqueue/claim | 1.004 | 약 0.59 | 0 | 같은 단계, C++ 비용이 더 큼(+0.41 vs .NET Framework). `ZLinkManagedMeshNode.cs:12930`, `ZLinkMeshOwnedMailbox.cs:104` |
| dispatch/coroutine | 0.173 | 약 0.45 | 0 | Comparable: 같은 역할이나 .NET Framework 추정치가 약 0.28 높음. `ZLinkMeshDispatchPump.cs:467,707`, `ZLinkRouteHandlerInvoker.cs:8` |
| handler | 0.292 | 0.04 미만 표본 | 약 0.13, command loop와 결합 | 같은 단계, C++ 비용이 더 큼(관측치 기준 최소 약 0.25). .NET Framework handler 표본은 해상도 아래. `ZLinkServer/Program.cs:79`, `Shared/BenchServerMetrics.cs:45`, raw `Program.cs:119` |
| completion/terminal | 0.136 | 약 0.28 | 0 | Comparable: 같은 역할이나 .NET Framework 추정치가 약 0.14 높음. `ZLinkApplicationJobQueue.cs:1291,1307` |
| allocation | 0.791 | 약 0.13 native malloc/free만 | 0.1 미만 native 표본 | 같은 단계의 native 표본에서 C++ 비용이 더 큼(+약 0.66). .NET managed allocation CPU는 이 perf 분류로 분리되지 않음. `ZLinkManagedMeshNode.cs:12930` |
| lock/futex/wakeup | 0.803 | 약 0.51 | 약 0.19 | 같은 단계, C++ 비용이 더 큼(+0.29 vs .NET Framework). 커널 schedule/spinlock 표본 및 `ZLinkMeshOwnedMailbox.cs:100` |
| 기타·미분류 | 7.430 | 약 10.8 | 약 5.9 | Comparable 판정 불가. 잔여 비중이 크며 Framework 기타에는 CLR, scheduling, clear/copy 표본이 포함됨 |

`.NET` Framework target 상위 managed 표본은 `ZLinkManagedMeshNode.ProcessReceived` 2.6–3.0%, `EnqueueOwned` 2.5–2.7%, `ZLinkMeshOwnedMailbox.ClaimAndDrainOnLane` 1.6–1.8%, route dispatcher 단계 3.5–3.8%, dispatch pump의 enqueue/drain 약 2.2%였다. 같은 책임을 처리하는 C++ Framework 경로가 존재하므로 이 단계들은 C++ only가 아니다. `ProcessReceived`에 포함된 route 분류와 wire decode는 perf 표본만으로 나눌 수 없다.

C++ only로 분류된 target 단계는 없다. 모든 Framework 책임에 .NET counterpart가 확인됐고, raw binding의 0 비용은 해당 Framework 전용 stage를 실행하지 않는다는 뜻이다. Managed handler처럼 표본 해상도 아래인 항목과 기타 구간은 같은 stage의 costlier/comparable 판정을 보류했다.

| source 단계 | C++ main | .NET Framework 추정 | .NET binding 추정 및 분류 |
|---|---:|---:|---|
| submit | 0.578 | 약 1.54 | 약 1.36–1.72. 같은 단계, .NET Framework 추정치가 더 큼. `Client/BenchMeasurement.cs:507`, `Client/Program.cs:357`, raw `ZLinkRawServer/Program.cs:119` |
| encode | 0.549 | 약 0.44 | raw binding에는 Framework envelope counterpart가 없음. 같은 Framework 단계에서 C++가 약 0.11 µs 더 큼. `ZLinkEnvelopeCodec.cs:183,188` |
| binding/Core call | 0.935 | 약 0.35, submit 표본과 일부 합쳐짐 | Comparable 판정이 어렵고 .NET에서 별도 구간으로 분리되지 않음. `ZLinkManagedMeshNode.cs:10955` |
| allocation | 0.549 | 약 0.91 native 할당 기준 | 약 0.49 native 할당 기준. .NET managed allocation 미분리; 관측 native 비용은 .NET Framework가 약 0.36 µs 더 큼 |
| lock/wakeup | 0.714 | 약 0.70 | 약 4.7. Framework는 C++와 comparable이고, raw .NET binding sender의 ThreadPool semaphore·schedule·yield 표본이 더 큼 |
| 기타·미분류 | 5.557 | 약 14.8 | 약 20.5. Comparable 판정 불가. CLR과 OS 표본이 주로 남아 직접 대응 불가 |

source의 .NET 단계별 값은 각 pair의 run 1 심볼 share를 해당 pair의 세 회차 CPU/message 중앙값에 곱한 거친 추정치다. raw binding source CPU가 Framework source보다 컸다. .NET의 자연 처리율·scheduler 변동 때문에 이 표를 C++ 136,889 msg/s의 동일 부하 비교로 취급하지 않는다. .NET batch들 사이 처리율 차이가 커도 pace API를 임의로 추가하거나 benchmark 코드를 바꾸지 않았다.

## F÷B 0.6 CPU 예산에서 줄여야 할 단계

같은-rate C++ binding 총 CPU는 source 2.595 + target 2.353 = 4.948 µs/message다. F÷B 0.6을 총 CPU 비용 예산으로 환산하면 `4.948 ÷ 0.6 = 8.247 µs/message`다. main은 8.875 + 13.176 = 22.051이므로 최소 13.804 µs/message를 줄여야 한다. Framework는 binding 비용 위에 최대 3.299 µs/message만 더 쓸 수 있다.

단계별 관측 초과량은 target 4.809, source 2.383 µs/message이며, 단계 중앙값 합의 독립 반올림 때문에 단순 총합과 다르다. 분류된 초과량은 합계 7.192 µs/message이고, 미분류 초과량은 약 9.8 µs/message다. 따라서 명시된 단계 전부를 binding 비용 수준까지 내려도 미분류 구간에서 최소 6.6 µs/message를 더 줄여야 예산에 닿는다. profiler만으로 그 잔여량을 단일 코드 함수에 귀속할 수는 없다. 아래 목록은 예산에 포함되는 단계와 기존 binding 경로가 이미 하는 일을 열거하며, 설계를 제안하지 않는다.

| 줄이거나 없앨 단계 | 관측 초과량 | 단계 소유 코드 | 판단 유형 |
|---|---:|---|---|
| Framework receive 조정 비용 | 1.028 | `raw_mesh_node_owner.cpp:3602`, `raw_route_port.cpp:459` | Framework 단계 제거 후 existing binding/Core `zlink_router_recv` 경로 사용 |
| Framework wire decode | 0.259 | `envelope_codec.cpp:1196` | raw binding에 없는 Framework envelope decode 단계 제거 |
| Framework route classification | 0.545 | `route_packet_dispatcher.cpp:203`, `mesh_record_dispatcher.cpp:32` | framework-only dispatcher 판정을 제거하고 existing native ROUTER 분류 사용 |
| mailbox enqueue/claim | 1.004 | `raw_mesh_node_owner.cpp:3286`, `service_mailbox.hpp:146` | Framework mailbox 단계 제거; binding pair의 기존 direct receive 경로로 대체 |
| dispatch/coroutine | 0.173 | `public_host_runtime.cpp:7503,8108`, `route_handler_invoker.cpp:10` | Framework dispatch 단계 제거; existing binding callback/receive loop로 대체 |
| handler 차이 | 0.247 | C++ benchmark target handler와 `mesh_node_host_service.cpp:3153` 경로 | Framework handler wrapper 차이를 줄이고 existing binding command-loop handler 사용 |
| completion/terminal | 0.129 | `application_job_queue.hpp:157`, `mesh_node_host_service.cpp:247` | 해당 Framework terminal 단계 제거; binding one-way completion 경로는 별도 terminal owner를 두지 않음 |
| target allocation | 0.734 | Framework receive/mailbox/dispatch 소유 객체 생성 지점; `service_mailbox.hpp:146` | surplus Framework allocation 제거; native message pool이 이미 소유하는 payload에는 기존 message ownership 사용 |
| target locks/wakeups | 0.690 | `raw_mesh_node_owner.cpp:3607` 및 mutex/kernel 표본 | 불필요한 Framework synchronization 비용 제거; binding의 기존 socket synchronization만 사용 |
| source submit 차이 | 0.497 | `mesh_node_runtime.cpp:4252`, `channel_runtime.cpp:1441` | Framework wrapper 차이를 줄이고 existing binding send submission 사용 |
| source encode 차이 | 0.488 | `envelope_codec.cpp:357,452,528,1316`, `service_wire_codec.cpp:3224` | Framework envelope 단계 제거가 가능한 raw send에서는 제거; existing binding message encoding 사용 |
| source binding/Core 진입 차이 | 0.677 | `mesh_node_runtime.cpp:4252`와 binding submit call | Framework-only 호출 비용 제거, existing binding send call 사용 |
| source allocation 차이 | 0.278 | send serializer·envelope assembly 경로 | surplus temporary allocation 제거; existing native message buffer 사용 |
| source lock/wakeup 차이 | 0.443 | send submit 및 kernel 표본 | 불필요한 Framework synchronization 비용 제거; 기존 binding send synchronization 사용 |
| 남은 미분류 비용 | 약 9.8 초과 중 최소 6.5 추가 감소 필요 | `std::__introsort_loop`는 `bench_stats_server.hpp:99`; 나머지는 CLR·libc·kernel·vdso 및 상위 표본 함수 | 샘플에서 이름을 분리할 수 없는 잔여 비용. 더 줄일 정확한 stage와 코드 위치는 이 profile만으로 결정되지 않음 |

## 중복 작업 점검

같은 payload 또는 header를 두 번 변환하는 표본은 확인하지 못했다. C++ envelope decode는 `decode_header_bytes`와 canonical reader 경로에서 관찰됐고, fallback SAX parser와 함께 한 메시지에서 두 번 decode하는 증거는 없다. `raw_mesh_node_owner.cpp:3292-3295`의 flow-header 확인은 tracing 조건부이며 이 benchmark에서는 비활성이다.

`service_mailbox_record_t`, `message_t`, `envelope_header_t`의 move constructor/destructor 표본은 소유권 이동 및 정리 비용이다. profile만으로 payload deep copy라고 판정하지 않았다. C++의 `std::__introsort_loop<double>` (`bench_stats_server.hpp:99`)과 .NET의 `GenericArraySortHelper<Int64>.PickPivotAndPartition`, `Int128::op_Division` 표본은 benchmark percentile/통계 업데이트 경로다. 이는 message당 payload 변환 중복이 아니다. 다른 중복 변환은 확인되지 않았다.

## 보존 자료

원시 C++ 및 .NET `perf.data`, perf stderr와 명령 JSON은 각각 위의 run 디렉터리에 있다. 분석 요약은 `analysis/cpp/`, `analysis/dotnet/`에 있다. .NET symbol map은 `dotnet-jitmaps/`에 보관했다. .NET 실행기 빌드 산출물은 `dotnet-bin/`에만 있으며, benchmark 저장소의 tracked 파일과 C++ binary는 변경하지 않았다. 테스트는 요청되지 않아 실행하지 않았다.
