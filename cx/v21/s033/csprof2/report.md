# Send-saturation 1 KiB CPU stage 귀속: #3868 두 번째 계측

## 1. 방법과 실행 결과

C++는 `csendb/build-main`의 main `a0056577c4`, 소스는 `csendb/main-source`를 사용했다. .NET은 `csprof/dotnet-bin`의 ratio040 `475305bebc` 산출물을 그대로 사용했다. 두 언어가 로드한 `libzlink`의 SHA-256은 동일했다(`native-provenance.json`). 저장소와 바이너리를 수정하거나 빌드하지 않았다. 실행 명령, 바이너리 SHA-256, 프로세스 map, affinity, 메모리 상태와 CPU counter를 원시 자료로 보존했다.

본 비교는 각 조합을 3회씩 unpaced로 실행했다. 실행 폴더의 `fp`는 기본 설정이며, C++ source의 DWARF 예외는 process별 command JSON에 기록했다. C++ paced 136,889 msg/s도 각 조합 3회 수집했다. Source CPU는 0–7, target은 8–15이며 모든 thread의 affinity를 수집 중 확인했다. 본 측정과 CPU를 많이 쓰는 사후 분석을 순차로 실행했다. 각 측정은 `flock -o /root/jobs/fx/cx/measure.lock` 안에서 수행하고, 현재 inode의 독점 잠금 소유자를 `/proc/locks`로 검증했다. 매초 `MemAvailable >= 12 GiB`를 확인했다.

각 회차는 30초 warmup 뒤 active 28초이며, active 안에서 22초 수집 timer를 사용했다. CPU 구간으로 자른 유효 표본의 실제 timestamp span은 최소 21.150초로, 모든 process가 20초 이상이다. 기본 명령은 `perf record -z -m 64M -N -k 1 -e cpu-clock -F 4999 --call-graph fp --sample-cpu -p PID`다. C++ source와 C++ binding target은 `--call-graph dwarf,8192`를 사용했다. FP pilot에서 native message 할당·IO cleanup의 caller가 끊겨 보수적인 분류의 source 미귀속이 10.6%였기 때문이다. C++ binding target도 FP에서 17–19%가 미귀속이어서 DWARF로 재수집했다. C++ Framework target과 .NET 두 process는 FP를 유지했다. CPU counter를 읽은 monotonic 구간 밖의 표본은 표에서 제외했다. .NET은 `DOTNET_PerfMapEnabled=1`로 생성한 process별 perf-map을 보존하고, 각 프레임 주소가 속한 method 구간을 직접 찾아 JIT 이름을 해석했다. 이 작업에는 다른 모델이나 sub-agent를 사용하지 않았다.

`pilot-*`는 스택 확인용이다. 초기 C++/.NET fp pilot은 사후 분석 프로세스와 시간이 겹쳐 성능 비교에서 제외했다. `cpp-framework-natural-dwarf-8192-r1`과 `dotnet-framework-natural-dwarf-8192-r1`은 native FP 체인을 확인한 pilot이며 본 표에 섞지 않았다. C++ binding의 FP 실행도 target의 귀속 품질이 부족해 본 표에서 제외하고 DWARF 실행으로 대체했다. 최종 실행 목록은 `selection.json`이다. DWARF 자료는 해당 회차의 CPU와 처리율로 직접 계산했다. paced C++ Framework 3회차에서 perf recorder 종료값 검사 실패가 1회 있었다. Source perf는 기록됐지만 target perf는 44 KiB에 가깝고 stderr가 비었다. 기존 수집기는 종료 코드를 저장하지 않아 원인은 미확정이다. 원시 자료는 `failed-cpp-framework-136889-fp-r3/`에 남겼으며, 정상 회차에 포함하지 않았다. 종료값 기록을 추가한 뒤 같은 조건의 유효 회차를 추가했다. 재수집 통과로 이 실패가 해결됐다고 판단하지 않았다.

아래 처리율은 표본 수집 구간의 target `received` 증가량으로 계산했다. CPU/message는 같은 구간의 프로세스 `/proc/PID/stat` user+system 증가량을 전달 건수로 나눴다. 각 회차의 user 표본은 실측 user CPU에, kernel 표본은 실측 system CPU에 각각 정규화했다. 세 회차를 합칠 때에는 stage CPU 시간의 합을 전달 건수의 합으로 나눴다. 따라서 stage 합계는 해당 프로세스의 실측 CPU/message와 같고, 서로 다른 회차의 중앙값을 더하지 않았다. HTTP 통계 읽기와 percentile 정렬 비용도 `metrics/bench`로 포함한다.

자연 처리율은 실행 조건이 같다는 뜻이며, 특히 binding 두 언어가 같은 절대 msg/s라는 뜻은 아니다. 표의 rate 차이를 함께 판단해야 한다. C++ Framework와 .NET Framework의 실측 rate 차이도 아래에 수치로 기록했다. F÷B는 자연 처리율의 Framework ÷ binding이며 CPU 비용의 역비율로 대신하지 않았다.

| 조합 | 수집 구간 rate 중앙값 | 전체 active rate 중앙값 | F÷B |
| --- | --- | --- | --- |
| C++ Framework | 158,824 | 162,584 | 0.272 |
| .NET Framework | 166,744 | 170,591 | 0.918 |
| C++ binding | 594,236 | 596,871 | 기준 |
| .NET binding | 175,755 | 185,856 | 기준 |

C++ Framework와 .NET Framework의 수집 구간 rate 중앙값 차이는 .NET 기준 -4.75%다. 자연 처리율의 차이가 남으므로 이 표를 완전히 같은 rate의 인과 비교로 해석하지 않았다.

| 회차 | 수집 초 | 전달 msg/s | source 완료 msg/s | 전달 건수 | target µs (user+system) | source µs (user+system) | target 표본 | source 표본 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| cpp-binding-136889-dwarf-8192-r1 | 22.506 | 136,582 | 136,889 | 3,073,920 | 2.349 (1.278+1.070) | 2.619 (1.438+1.181) | 31,894 | 36,702 |
| cpp-binding-136889-dwarf-8192-r2 | 22.487 | 136,571 | 136,888 | 3,071,135 | 2.318 (1.299+1.019) | 2.605 (1.495+1.110) | 31,609 | 36,471 |
| cpp-binding-136889-dwarf-8192-r3 | 22.502 | 136,579 | 136,888 | 3,073,280 | 2.307 (1.272+1.035) | 2.564 (1.425+1.139) | 31,572 | 36,188 |
| cpp-binding-174796-dwarf-8192-r1 | 22.520 | 174,416 | 174,796 | 3,927,797 | 2.307 (1.276+1.031) | 2.599 (1.477+1.123) | 40,332 | 46,839 |
| cpp-binding-175755-dwarf-8192-r2 | 22.498 | 175,326 | 175,753 | 3,944,448 | 2.330 (1.311+1.019) | 2.563 (1.443+1.121) | 40,730 | 46,461 |
| cpp-binding-178222-dwarf-8192-r3 | 22.525 | 177,773 | 178,222 | 4,004,291 | 2.357 (1.306+1.051) | 2.637 (1.473+1.164) | 42,007 | 48,335 |
| cpp-binding-natural-dwarf-8192-r1 | 22.670 | 594,236 | 595,327 | 13,471,497 | 2.796 (1.591+1.205) | 2.969 (1.667+1.302) | 171,722 | 189,022 |
| cpp-binding-natural-dwarf-8192-r2 | 22.665 | 574,266 | 575,309 | 13,015,897 | 2.583 (1.653+0.930) | 3.046 (1.691+1.354) | 150,496 | 186,982 |
| cpp-binding-natural-dwarf-8192-r3 | 22.640 | 599,335 | 600,550 | 13,568,932 | 2.829 (1.602+1.226) | 2.960 (1.677+1.284) | 176,199 | 189,310 |
| cpp-framework-136889-fp-r1 | 22.652 | 136,547 | 136,943 | 3,093,021 | 13.501 (9.602+3.899) | 9.224 (5.260+3.964) | 190,222 | 131,854 |
| cpp-framework-136889-fp-r2 | 22.693 | 136,452 | 136,890 | 3,096,507 | 13.380 (9.488+3.891) | 9.185 (5.280+3.904) | 188,403 | 131,678 |
| cpp-framework-136889-fp-r3 | 22.671 | 136,439 | 136,859 | 3,093,198 | 13.581 (9.560+4.022) | 9.295 (5.273+4.022) | 192,112 | 134,004 |
| cpp-framework-natural-fp-r1 | 22.829 | 158,824 | 159,333 | 3,625,850 | 15.834 (11.043+4.791) | 11.843 (6.834+5.008) | 266,128 | 203,108 |
| cpp-framework-natural-fp-r2 | 22.843 | 146,137 | 146,635 | 3,338,208 | 17.141 (11.896+5.245) | 12.962 (7.462+5.500) | 265,327 | 204,465 |
| cpp-framework-natural-fp-r3 | 22.722 | 178,660 | 179,268 | 4,059,586 | 13.770 (9.730+4.040) | 10.242 (5.882+4.360) | 258,402 | 194,856 |
| dotnet-binding-natural-fp-r1 | 23.257 | 174,796 | 175,651 | 4,065,216 | 5.899 (3.102+2.797) | 26.658 (17.192+9.466) | 106,737 | 470,358 |
| dotnet-binding-natural-fp-r2 | 22.272 | 175,755 | 176,644 | 3,914,397 | 5.866 (3.078+2.787) | 26.321 (17.144+9.176) | 102,043 | 446,332 |
| dotnet-binding-natural-fp-r3 | 22.347 | 178,222 | 179,183 | 3,982,762 | 5.850 (3.103+2.747) | 26.140 (17.327+8.813) | 103,906 | 451,595 |
| dotnet-framework-natural-fp-r1 | 23.189 | 162,890 | 163,848 | 3,777,270 | 15.506 (11.670+3.836) | 19.819 (15.421+4.397) | 271,076 | 350,560 |
| dotnet-framework-natural-fp-r2 | 23.161 | 166,744 | 167,715 | 3,861,942 | 15.383 (11.603+3.780) | 20.971 (16.391+4.581) | 276,470 | 381,117 |
| dotnet-framework-natural-fp-r3 | 23.193 | 168,213 | 169,283 | 3,901,391 | 15.338 (11.842+3.496) | 21.046 (16.435+4.611) | 277,851 | 386,045 |

이전 비프로파일 paced 기준도 보존한다. 136,889 msg/s에서 C++ Framework target 13.176, source 8.875 µs/message이고, C++ binding target 2.353, source 2.595 µs/message다. 아래 새 paced 표는 새 실행의 CPU에 정규화했으며, 이 이전 총 CPU와 새 표본 비율을 섞지 않았다.

## 2. Stage 규칙

각 표본의 leaf부터 caller 방향으로 이동해 최초로 일치하는 stage에 한 번만 귀속했다. `std::`, libc, allocator, 잠금, vDSO와 kernel 자체는 stage 경계가 아니다. 일반 message/value 생성·이동·해제와 `zlink_msg_size/data` accessor는 이를 호출한 stage에 귀속한다. Core `decoder_allocators.cpp`의 수신 buffer pool과 `pipe_receive.cpp`의 read admission은 수신 전용 owner이므로 Core receive 경계로 처리한다. 명시적인 terminal/permit/mailbox 정산 owner는 completion 경계다. 이름에 다른 stage의 type이 template 인자로 나타난 것만으로 그 stage에 귀속하지 않았다. 실제 정규식과 예외 순서는 `scripts/rules.py`에 있다.

| stage | C++ function patterns | .NET function patterns | 소유 파일 또는 경계 |
| --- | --- | --- | --- |
| Core/binding receive | `raw_route_port_t::{receive_if_ready,poll}`, `recv_whole_envelope`, `recv_envelope_t`, ROUTER receive, native decoder/read | `SocketKernel::{Receive*,TryReceive*,Poll*}`, `ReceiveRawBatch`, native ROUTER receive | C++ `runtime/backend/raw_route_port.cpp`, binding `Runtime/Native/native_receive.hpp`, Core `core/src/runtime/protocol/decoder_allocators.cpp`·`core/src/runtime/core/pipe_receive.cpp`; .NET `SocketKernel.ReceiveCore.cs`, `ZLinkManagedMeshNode.cs` |
| wire decode | `decode_header_bytes`, canonical reader/SAX, `envelope_codec_t::decode*`, `protocol::decode*`, protobuf Parse/Read | `ZLinkEnvelopeCodec::Decode*/Read*`, service/application payload codec decode, protobuf Parse/Merge/CreateMessage | C++ `runtime/messaging/envelope_codec.cpp`, `runtime/protocol/service_wire_codec.cpp`; .NET `ZLinkEnvelopeCodec.cs`, `ZLinkApplicationPayloadEnvelopeCodec.cs` |
| route classification | `receive_next`, topology peer, mesh/route packet dispatcher, handler resolve, message context construction | `ProcessReceived`, mesh route dispatcher, handler resolve | C++ `runtime/mesh/raw_mesh_node_owner.cpp`, `runtime/channels/route_packet_dispatcher.cpp`; .NET `ZLinkManagedMeshNode.cs`, `ZLinkMeshNodeRouteDispatcher.cs` |
| mailbox enqueue/claim | `service_mailbox_t` enqueue/claim, record movement, `enqueue_received`, claim batch handoff | `EnqueueOwned`, owned mailbox enqueue/claim | C++ `runtime/mesh/service_mailbox.cpp/.hpp`; .NET `ZLinkMeshOwnedMailbox.cs`, `ZLinkManagedMeshNode.cs` |
| dispatch/coroutine/pump | host application dispatch, route handler invoker, task/coroutine, DI scope, application job queue/execution owner | state lane, dispatch pump, handler dispatcher/invocation engine, execution pool, ThreadPool scheduling | C++ `runtime/stateful/public_host_runtime.cpp`, `runtime/channels/route_handler_invoker.cpp`; .NET `ZLinkStateLane.cs`, `ZLinkMeshDispatchPump.cs` |
| handler | `command_handler_t::handle`, raw `command_loop` body | command handler, raw `RunCommandRouter` body | C++/.NET benchmark server handler; 안쪽 metrics/protobuf/receive 프레임은 각자의 더 가까운 경계에 귀속한다. |
| completion/terminal | terminal owner, permit release, mailbox settle, completion signal | job record owner Dispose, lease release, retained record cleanup | C++ `runtime/dispatch/application_job_queue.hpp`, `runtime/mesh/mesh_node_host_service.cpp`; .NET `ZLinkApplicationJobQueue.cs` |
| metrics/bench | stats snapshot, latency record, benchmark payload creation, source run_tasks, HTTP handling and sorting | BenchServerMetrics, benchmark send worker bookkeeping, HTTP/Kestrel/statistics | C++ `bench_stats_server.hpp:99`의 정렬은 이 행이다. Framework 메시지 stage가 아니라 polling 비용이다. |
| send submit | route client/send call, channel/node submission and target lookup | route client/send call and managed send submission | C++ `runtime/channels/channel_runtime.cpp`, `runtime/mesh/mesh_node_runtime.cpp`; .NET route client and managed mesh send |
| encode | client call codec, envelope header writer, service wire encoder, serializer/protobuf write | envelope/service encoder, client call codec, protobuf serializer | C++ `runtime/messaging/envelope_codec.cpp`, `runtime/protocol/service_wire_codec.cpp`; .NET `ZLinkEnvelopeCodec.cs`, protobuf extension |
| binding/Core send | raw submit, binding send operation, native socket/pipe send and transport write | SocketKernel submit, send operation, native socket/pipe send and transport write | C++ `runtime/backend/raw_route_port.cpp`, binding send; .NET `SocketKernel` and send operation |
| unattributed | 확인 가능한 owner가 없다. | 확인 가능한 owner가 없다. 호출자가 없는 GC/finalizer도 여기에 남긴다. | allocator/kernel 이름만으로 Framework 호출자를 가정하지 않는다. |

비동기 Core IO에는 Framework caller가 같은 stack에 남지 않는다. 명시적인 read/decode·write/send 프레임이 있으면 그 방향을 사용하고, 방향이 없는 `libzlink` transport/pump 프레임은 target에서 receive, source에서 send 경계로 귀속했다. 이는 비동기 연결을 따라 Framework 함수를 복원한 것이 아니라 Core 경계를 process 역할로 분류한 것이다. stage 분석 JSON은 이 귀속을 `native-role`로 별도 표시한다. CLR GC처럼 Core 경계도 없는 stack은 이 방식으로 처리하지 않았다.

## 3. Target과 source의 CPU 비용

단위는 µs/message다. A는 stack에서 확인되는 user allocator CPU, L은 user lock/spin/wait CPU, K는 kernel leaf CPU다. A/L/K는 서로 겹치지 않는다. 나머지는 그 stage의 다른 user CPU다. Kernel futex와 allocator 문맥은 별도 교차 집계로 보존했다. 이는 할당 건수나 잠금 횟수가 아니다. Inlined managed allocation이나 이름이 없는 CLR helper는 A/L의 확정값에 포함할 수 없으므로, A/L은 식별 가능한 비용의 하한이다.

### target

| stage | C++ Framework | .NET Framework | C++ binding | .NET binding |
| --- | --- | --- | --- | --- |
| Core/binding receive | 6.902 | 5.680 | 2.424 | 4.479 |
| wire decode | 1.118 | 0.407 | 0.192 | 0.278 |
| route classification | 1.494 | 1.825 | 0.000 | 0.000 |
| mailbox enqueue/claim | 1.365 | 0.823 | 0.000 | 0.000 |
| dispatch/coroutine/pump | 3.153 | 4.423 | 0.000 | 0.001 |
| handler | 0.054 | 0.000 | 0.049 | 0.286 |
| completion/terminal | 0.454 | 0.698 | 0.000 | 0.000 |
| metrics/bench | 0.049 | 0.366 | 0.067 | 0.321 |
| send submit | 0.004 | 0.000 | 0.000 | 0.000 |
| encode | 0.009 | 0.000 | 0.000 | 0.000 |
| binding/Core send | 0.034 | 0.035 | 0.000 | 0.000 |
| unattributed | 0.834 | 1.152 | 0.005 | 0.508 |
| 합계 | 15.469 | 15.408 | 2.738 | 5.872 |

C++ Framework의 표본과 A/L/K.

| stage | 표본 n | A | L | K |
| --- | --- | --- | --- | --- |
| Core/binding receive | 332,623 | 0.272 | 0.050 | 4.341 |
| wire decode | 59,990 | 0.027 | 0.000 | 0.000 |
| route classification | 80,206 | 0.172 | 0.038 | 0.001 |
| mailbox enqueue/claim | 71,957 | 0.217 | 0.050 | 0.154 |
| dispatch/coroutine/pump | 168,982 | 0.389 | 0.114 | 0.031 |
| handler | 2,867 | 0.000 | 0.003 | 0.006 |
| completion/terminal | 24,101 | 0.034 | 0.004 | 0.031 |
| metrics/bench | 2,606 | 0.000 | 0.000 | 0.004 |
| send submit | 195 | 0.000 | 0.000 | 0.000 |
| encode | 475 | 0.000 | 0.000 | 0.000 |
| binding/Core send | 1,555 | 0.000 | 0.000 | 0.033 |
| unattributed | 44,300 | 0.083 | 0.022 | 0.050 |

미귀속 표본은 5.61%다. 회차별 비중은 `runs.csv`에 있다.

.NET Framework의 표본과 A/L/K.

| stage | 표본 n | A | L | K |
| --- | --- | --- | --- | --- |
| Core/binding receive | 294,295 | 0.297 | 0.029 | 2.654 |
| wire decode | 22,530 | 0.000 | 0.000 | 0.002 |
| route classification | 101,140 | 0.000 | 0.000 | 0.002 |
| mailbox enqueue/claim | 45,588 | 0.000 | 0.000 | 0.002 |
| dispatch/coroutine/pump | 238,838 | 0.000 | 0.510 | 0.816 |
| completion/terminal | 38,675 | 0.052 | 0.000 | 0.001 |
| metrics/bench | 20,064 | 0.000 | 0.000 | 0.032 |
| send submit | 3 | 0.000 | 0.000 | 0.000 |
| binding/Core send | 1,665 | 0.000 | 0.000 | 0.033 |
| unattributed | 62,599 | 0.028 | 0.023 | 0.162 |

미귀속 표본은 7.58%다. 회차별 비중은 `runs.csv`에 있다.

C++ binding의 표본과 A/L/K.

| stage | 표본 n | A | L | K |
| --- | --- | --- | --- | --- |
| Core/binding receive | 435,718 | 0.076 | 0.022 | 1.118 |
| wire decode | 38,591 | 0.028 | 0.000 | 0.000 |
| dispatch/coroutine/pump | 27 | 0.000 | 0.000 | 0.000 |
| handler | 9,746 | 0.020 | 0.004 | 0.000 |
| metrics/bench | 13,415 | 0.000 | 0.005 | 0.002 |
| unattributed | 920 | 0.001 | 0.000 | 0.003 |

미귀속 표본은 0.18%다. 회차별 비중은 `runs.csv`에 있다.

.NET binding의 표본과 A/L/K.

| stage | 표본 n | A | L | K |
| --- | --- | --- | --- | --- |
| Core/binding receive | 232,605 | 0.078 | 0.032 | 2.557 |
| wire decode | 16,545 | 0.000 | 0.000 | 0.000 |
| dispatch/coroutine/pump | 69 | 0.000 | 0.000 | 0.001 |
| handler | 17,015 | 0.000 | 0.000 | 0.000 |
| metrics/bench | 18,666 | 0.000 | 0.000 | 0.035 |
| unattributed | 27,786 | 0.017 | 0.025 | 0.184 |

미귀속 표본은 8.89%다. 회차별 비중은 `runs.csv`에 있다.

### source

| stage | C++ Framework | .NET Framework | C++ binding | .NET binding |
| --- | --- | --- | --- | --- |
| Core/binding receive | 0.069 | 0.033 | 0.000 | 0.000 |
| wire decode | 0.000 | 0.000 | 0.000 | 0.000 |
| route classification | 0.110 | 0.000 | 0.000 | 0.000 |
| mailbox enqueue/claim | 0.000 | 0.000 | 0.000 | 0.000 |
| dispatch/coroutine/pump | 0.311 | 0.589 | 0.000 | 15.688 |
| handler | 0.000 | 0.000 | 0.000 | 0.000 |
| completion/terminal | 0.007 | 0.222 | 0.000 | 0.000 |
| metrics/bench | 0.714 | 4.072 | 0.754 | 3.651 |
| send submit | 1.056 | 7.003 | 0.000 | 0.000 |
| encode | 1.845 | 1.998 | 0.043 | 0.133 |
| binding/Core send | 7.282 | 5.333 | 2.185 | 5.279 |
| unattributed | 0.199 | 1.370 | 0.008 | 1.624 |
| 합계 | 11.592 | 20.619 | 2.991 | 26.375 |

C++ Framework의 표본과 A/L/K.

| stage | 표본 n | A | L | K |
| --- | --- | --- | --- | --- |
| Core/binding receive | 3,542 | 0.007 | 0.004 | 0.038 |
| route classification | 5,779 | 0.000 | 0.020 | 0.000 |
| mailbox enqueue/claim | 19 | 0.000 | 0.000 | 0.000 |
| dispatch/coroutine/pump | 16,364 | 0.102 | 0.007 | 0.011 |
| completion/terminal | 361 | 0.000 | 0.001 | 0.005 |
| metrics/bench | 37,646 | 0.087 | 0.006 | 0.002 |
| send submit | 55,616 | 0.122 | 0.066 | 0.025 |
| encode | 97,196 | 0.334 | 0.023 | 0.050 |
| binding/Core send | 375,600 | 0.220 | 0.114 | 4.687 |
| unattributed | 10,306 | 0.022 | 0.018 | 0.101 |

미귀속 표본은 1.71%다. 회차별 비중은 `runs.csv`에 있다.

.NET Framework의 표본과 A/L/K.

| stage | 표본 n | A | L | K |
| --- | --- | --- | --- | --- |
| Core/binding receive | 1,579 | 0.000 | 0.000 | 0.030 |
| route classification | 4 | 0.000 | 0.000 | 0.000 |
| dispatch/coroutine/pump | 30,481 | 0.002 | 0.186 | 0.304 |
| completion/terminal | 12,458 | 0.178 | 0.000 | 0.000 |
| metrics/bench | 227,771 | 0.170 | 0.003 | 0.033 |
| send submit | 389,181 | 0.141 | 0.096 | 0.377 |
| encode | 111,671 | 0.281 | 0.003 | 0.020 |
| binding/Core send | 269,028 | 0.056 | 0.021 | 3.624 |
| unattributed | 75,549 | 0.497 | 0.045 | 0.143 |

미귀속 표본은 6.76%다. 회차별 비중은 `runs.csv`에 있다.

C++ binding의 표본과 A/L/K.

| stage | 표본 n | A | L | K |
| --- | --- | --- | --- | --- |
| Core/binding receive | 79 | 0.000 | 0.000 | 0.000 |
| dispatch/coroutine/pump | 35 | 0.000 | 0.000 | 0.000 |
| metrics/bench | 143,315 | 0.267 | 0.032 | 0.196 |
| encode | 8,272 | 0.002 | 0.000 | 0.000 |
| binding/Core send | 412,110 | 0.143 | 0.045 | 1.113 |
| unattributed | 1,503 | 0.003 | 0.000 | 0.003 |

미귀속 표본은 0.27%다. 회차별 비중은 `runs.csv`에 있다.

.NET binding의 표본과 A/L/K.

| stage | 표본 n | A | L | K |
| --- | --- | --- | --- | --- |
| Core/binding receive | 16 | 0.000 | 0.000 | 0.000 |
| dispatch/coroutine/pump | 828,193 | 0.095 | 5.607 | 4.800 |
| metrics/bench | 217,017 | 0.118 | 0.005 | 0.027 |
| encode | 7,947 | 0.000 | 0.000 | 0.000 |
| binding/Core send | 229,576 | 0.040 | 0.016 | 3.820 |
| unattributed | 85,536 | 0.497 | 0.041 | 0.507 |

미귀속 표본은 6.25%다. 회차별 비중은 `runs.csv`에 있다.

C++ paced 136,889 msg/s의 새 실행.

| target stage | Framework | binding |
| --- | --- | --- |
| Core/binding receive | 5.677 | 2.079 |
| wire decode | 1.042 | 0.119 |
| route classification | 1.359 | 0.000 |
| mailbox enqueue/claim | 1.151 | 0.000 |
| dispatch/coroutine/pump | 2.968 | 0.000 |
| handler | 0.056 | 0.030 |
| completion/terminal | 0.452 | 0.000 |
| metrics/bench | 0.054 | 0.089 |
| send submit | 0.004 | 0.000 |
| encode | 0.008 | 0.000 |
| binding/Core send | 0.001 | 0.000 |
| unattributed | 0.715 | 0.007 |
| 합계 | 13.487 | 2.325 |

| source stage | Framework | binding |
| --- | --- | --- |
| Core/binding receive | 0.002 | 0.000 |
| wire decode | 0.000 | 0.000 |
| route classification | 0.090 | 0.000 |
| mailbox enqueue/claim | 0.000 | 0.000 |
| dispatch/coroutine/pump | 0.245 | 0.000 |
| handler | 0.000 | 0.000 |
| completion/terminal | 0.000 | 0.000 |
| metrics/bench | 0.584 | 0.563 |
| send submit | 0.859 | 0.000 |
| encode | 1.496 | 0.045 |
| binding/Core send | 5.896 | 1.979 |
| unattributed | 0.061 | 0.009 |
| 합계 | 9.234 | 2.596 |

Paced stage별 표본 수와 A/L/K는 `stages-per-run.csv`에 모두 보존했다.

## 4. C++와 .NET의 비교와 상위 호출 경로

동일 책임에 .NET counterpart가 없을 때만 C++ only로 판정했다. C++ 비용이 세 회차 모두 .NET의 최대 관측값보다 0.1 µs/message 이상 높을 때 costlier로 표시했다. 나머지는 comparable로 표시하되, C++가 더 낮거나 회차 변동이 겹치는 경우를 구분했다. Inlining과 복합 함수 때문에 wire decode/route classification 및 dispatch/completion의 경계에는 구현별 차이가 남을 수 있다. 합산값도 함께 제시한다.

target의 Framework 비교.

| stage | C++ | .NET | C++−.NET | 판정 |
| --- | --- | --- | --- | --- |
| Core/binding receive | 6.902 | 5.680 | +1.222 | same stage but costlier (+1.222) |
| wire decode | 1.118 | 0.407 | +0.711 | same stage but costlier (+0.711) |
| route classification | 1.494 | 1.825 | -0.331 | comparable; C++가 0.331 낮음 |
| mailbox enqueue/claim | 1.365 | 0.823 | +0.542 | same stage but costlier (+0.542) |
| dispatch/coroutine/pump | 3.153 | 4.423 | -1.270 | comparable; C++가 1.270 낮음 |
| handler | 0.054 | 0.000 | +0.054 | comparable; 0.1 이하 또는 회차 변동 중첩 |
| completion/terminal | 0.454 | 0.698 | -0.244 | comparable; C++가 0.244 낮음 |
| metrics/bench | 0.049 | 0.366 | -0.317 | comparable; C++가 0.317 낮음 |
| send submit | 0.004 | 0.000 | +0.004 | comparable; 0.1 이하 또는 회차 변동 중첩 |
| encode | 0.009 | 0.000 | +0.009 | comparable; 0.1 이하 또는 회차 변동 중첩 |
| binding/Core send | 0.034 | 0.035 | -0.000 | comparable; 0.1 이하 또는 회차 변동 중첩 |

wire decode + route classification 합계: C++ 2.612, .NET 2.232, 차이 +0.380 µs/message.
dispatch/coroutine/pump + completion/terminal 합계: C++ 3.607, .NET 5.121, 차이 -1.514 µs/message.

source의 Framework 비교.

| stage | C++ | .NET | C++−.NET | 판정 |
| --- | --- | --- | --- | --- |
| Core/binding receive | 0.069 | 0.033 | +0.036 | comparable; 0.1 이하 또는 회차 변동 중첩 |
| wire decode | 0.000 | 0.000 | +0.000 | comparable; 0.1 이하 또는 회차 변동 중첩 |
| route classification | 0.110 | 0.000 | +0.110 | comparable; 0.1 이하 또는 회차 변동 중첩 |
| mailbox enqueue/claim | 0.000 | 0.000 | +0.000 | comparable; 0.1 이하 또는 회차 변동 중첩 |
| dispatch/coroutine/pump | 0.311 | 0.589 | -0.278 | comparable; C++가 0.278 낮음 |
| handler | 0.000 | 0.000 | +0.000 | comparable; 0.1 이하 또는 회차 변동 중첩 |
| completion/terminal | 0.007 | 0.222 | -0.215 | comparable; C++가 0.215 낮음 |
| metrics/bench | 0.714 | 4.072 | -3.358 | comparable; C++가 3.358 낮음 |
| send submit | 1.056 | 7.003 | -5.947 | comparable; C++가 5.947 낮음 |
| encode | 1.845 | 1.998 | -0.152 | comparable; C++가 0.152 낮음 |
| binding/Core send | 7.282 | 5.333 | +1.949 | same stage but costlier (+1.949) |

wire decode + route classification 합계: C++ 0.110, .NET 0.000, 차이 +0.110 µs/message.
dispatch/coroutine/pump + completion/terminal 합계: C++ 0.318, .NET 0.811, 차이 -0.493 µs/message.

C++ only로 판정한 stage는 없다. 같은 기능이 .NET에서 inline되거나 더 큰 함수에 포함될 수 있으므로, 0 표본을 기능 부재로 해석하지 않았다. 위 판정은 관측한 함수 경계의 CPU 차이다. 다른 rate에서의 순수 구현 차이나 개별 경로를 제거했을 때의 개선량을 뜻하지 않는다.

### 상위 호출 경로

화살표는 caller → leaf 방향이다. 비용은 각 언어에서 전달 건수로 정규화했다. Wire decode/mailbox는 대응하는 method 이름이 달라 C++ 경로 자체 비용을 제시한다. Core receive/send는 leaf부터 최초 stage 경계까지 동일한 native 함수열을 묶어 두 언어의 관측 차액이 큰 세 경로를 골랐다. Native 함수열의 .NET 0은 동일 함수열을 관측하지 못했다는 뜻이며 대응 기능이 없다는 뜻이 아니다. 양수·음수 native 경로를 모두 합한 값은 stage 차액과 일치한다. 원본의 가장 뜨거운 전체 C++ 스택도 `top-chains.json`에 따로 보존했다.

행 번호는 C++ 실행 파일의 DWARF PC에서 얻은 대표 위치 또는 해당 함수의 정의·호출 지점이다. 한 함수열의 모든 PC가 그 행 하나에서 실행됐다는 뜻은 아니다. Core shared library에는 line debug 정보가 없어 `main-source/core/src`의 해당 owner 정의·호출 지점으로 연결했다. `core/include`는 읽지 않았다.

target Core/binding receive: stage 관측 초과 +1.222 µs/message.

| 순위 | 호출 경로 | main-source 위치 | C++ µs | .NET 동일 함수열 µs | 관측 차액 | 표본 C++/.NET |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | recv_whole_envelope | `bindings/cpp/src/Runtime/Native/native_receive.hpp:177` | 0.1112 | 0.0000 | +0.1112 | 5,970/0 |
| 2 | recv_envelope_t::receive_native_parts → new → malloc → _int_malloc | `bindings/cpp/src/Runtime/Native/native_receive.hpp:69` | 0.0884 | 0.0000 | +0.0884 | 4,746/0 |
| 3 | socket_t::receive_impl | `bindings/cpp/src/Runtime/Sockets/socket.cpp:358` | 0.0577 | 0.0000 | +0.0577 | 3,097/0 |

source binding/Core send: stage 관측 초과 +1.949 µs/message.

| 순위 | 호출 경로 | main-source 위치 | C++ µs | .NET 동일 함수열 µs | 관측 차액 | 표본 C++/.NET |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | encoder_base_t::encode → __memmove_avx_unaligned_erms | `core/src/runtime/engine/asio/asio_engine.cpp:1332` → `prepare_output_buffer` / native encoder | 0.0966 | 0.0000 | +0.0966 | 5,093/0 |
| 2 | boost::asio::detail::reactive_socket_send_op_base::do_perform → __send → … → tcp_sendmsg → lock_sock_nested → _raw_spin_lock_bh → native_queued_spin_lock_slowpath | `core/src/runtime/transports/tcp/tcp_transport.cpp:519`, `core/src/runtime/engine/asio/asio_engine.cpp:754` | 0.1393 | 0.0490 | +0.0904 | 7,100/2,341 |
| 3 | router_socket_t::send | `bindings/cpp/src/Runtime/Sockets/router.cpp:22` | 0.0767 | 0.0000 | +0.0767 | 4,042/0 |

target wire decode: stage 관측 초과 +0.711 µs/message.

| 순위 | C++ 호출 경로 | main-source 위치 | C++ 경로 µs | 표본 |
| --- | --- | --- | --- | --- |
| 1 | decode_mesh_record_header_impl → messaging::envelope_codec_t::decode_header_view → messaging::decode_header_bytes → __memcmp_avx2_movbe | `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:1205`, inline string 비교 `:632` | 0.0914 | 4,905 |
| 2 | messaging::envelope_codec_t::decode_header_view → messaging::decode_header_bytes → messaging::canonical_header_reader_t::string | `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:662`, `:1205` | 0.0859 | 4,612 |
| 3 | decode_mesh_record_header_impl → messaging::envelope_codec_t::decode_header_view → messaging::decode_header_bytes | `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:1204` | 0.0856 | 4,596 |

target mailbox enqueue/claim: stage 관측 초과 +0.542 µs/message.

| 순위 | C++ 호출 경로 | main-source 위치 | C++ 경로 µs | 표본 |
| --- | --- | --- | --- | --- |
| 1 | host::public_host_runtime_t::dispatch_ready_impl → mesh::raw_mesh_node_owner_t::receive_next → mesh::raw_mesh_node_owner_t::enqueue_received | `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:3291`, caller `:4237` | 0.0758 | 4,069 |
| 2 | mesh::raw_mesh_node_owner_t::receive_next → mesh::raw_mesh_node_owner_t::enqueue_received → mesh::service_mailbox_t::try_enqueue → new → malloc → _int_malloc | `framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:102`, caller `raw_mesh_node_owner.cpp:3353` | 0.0652 | 3,501 |
| 3 | mesh::raw_mesh_node_owner_t::enqueue_received → mesh::service_mailbox_t::try_enqueue → mesh::service_mailbox_record_t::service_mailbox_record_t | `framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.hpp:146`, caller `service_mailbox.cpp:102` | 0.0633 | 3,398 |

C++는 매번 canonical JSON header를 읽고 검증하는 경로가 관찰됐다. .NET의 실제 build commit `475305bebc`에서도 `ZLinkEnvelopeCodec.cs:377`의 decoded-header cache와 `:188`의 simple-header byte cache를 확인했다. 이 command benchmark는 correlation/deadline/flow가 없는 반복 header를 사용한다. Cache 경로의 존재와 C++ header reader 표본은 wire decode 차이를 설명하는 근거이며, profiler가 cache hit 횟수를 계측한 것은 아니다.

Source route classification도 합산값은 +0.110 µs 높지만 모든 회차에서 0.1 µs 초과 기준을 충족하지 않아 comparable로 판정했다. 관측 양수인 이 행의 상위 C++ 경로도 남긴다.

| 순위 | leaf ← caller | 소유 위치와 호출 지점 | C++ 경로 µs | 표본 |
| --- | --- | --- | --- | --- |
| 1 | zlink::framework::runtime::mesh::service_topology_registry_t::peer ← zlink::framework::runtime::mesh::raw_mesh_node_owner_t::classify_node_target ← zlink::framework::detail::mesh_node_runtime_t::send_to_node | `framework/languages/cpp/framework/src/runtime/mesh/service_topology_registry.cpp:515`; 호출 경로 `mesh_node_runtime.cpp:4194`, `raw_mesh_node_owner.cpp:1764` | 0.0253 | 1334 |
| 2 | zlink::framework::runtime::mesh::service_topology_registry_t::peer ← zlink::framework::runtime::mesh::raw_mesh_node_owner_t::send_with_header_result ← zlink::framework::runtime::mesh::raw_mesh_node_owner_t::send_to_node_result | `framework/languages/cpp/framework/src/runtime/mesh/service_topology_registry.cpp:515`; 호출 경로 `mesh_node_runtime.cpp:4194`, `raw_mesh_node_owner.cpp:1764` | 0.0146 | 769 |
| 3 | __memcmp_avx2_movbe ← zlink::framework::runtime::mesh::service_topology_registry_t::peer ← zlink::framework::runtime::mesh::raw_mesh_node_owner_t::classify_node_target ← zlink::framework::detail::mesh_node_runtime_t::send_to_node | `framework/languages/cpp/framework/src/runtime/mesh/service_topology_registry.cpp:515`; 호출 경로 `mesh_node_runtime.cpp:4194`, `raw_mesh_node_owner.cpp:1764` | 0.0102 | 538 |

그 밖의 양수 차이는 0.1 µs/message 미만이다. 이 값들까지 포함한 모든 stage의 상위 함수열은 `top-chains.json`에 있으며, 작은 양수 차이를 개선 대상이라는 근거로 사용하지 않았다.

작은 관측 양수 차이의 경로도 아래에 남긴다. 이 표는 비용 초과가 확정됐다는 뜻이 아니다. Target encode는 전체 함수열이 두 개만 관측돼 두 경로만 기재했다. Source mailbox의 전체 표본 19개는 차이를 해석하기에 부족하다.

| process | stage | 순위 | caller → leaf | main-source 대표 위치 | C++ µs | 표본 |
| --- | --- | --- | --- | --- | --- | --- |
| target | handler | 1 | std::_Function_handler::_M_invoke → zlink::framework::mesh_channel_server_builder_t::add_handler → command_handler_t::handle → std::chrono::_V2::steady_clock::now → clock_gettime@@GLIBC_2.17 → [unknown] | `framework/bench/grpc/cpp/common/bench_stats_server.hpp:57` | 0.031837 | 1709 |
| target | handler | 2 | std::_Function_handler::_M_invoke → zlink::framework::mesh_channel_server_builder_t::add_handler → command_handler_t::handle | `framework/bench/grpc/cpp/common/bench_stats_server.hpp:62` | 0.007656 | 411 |
| target | handler | 3 | std::_Function_handler::_M_invoke → zlink::framework::mesh_channel_server_builder_t::add_handler → command_handler_t::handle → pthread_mutex_lock@@GLIBC_2.2.5 | `framework/bench/grpc/cpp/common/bench_stats_server.hpp:47` | 0.003261 | 175 |
| target | send submit | 1 | zlink::framework::runtime::mesh_node_host_service_t::dispatch_application → zlink::framework::detail::spot_node_runtime_t::dispatch_mesh_record → zlink::framework::route_client_t::route_client_t | `framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:1436` | 0.001751 | 94 |
| target | send submit | 2 | zlink::framework::runtime::mesh_node_host_service_t::dispatch_application → zlink::framework::detail::spot_node_runtime_t::dispatch_mesh_record → zlink::framework::route_client_t::~route_client_t | `framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:1434` | 0.001081 | 58 |
| target | send submit | 3 | std::_Function_handler::_M_invoke → zlink::framework::runtime::mesh_node_host_service_t::dispatch_application → zlink::framework::route_client_t::~route_client_t | `framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:1434` | 0.000801 | 43 |
| target | encode | 1 | zlink::framework::runtime::messaging::anonymous_namespace::header_sax_t::finish → zlink::framework::runtime::messaging::anonymous_namespace::write_metadata → zlink::framework::runtime::messaging::anonymous_namespace::header_writer_t::append | `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:245` | 0.006853 | 368 |
| target | encode | 2 | zlink::framework::runtime::messaging::anonymous_namespace::decode_header_bytes → zlink::framework::runtime::messaging::anonymous_namespace::header_sax_t::finish → zlink::framework::runtime::messaging::anonymous_namespace::header_writer_t::append | `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:241` | 0.001993 | 107 |
| source | Core/binding receive | 1 | zlink::framework::runtime::mesh_node_host_service_t::node_receive_role_t::rotate → zlink::framework::runtime::mesh::raw_mesh_node_owner_t::wait_for_hosted_activity → … → wake_up_q → try_to_wake_up → _raw_spin_unlock_irqrestore | `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:422` | 0.011536 | 587 |
| source | Core/binding receive | 2 | zlink::framework::runtime::mesh_node_host_service_t::node_receive_role_t::rotate → zlink::framework::runtime::mesh::raw_mesh_node_owner_t::wait_for_hosted_activity → zlink::framework::detail::backend::raw_route_port_t::poll → zlink::poller_t::impl::wait → zlink::detail::completion_owner_t::drain_impl | `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:422` | 0.002050 | 108 |
| source | Core/binding receive | 3 | zlink::framework::runtime::mesh_node_host_service_t::node_receive_role_t::rotate → zlink::framework::runtime::mesh::raw_mesh_node_owner_t::wait_for_hosted_activity → … → zlink::framework::runtime::foundation::anonymous_namespace::notify → zlink::framework::runtime::foundation::anonymous_namespace::post_completions → zlink::framework::detail::dispatch_completion | `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:422` | 0.001101 | 58 |
| source | mailbox enqueue/claim | 1 | zlink::framework::runtime::host::public_host_runtime_t::dispatch_user_spot_operations → zlink::framework::runtime::host::public_host_runtime_t::dispatch_user_spot_operations → zlink::framework::runtime::mesh::service_mailbox_t::try_claim | `framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:123` | 0.000114 | 6 |
| source | mailbox enqueue/claim | 2 | zlink::framework::runtime::mesh_node_host_service_t::node_receive_role_t::run → zlink::framework::runtime::mesh_node_host_service_t::node_receive_role_t::rotate → zlink::framework::runtime::mesh::service_mailbox_t::take_application_receive_turn[abi:cxx11] | `framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:498` | 0.000057 | 3 |
| source | mailbox enqueue/claim | 3 | zlink::framework::runtime::mesh_node_host_service_t::node_receive_role_t::run → zlink::framework::runtime::mesh_node_host_service_t::node_receive_role_t::rotate → zlink::framework::runtime::mesh::service_mailbox_t::begin_application_receive_turn → pthread_mutex_lock@@GLIBC_2.2.5 | `framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:487` | 0.000057 | 3 |



### Binding의 rate를 맞춘 대조

C++ binding의 자연 rate는 .NET보다 약 3.4배 높았다. 추가 C++ binding 3회는 각 .NET binding 수집 구간의 전달률에 맞춰 pace했다. 실제 target rate 차이는 약 0.2–0.3%이며, 아래 비용은 각 실행 자체의 CPU로 계산했다. Framework 두 언어의 자연 rate 차이 4.75%는 그대로 남는다.

| 회차 | .NET 전달 msg/s | C++ 전달 msg/s | rate 차이 |
| --- | --- | --- | --- |
| 1 | 174,795.6 | 174,416.1 | -0.217% |
| 2 | 175,754.6 | 175,326.3 | -0.244% |
| 3 | 178,221.8 | 177,773.3 | -0.252% |

target의 binding 비용.

| stage | rate 맞춘 C++ | .NET | C++−.NET | C++ 표본 | C++ A/L/K |
| --- | --- | --- | --- | --- | --- |
| Core/binding receive | 2.091 | 4.479 | -2.387 | 108755 | 0.051/0.016/1.020 |
| wire decode | 0.119 | 0.278 | -0.158 | 7227 | 0.016/0.000/0.000 |
| route classification | 0.000 | 0.000 | +0.000 | 0 | 0.000/0.000/0.000 |
| mailbox enqueue/claim | 0.000 | 0.000 | +0.000 | 0 | 0.000/0.000/0.000 |
| dispatch/coroutine/pump | 0.000 | 0.001 | -0.001 | 9 | 0.000/0.000/0.000 |
| handler | 0.031 | 0.286 | -0.254 | 1897 | 0.014/0.003/0.000 |
| completion/terminal | 0.000 | 0.000 | +0.000 | 0 | 0.000/0.000/0.000 |
| metrics/bench | 0.084 | 0.321 | -0.237 | 4932 | 0.000/0.004/0.010 |
| send submit | 0.000 | 0.000 | +0.000 | 0 | 0.000/0.000/0.000 |
| encode | 0.000 | 0.000 | +0.000 | 0 | 0.000/0.000/0.000 |
| binding/Core send | 0.000 | 0.000 | +0.000 | 0 | 0.000/0.000/0.000 |
| unattributed | 0.005 | 0.508 | -0.502 | 249 | 0.001/0.000/0.004 |

.NET의 표본과 A/L/K는 위 natural binding 표와 같다.

source의 binding 비용.

| stage | rate 맞춘 C++ | .NET | C++−.NET | C++ 표본 | C++ A/L/K |
| --- | --- | --- | --- | --- | --- |
| Core/binding receive | 0.000 | 0.000 | -0.000 | 0 | 0.000/0.000/0.000 |
| wire decode | 0.000 | 0.000 | +0.000 | 0 | 0.000/0.000/0.000 |
| route classification | 0.000 | 0.000 | +0.000 | 0 | 0.000/0.000/0.000 |
| mailbox enqueue/claim | 0.000 | 0.000 | +0.000 | 0 | 0.000/0.000/0.000 |
| dispatch/coroutine/pump | 0.000 | 15.688 | -15.687 | 7 | 0.000/0.000/0.000 |
| handler | 0.000 | 0.000 | +0.000 | 0 | 0.000/0.000/0.000 |
| completion/terminal | 0.000 | 0.000 | +0.000 | 0 | 0.000/0.000/0.000 |
| metrics/bench | 0.573 | 3.651 | -3.078 | 31818 | 0.212/0.026/0.115 |
| send submit | 0.000 | 0.000 | +0.000 | 0 | 0.000/0.000/0.000 |
| encode | 0.041 | 0.133 | -0.092 | 2334 | 0.003/0.000/0.000 |
| binding/Core send | 1.979 | 5.279 | -3.300 | 107097 | 0.130/0.041/1.017 |
| unattributed | 0.007 | 1.624 | -1.617 | 379 | 0.002/0.000/0.003 |

.NET의 표본과 A/L/K는 위 natural binding 표와 같다.

### 미귀속과 확인 범위

최종 42개 process의 미귀속 최대값은 9.25%다. 총 표본은 7,517,520개이며, 실제 표본 timestamp 구간은 최소 21.150초였다. Lost sample은 42개 원시 perf report 모두 0이었다. CPU 합계·차원별 합계·affinity·동일 분류기 hash 검사를 통과했다. 측정 중 최소 MemAvailable은 20.67 GiB였고, 23개 실행 파일/assembly SHA-256이 처음과 같았다.

미귀속의 주된 원인은 호출자가 끊긴 native helper, 이름 없는 CLR GC/finalizer 함수와 user frame이 없는 kernel stack이다. GC나 libc만 보고 특정 Framework stage를 추정하지 않았다. 다음은 자연 Framework process의 미귀속 상위 세 경로이며, 동일 전체 함수열을 기준으로 집계했다. 모든 process의 상위 미귀속 경로는 각 `analysis/*-stages.json`에 있다.

| process | thread | leaf ← caller (일부) | 표본 | µs/message 또는 기준 |
| --- | --- | --- | --- | --- |
| C++ target | bench_cpp_frame | __memmove_avx_unaligned_erms ← [unknown] ← [unknown] | 3279 | 0.0611 |
| C++ target | bench_cpp_frame | zlink::msg_t::close ← [unknown] | 2370 | 0.0441 |
| C++ target | bench_cpp_frame | zlink_msg_close ← [unknown] | 1551 | 0.0289 |
| C++ source | bench_cpp_clien | _raw_spin_unlock_irqrestore ← try_to_wake_up ← wake_up_q ← futex_wake ← do_futex | 900 | 0.0177 |
| C++ source | bench_cpp_clien | _raw_spin_unlock_irqrestore ← hrtimer_start_range_ns ← do_nanosleep ← hrtimer_nanosleep ← common_nsleep | 835 | 0.0164 |
| C++ source | bench_cpp_clien | zlink::framework::app_t::run ← std::thread::_State_impl::_M_run ← [unknown] ← start_thread ← clone3 | 267 | 0.0051 |
| .NET target r2 | .NET Server GC | [unknown] ← [unknown] ← [unknown] ← [unknown] ← [unknown] | 5705 | 회차 raw 표본 |
| .NET target r2 | .NET Server GC | [unknown] ← [unknown] ← [unknown] ← [unknown] ← [unknown] | 3979 | 회차 raw 표본 |
| .NET target r2 | .NET Server GC | [unknown] ← [unknown] ← [unknown] ← [unknown] ← [unknown] | 1022 | 회차 raw 표본 |
| .NET source r2 | .NET Finalizer | [unknown] ← [unknown] ← [unknown] ← [unknown] ← [unknown] | 3273 | 회차 raw 표본 |
| .NET source r2 | .NET TP Worker | _int_malloc ← malloc ← zlink::msg_t::init_size | 2972 | 회차 raw 표본 |
| .NET source r2 | ZLINKbg/IO/0 | __memmove_avx_unaligned_erms ← [unknown] ← zlink::zmp_encoder_t::~zmp_encoder_t ← [unknown] | 2231 | 회차 raw 표본 |

두 언어의 미귀속 CPU를 특정 stage에 모두 넣을 수 있다고 가정하면 stage 차액의 범위가 넓어진다. 아래는 .NET 미귀속 전부를 해당 stage에 넣는 극단적인 하한이다. 실제 GC 원인이나 사라진 caller가 그 stage였다는 뜻은 아니다.

| process | stage | 관측 차액 | .NET 미귀속 전체 | 극단적 차액 하한 |
| --- | --- | --- | --- | --- |
| target | Core/binding receive | +1.222 | 1.152 | +0.070 |
| target | wire decode | +0.711 | 1.152 | -0.441 |
| target | mailbox enqueue/claim | +0.542 | 1.152 | -0.610 |
| source | binding/Core send | +1.949 | 1.370 | +0.579 |

Stage의 함수 경계·inlining과 남은 rate 차이 때문에 관측 차액을 곧바로 제거 가능한 비용으로 해석하지 않았다. Source의 native encoder memcpy는 Core wire encoding 경로이며, 그것만으로 Framework에서 같은 payload를 두 번 복사했다고 판정하지 않았다.


## 5. 중복 작업 점검

아래는 소스 코드의 실제 호출 경로를 함께 확인한 결과다. 변환·표현 왕복 후보 3개와 별도 반복 조회 1개를 기록했으며, 수정한 항목은 0개다. CPU 표본은 호출 횟수 계측이 아니므로 두 번의 동작을 각각 몇 ns로 나누지는 않았다. 구현 수정과 리팩토링은 수행하지 않았다.

| 대상 | 확인한 동작 | main-source 위치 |
| --- | --- | --- |
| C++ source header | `write_header`를 크기 계산과 실제 쓰기에 두 번 호출한다. Header traversal은 2회이고 실제 byte 쓰기는 1회다. | `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:504`, `:509`; prepared payload 경로도 `:541`, `:548` |
| C++ target service command prefix | Ingress에서 읽은 service command header를 mailbox dispatch에서 다시 읽는다. Stateful 검사 경로가 활성화될 때의 추가 decode는 별도 조건이다. | `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:3644`, `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:8323`; 조건부 `:8150` |
| C++ target routing id | 수신한 typed routing id를 vector로 복사한 뒤 host dispatch에서 typed routing id로 다시 구성한다. 같은 바이트의 표현 왕복이다. | `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:481`, `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:8379`; binding `Contracts/Core/routing_id.hpp:300`, `:369` |
| C++ source topology lookup | Known target 분류와 send admission에서 같은 peer를 각각 조회한다. 조회 목적이 다르고 반환은 공유 포인터이므로 descriptor deep copy로 세지 않았다. | `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:4194`, `raw_mesh_node_owner.cpp:4373`, `:1764`, `service_topology_registry.cpp:515` |
| payload와 message 이동 | `encoded_payload_to_raw` rvalue 경로는 native message를 move하고, application multipart 해석은 retained owner를 이용한다. Move 생성자나 `.copy()` 이름만으로 payload deep copy를 주장하지 않았다. | `framework/languages/cpp/framework/include/zlink/framework/contracts/codecs/serializer.hpp:222`, `runtime/protocol/service_wire_codec.cpp:3400` |
| C++ bench 통계 | `snapshot_json`이 latency 배열을 복사·정렬한다. Stats polling 비용이며 Framework per-message stage가 아니다. | `framework/bench/grpc/cpp/common/bench_stats_server.hpp:90`, `:99` |

검증: 합성 스택 9개와 역검사 7개로 leaf allocator·kernel·std helper가 caller stage에 귀속되는지 확인했다. Self symbol만 사용하는 잘못된 분류는 mailbox 귀속 검사를 통과하지 못했다. Stage CPU 합계와 user/system 합계, 표본 합계, count 정합성과 perf loss를 원시 자료에 대조했다.

원시 자료는 각 실행의 `capture/`, process map과 `jitmaps/`에 있다. 해석된 전체 체인과 대표 PC는 `analysis/*.pkl.gz`, stage 규칙과 분석기는 `scripts/`, 회차별 CPU·stage·표본은 `runs.csv`, `stages-per-run.csv`, 합산 결과는 `summary.json`에 있다.
