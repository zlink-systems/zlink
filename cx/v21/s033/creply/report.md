C++에서도 Framework reply가 binding보다 더 많고 작은 TCP data packet으로 전송됐다. 40k req/s의 두 perf 구간에서 packet 수는 Framework 약 0.886/request, binding 0.683–0.693/request였다. 평균 data segment는 약 1,600 B와 1,808–1,834 B였다. Packet당 kernel 비용도 Framework가 더 높았다. Java의 packet 수만으로 거의 전부 설명되는 결과와 차이가 있다.

Framework mutex 경합도 관측했다. 유효 요청 1,200,000건 중 130,530건에서 trylock이 EBUSY였고, 뒤따르는 pthread_mutex_lock의 누적 경과 시간은 1,059,849,359 ns였다. 요청당 0.883208 µs, 경합 호출당 약 8.120 µs다. Mutex가 보호하는 C++ 객체 접근은 유지하고 native submit만 lock 밖으로 옮기는 조건은 성립했다. 공유 슬롯 대기로 시간 제한 안에 프로토타입 검증과 후속 측정을 완료하지 못했다. 원본 측정과 미검증 제안을 구분해 기록했다.

경로의 줄 번호는 측정 전 worktree 소스 기준이다. 표의 경로는 `/root/worktree/zlink-creply/`를 기준으로 한다.

| 단계 | 파일:줄 | 실행 thread |
|---|---|---|
| typed echo handler | `framework/bench/grpc/cpp/framework/bench_framework_cpp_server.cpp:22` | application dispatch worker |
| handler 호출과 protobuf reply 직렬화 | `framework/languages/cpp/framework/include/zlink/framework/contracts/configuration/mesh_node.hpp:678`, `framework/languages/cpp/extensions/framework-codec-protobuf/include/zlink/codecs/protobuf.hpp:24` | 같은 worker. 이 handler는 동기 함수다. |
| completing handler turn에서 reply completion 호출 | `framework/languages/cpp/framework/src/runtime/channels/route_handler_invoker.cpp:63` | 같은 worker |
| reply envelope 구성과 submit callback | `framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp:426`, `framework/languages/cpp/framework/src/runtime/mesh/mesh_record_dispatcher.cpp:61` | 같은 worker |
| host reply token 조회와 application payload 보관 | `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:9951`, `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:8733` | 같은 worker |
| port 참조 조회와 service reply header/body 구성 | `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:2042` | 호출 worker. `_lane.run_sync`는 호출자가 자기 turn을 실행한다. 경합하면 turn을 기다린다(`runtime/execution/state_lane.hpp:289`). |
| socket mutex와 binding reply operation 생성·제출 | `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:501`, 실제 lock `:506`, submit `:517` | 같은 worker |
| binding operation 생성과 token owner 확인 | `bindings/cpp/src/Runtime/Sockets/router.cpp:107` | 같은 worker |
| token owner 재확인, native part 배열 제출과 `zlink_reply` | `bindings/cpp/src/Runtime/Messaging/reply_operations.cpp:16`, `:32` | 같은 worker |
| Core public handle pin과 reply token·Completion route·record admission | `core/src/api/socket/socket_request_reply_submit_api.cpp:634`, `:474`, `core/src/api/socket/socket_api_internal.hpp:102` | 같은 worker |
| Core TCP output와 비동기 write | `core/src/runtime/engine/asio/asio_engine.cpp:1629`, `:754`, `core/src/runtime/transports/tcp/tcp_transport.cpp:519` | 별도 Core I/O thread. 실제 TID는 capture에서 확인한다. |

`raw_route_port_t::_socket_mutex`는 reply 전용 lock이 아니다. 같은 포트의 send(`raw_route_port.cpp:183`), request(`:375`), receive와 재사용 receive envelope(`:461`), close와 `_socket = nullptr`(`:527`)를 함께 보호한다. Mesh owner도 같은 mutex를 주입한다(`raw_mesh_node_owner.cpp:545`). 따라서 이 mutex에는 C++ wrapper를 읽는 동안 포인터가 무효화되지 않게 하는 역할과, native reply 제출까지 직렬화하는 역할이 함께 있다.

Binding의 `router_socket_t::reply`는 native handle과 weak runtime 참조를 operation에 보관한다(`router.cpp:110–119`). 동기 submit은 runtime의 strong 참조를 얻고 token owner를 확인한 뒤 native 호출을 수행한다(`reply_operations.cpp:20–35`). Operation 생성 뒤에는 C++ socket 객체를 다시 읽지 않는다. Core `as_socket_handle`은 public handle을 acquire하고, scope 소멸 때 pin을 반환한다(`socket_api_internal.hpp:102`). Core close gate는 `socket_public_handle.cpp:90`이, record admission은 `socket_request_reply_submit_api.cpp:495`의 complete-send scope가 담당한다.

기준 계약은 `bindings/doc/spec/README.ko.md:496`의 Multipart 제출, 같은 문서 `:1186`의 별도 송신 gate 금지와 close 경합 소유, `core/doc/spec/core/socket/README.ko.md:43`의 same-handle concurrency·close gate다. Framework의 C++ 객체 수명 보호는 필요하다. 독립 record의 native admission을 위한 두 번째 직렬화는 Core가 이미 소유한다.

| 결정 | spec 소유 | 현재 판정 위치 | 프로토타입에서 사용할 소유자 |
|---|---|---|---|
| whole-record native reply 제출 | Core socket thread safety와 binding Multipart 제출 | C++ `raw_route_port.cpp:506`; Java `ZLinkJavaRawServicePort.java:531`; .NET `ZLinkRawRouterServicePort.cs:247`; Node `node-raw-binding-port.ts:260`; 각 binding/Core | binding/Core. C++ mutex는 operation 생성까지만 보호하고 native submit 전에 해제할 수 있다. |
| C++ wrapper 포인터 수명 | Framework port의 resource lifetime | C++ `raw_route_port.cpp:506`, `:527` | 기존 socket mutex. 새 상태나 별도 gate를 만들지 않는다. |
| token owner·one-shot reply와 물리 reply route | binding/Core request/reply | C++ binding `router.cpp:114`, `reply_operations.cpp:24`, Core `socket_request_reply_submit_api.cpp:489` | 기존 binding/Core |

.NET과 Node는 binding reply를 직접 제출하고 Java는 port state lane 안에서 제출한다. C++와 Java의 추가 직렬화는 실행 자원 선택의 차이다. 네 언어의 public 결과 계약을 바꾸는 실험은 아니다. 측정 조건이 충족되면 변경 분류 A, 기존 binding 동시 제출 계약의 Framework 적용으로 한정한다. 다른 언어의 구현과 spec은 수정하지 않는다.


Source CPU 0–7, target CPU 8–15의 condition B다. 같은 머신의 127.0.0.1 TCP 연결을 사용했다. 여기서 packet은 NIC capture가 아니라 TCP data segment 카운터다. Pattern은 C++의 기존 request-backpressure이고 실제 application window는 0, 즉 무제한이다. Trigger JSON의 requestWindow 100은 C++ trigger 계약의 고정값이며 실제 window는 CLI pattern이 결정한다. Payload는 1,024 B, sendConcurrency는 8이다. Warmup 30초, active 40초 안에서 perf 25초와 겹치는 ss 카운터를 사용했다. 사전 실행의 실제 처리율은 Framework 39,995.984, binding 39,999.584 req/s였다.

| 40k perf 실행 | 실제 perf 구간 req/s | reply data packet/request | 평균 data segment B | reply 방향 pure ACK/request | tcp_sendmsg 이하 cycles/request | cycles/data packet |
|---|---:|---:|---:|---:|---:|---:|
| main-perf-1 | 39,942.360 | 0.887633 | 1,596.046 | 0.653086 | 30,773 | 34,669 |
| binding-perf-1 | 39,938.161 | 0.692708 | 1,807.512 | 0.340399 | 20,979 | 30,286 |
| main-perf-2 | 39,920.242 | 0.885380 | 1,600.519 | 0.399374 | 31,338 | 35,395 |
| binding-perf-2 | 39,935.208 | 0.682531 | 1,834.344 | 0.335583 | 21,172 | 31,020 |

Reply TCP payload는 Framework 약 1,417 B/request, binding 약 1,252 B/request다. TCP/IP header는 제외한다. 네 perf 실행의 request/reply 연결에서 retransmit delta는 모두 0이었다. 별도 network BPF의 모든 GSO 관측은 gso_segs=1, gso_size=0이었다. Kernel cycles는 Java 수집기와 동일하게 thread별 cycles:k counter에 tcp_sendmsg stack 표본 period 비율을 적용한 추정치다. 표본은 249 Hz다. 다른 함수의 inclusive 비용을 합산하지 않았다. Packet과 ACK는 perf 안에 들어가는 ss 구간의 완료 요청 수로 나눴다. ACK는 reply sender에 들어온 반대 방향 pure ACK다.

1회에서 binding의 packet당 비용을 고정하면 packet 증가가 추가 9,794 cycles/request 중 5,903 cycles/request를 설명한다. 2회에서 binding의 packet당 비용을 고정하면 packet 증가가 추가 10,166 cycles/request 중 6,292 cycles/request를 설명한다. 이 분해의 범위는 target의 tcp_sendmsg 이하 비용이다. Source caller의 CPU 시간이나 전체 처리량 차이를 설명하는 계산은 아니다.

| 별도 BPF 구간 | target reply sendto/request | 반환 B/성공 call | target reply I/O wakeup/request | Native reply producer TID |
|---|---:|---:|---:|---|
| Framework | 0.853941 | 1,662.385 | 0.457380 | 2010860, 2011025, 2011149 |
| binding | 0.937249 | 1,337.689 | 0.709926 | 2011853 |

Framework의 실제 sendto는 target I/O TID 2009759, binding은 2011850에서 관측했다. 둘 다 comm은 ZLINKbg/IO/1이다. Producer와 실제 TCP 송신은 별도 thread다. Syscall은 target의 모든 thread를 대상으로 수집했고 표는 reply data FD를 골랐다. 해당 FD의 short send와 EAGAIN은 두 조건 모두 0이었다. Native reply 호출 경계에서 producer TID를 계수했다. 각 소스 단계의 OS thread를 메시지별로 연결하는 추적은 하지 않았다. 동기 echo의 handler completion과 reply 제출이 같은 worker에서 수행되는 것은 위 소스 경로로 확인했다.

BPF syscall 구간의 실제 처리율은 Framework 40,022.173, binding 39,944.002 req/s였다. 약 6초 동안 reply FD의 sendto는 각각 205,063회와 224,628회였다. 약 5초 wakeup 구간에서 해당 target I/O thread는 91,273회와 141,790회 깨어났다. BPF의 request당 값은 관측한 완료율에 trace 시간을 곱한 정규화 요청 수로 나눈 값이며, trace 경계와 완전히 일치하는 완료 카운터 delta는 아니다. Syscall 분모는 약 240,137.125와 239,667.491, wakeup 분모는 약 199,556.088과 199,725.066 요청이다.

BPF 구간의 묶음 전송과 wakeup은 perf 구간과 다르다. 따라서 위 sendto 수나 wakeup 수를 perf 비용과 곱해 원인을 계산하지 않았다. Packet/CPU 표는 같은 perf 구간 안의 ss만 사용한다. Framework의 reply wire bytes도 binding보다 약 165 B/request 더 많다. 따라서 packet 증가 전체를 mutex가 제출을 분산시킨 결과로 단정할 수 없다. 추가 native 제출 직렬화를 없앤 뒤에도 application dispatch와 wire framing 비용은 남는다.

Mutex는 reply 함수의 직접 pthread_mutex_lock call site만 선택했다. LD_PRELOAD 계측기는 trylock 성공 호출을 대기 0으로 처리하고, EBUSY 호출만 실제 lock 경과 시간을 잰다. Active 직전·직후 /proc/PID/mem으로 카운터를 읽어 warmup을 제외했다. Calls 1,200,000건과 active 완료 요청 수가 정확히 일치했다. 이 값은 계측된 wall wait이며 CPU 시간과 구분한다. Clock과 trylock 계측의 영향은 포함된다. 정상 100회, 의도한 50 ms 경합 1회, 범위 밖 호출 제외를 모두 검사했다(tools/mutex-validation.json).

확인한 중복 작업은 다음과 같다. 현재 runtime 프로토타입은 아직 적용되지 않았다.

| 중복 작업 | 파일:줄 | 상태 |
|---|---|---|
| Native record 제출을 Framework mutex와 Core admission이 각각 직렬화 | raw_route_port.cpp:506, Core socket_request_reply_submit_api.cpp:495 | 미검증 diff에서 submit의 Framework lock 범위 제거. Runtime에는 미적용 |
| Router reply factory와 terminal이 같은 token owner를 확인 | bindings/cpp/src/Runtime/Sockets/router.cpp:114, bindings/cpp/src/Runtime/Messaging/reply_operations.cpp:24 | binding 수정 금지 범위이므로 유지 |
| Protobuf native buffer에 쓴 body를 service wire buffer에 다시 복사 | framework/languages/cpp/extensions/framework-codec-protobuf/include/zlink/codecs/protobuf.hpp:30, runtime/protocol/service_wire_codec.cpp:3201 | wire·codec 변경 없이 유지. Binding bench도 string 직렬화 뒤 native message 복사가 있다(cpp/zlink/bench_zlink_cpp_server.cpp:63–65). |
| Source RID를 binding 값으로 만들고 operation 값으로 다시 복사 | raw_route_port.cpp:510, bindings/cpp/src/Runtime/Sockets/router.cpp:118 | 새 cache나 상태를 추가하지 않고 유지 |

미완료 항목은 프로토타입 close 경합 검사, RUNS=5 처리량 비교, 동일 요청률 재계측이다. 최초 빌드는 MemAvailable 11.916 GiB를 관측한 즉시 SIGTERM으로 중단했다. 재개는 -j1과 13 GiB 중단 여유로 수행했다. 첫 trigger 사전 실행은 requestWindow 값이 잘못돼 HTTP 400으로 끝났으며 보존했다. Close fixture의 최초 컴파일은 errno getter 선언 오류로 중단됐고 수정했다. 이 실행들에서 성능 수치를 가져오지 않았다.

프로토타입 제안은 `prototype.diff`다. 현재 이 diff는 runtime 파일에 적용하지 않았고 빌드·close 검증도 완료하지 않았다. 검토한 대안은 mutex 전체 제거와 operation 생성까지만 mutex로 보호하는 방식이다. 전체 제거는 `_socket` 검사·C++ wrapper 접근과 wrapper 파괴의 경합을 보호하지 못한다. 선택한 방식은 기존 mutex로 `router.reply(...).message(...)` 생성까지만 보호하고, cached native handle과 weak runtime token을 가진 기존 binding operation의 `.submit()` 전에 unlock한다. 새 queue·lock·cache·control flag·runtime state는 없다.

Close가 operation 생성 전에 port를 닫으면 기존 null 검사로 false를 반환한다. 생성 후 wrapper가 사라져 weak runtime이 만료되면 binding이 submit error를 내고 기존 catch가 false를 반환한다. Runtime 참조를 얻은 뒤 close가 먼저 수락되면 Core public handle gate가 새 API 진입을 거부한다. API가 먼저 진입한 경우 Core의 pin·close admission 계약을 따른다. 이 설계는 C++ wrapper를 submit 중 다시 읽지 않는다. Close fixture는 context를 유지한 상태에서 socket close만 경합시키도록 준비했다. 실제 close 경합 fixture 결과는 아직 없다. 따라서 close 안전성을 실측 검증했다고 주장하지 않는다.

제안의 native record 제출 직렬화 판정 위치는 Framework+Core 2곳에서 Core 1곳이다. C++ 객체 접근 보호의 소유자는 기존 Framework mutex 1곳으로 유지한다. 현재 실제 소스는 원본이므로 적용 후 규칙 수나 성능 개선을 완료 결과로 보고하지 않는다. 점검에서 기록한 중복은 4항목이며, 제안은 제출 직렬화 범위 1항목만 다룬다. Token owner 확인, body 복사, RID 복사는 유지한다. GitHub 이관·commit·push·spec 수정·Core/binding 수정은 하지 않았다.


계측 재현 자료는 `runner/`, `tools/`, `runs/`, `tables.json`, `perf-summary.json`, `aligned-ss-summary.json`에 보존했다. Java의 jsend runner와 perf·ss·BPF 도구를 C++ 실행 파일과 trigger 계약에 맞춰 재사용했다. `bin-main/`에 실제 사용한 바이너리, `binary-main.json`과 `inputs.json`에 입력 해시, 각 run의 `meta.json`에 실제 로드한 Core와 thread affinity를 보존했다. Core 1.23.0과 C++ binding 1.23.0은 변경하지 않았다. Core library SHA-256은 Java report에서 사용한 것과 같다. 빌드 설정은 Release `-O3 -DNDEBUG -g -fno-omit-frame-pointer`이며 C++ 빌드는 지정된 cpp-build-slot.sh를 거쳤다.

원본 유효 실행 9개는 총 12,800,000 요청을 완료했다. 각 실행에서 submitted=completed=target.received, errors=abandoned=header_validation_failures=target.errors=0, contaminated=false, drain_bound_hit=false를 확인했다. 검사기가 완료 수 변경, 오류, abandoned, 혼입, drain 초과를 넣은 입력을 각각 거부하는 것도 확인했다. 유효 batch 종료 코드 0과 runner의 fail-closed 검사 루프가 이 판정의 근거다. 성능 실행은 모두 지정된 배타 measure.lock 안에서 수행했다. Mutex 계측기의 성공·경합·범위 제외 검사는 measure-shared.sh로 수행했다.

유효 원본 성능 실행의 MemAvailable 최솟값은 24.941 GiB였다. 앞서 기록한 빌드 중 11.916 GiB 관측과 구분한다.


3단계는 미완료다. Packet 증가와 mutex 경합을 모두 관측했으므로 조건 불성립으로 중단한 것은 아니다. 수정한 close fixture를 빌드하는 명령이 공유 측정 큐 뒤에서 시작되지 못했고, 약 4시간 제한에 맞춰 대기 프로세스를 SIGTERM으로 종료했다. Native submit을 lock 밖으로 옮기는 `prototype.diff`는 소스 검토만 마친 미검증 제안이다. Close fixture의 컴파일 성공, close 경합 통과, 프로토타입 처리량 개선은 주장하지 않는다.

이번 세션의 unpaced RUNS=5 request-backpressure 1 KiB·4 KiB와 request-serial 1 KiB 및 프로토타입의 동일 요청률 packet 수는 미측정이다. 따라서 새 F÷B와 메시지당 예산 초과 µs도 계산하지 않았다. Paced 40k req/s의 요청 간격이나 앞선 조사 수치로 이 값을 대체하지 않았다. 0.6 목표는 유지한다.

측정용 변경은 `bench.diff`, `prototype.diff`, `measurement-source/`에 보존한 뒤 worktree에서 제거했다. 생성한 C++ build 디렉터리와 이 작업의 대기 프로세스도 정리했다. 시작 전부터 있던 네 언어 run_local.sh CPU-pin 변경만 남겼다. `final-status.txt`에 최종 상태를 저장했다. Runtime 원본은 변경하지 않았다. 준비한 close fixture와 후속 실행 도구는 보존했지만 실행 완료의 근거로 취급하지 않았다.

제안의 소유 계층은 Framework의 C++ wrapper 수명 보호와 binding/Core의 native record 제출·close admission이다.

제안의 spec 근거는 binding README의 Multipart 제출·별도 송신 gate 금지와 Core socket README §2의 same-handle concurrency·close gate다.

교차언어 대조는 C++ mutex, Java port lane, .NET·Node 직접 binding 제출을 소스로 확인한 범위다. 교차언어 contract test는 실행하지 않았다.

변경 분류는 제안에 한해 A, 기존 binding 계약 적용이다. Runtime 구현 완료나 검증된 수정으로 분류하지 않는다.

실제 runtime의 제출 판정 위치 수는 변경 전 2곳, 변경 후 2곳이다. 제안은 2곳을 1곳으로 줄이는 내용이다. 리팩토링 점검은 중복 4항목 발견, 실제 수정 0항목, 미검증 제안 1항목이며 현재 4항목 모두 원본에 남아 있다.
