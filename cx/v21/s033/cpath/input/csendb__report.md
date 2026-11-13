# C++ send-saturation 구조 변경 (#3868)

Node record 독립 실행과 source의 최종 service payload 단일 조립을 구현했다. 전체 C++ unit·contract CTest 306개가 통과했다. 그러나 1 KiB send의 Framework÷binding은 main 0.2839에서 변경판 0.2737로 낮아졌고, 변경판은 예산을 메시지당 3.162 µs 초과했다. 동일 속도 총 CPU도 main 22.051에서 변경판 45.123 µs/message로 늘었다. 이번 변경은 성능 목표를 달성하지 못했다.

main a0056577c4와 기존 CPU pin 변경을 시작점으로 사용했다. 기존 csend/report.md를 전부 읽고 원인 분석을 재수행하지 않았다. csend의 measure.py, runner_common.sh, collect.py, check_results.py와 pacing.diff를 재사용했다. 단독으로 진행했고 binding, Core와 spec은 수정하지 않았다.

## 1. 스펙 근거

S02 §2, `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:97–105`는 다음과 같이 정한다.

> Node handler와 ChannelName handler에는 execution gate가 없다. Handler turn은 record마다 독립이다.
> Worker는 owner queue에서 record를 FIFO 순서로 꺼내지만, record별 filter·handler pipeline과 handler 본문에 진입하는 순서는 정하지 않는다.
> 다음 record의 pipeline은 앞 handler가 suspend하거나 완료하기 전에도 다른 application worker에서 실행될 수 있다.

같은 문서 94–96행과 106행은 다음과 같이 gated owner의 경계를 정한다.

> 각 Spot과 각 Actor는 자신에게 적용되는 gate의 순서로 record를 처리한다.
> 일반 비동기 terminal은 완료 continuation까지 그 권한을 유지하므로 다음 application record가 현재 handler의 상태 접근과 겹치지 않는다.
> 같은 gate의 application turn 두 개를 동시에 실행하지 않는다.

S04 §3, `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:116–125`의 permit 순서는 다음과 같다.

> 가장 오래 기다린 live source 순서로 host-shared permit을 기다린다.
> Permit을 얻은 뒤에만 Core·binding에서 record를 receive·claim한다.
> Application record는 permit을 owner mailbox나 serial queue의 handler turn으로 이전한다.
> Worker가 owner queue에서 record를 먼저 꺼내더라도 이 계산과 permit 소유권은 바뀌지 않는다.
> Callback 전에 terminal로 끝나면 그 정리 과정에서 permit을 정확히 한 번 반환한다. 그 밖에는 callback handler의 실제 첫 instruction 직전에 반환한다.

같은 문서 191–218행은 ready owner를 한 번만 등록하고, 추가 worker는 아직 꺼내지 않은 record와 사용 가능한 idle worker가 있을 때만 깨우며, 이미 예약된 worker에는 중복 통지하지 않도록 정한다. 구현은 기존 executor의 queue와 worker count로 이 예약을 처리한다.

S05 §4, `framework/doc/framework/common/spec/server/01-execution/05-payload-ownership-and-codec.ko.md:60–68`은 다음과 같이 수명을 정한다.

> 실행 대기열에 들어간 message의 payload는 framework가 소유한다.
> 해제는 handler가 끝난 뒤에 한다.

같은 문서 92–96행은 exception, timeout, cancellation, shutdown과 relocation도 같은 release 지점에 모이며 release를 두 번 실행하지 않도록 정한다.

S08 §4.3, `framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md:229–247`은 다음과 같이 record 소유권과 worker 재사용을 정한다.

> Worker는 W1에서 owner queue head의 record 하나에 대한 실행 소유권만 얻으며, owner 전체를 독점하지 않는다.
> Record를 꺼낸 worker는 W2~W4와 그 record의 terminal 정리를 책임진다.
> Framework는 record마다 별도 dispatch task나 supervisor wrapper를 만들지 않고 공유 worker의 drain을 재사용한다.

S08 §3 E1, 같은 문서 96행은 service wire가 요구하는 전체 크기의 buffer 하나를 잡고 각 part를 한 번 쓰도록 정한다. E4의 binding operation, BACKPRESSURED admission과 physical retry 소유권은 변경하지 않는다. 이 조항들은 승인된 Node 독립 실행과 최종 service payload 단일 조립을 허용한다. FIFO는 record take 순서이며 terminal 완료를 FIFO로 직렬화하라는 요구가 아니다.

## 2. 구현

Target은 Node·ChannelName의 기존 FIFO list node를 claim의 retained storage로 splice한다. Pointer view만 남긴 채 active guard를 지우지 않는다. 다음 claim은 이미 소유권이 이전된 prefix를 다시 볼 수 없다. `service_mailbox.cpp:181,237`에서 take를 직렬화하고 splice한다. Independent claim 안에는 자신을 강하게 참조하는 credit을 보관하지 않고, 실제 transfer가 credit을 인수한다. `service_mailbox.cpp:301,419`에서 transfer와 terminal을 record에 귀속한다. Node claim은 owner의 active count를 갱신하지 않으며, 마지막 take가 빈 owner entry를 지운다. Gated Spot·Actor의 relocation-visible prefix와 gate 수명은 기존 경로를 사용한다.

Ready 집합은 mailbox에 한 곳만 남는다. 통지 batch는 그 집합의 수명과 통지 건수를 보관하며 list node를 복제하지 않는다. `service_mailbox.cpp:497,590`과 `offload_executor.cpp:126,145`에서 기존 offload executor의 queue와 mutex, idle·live worker count로 owner drain을 예약한다. `mesh_node_host_service.cpp:3348`의 순회 cursor는 기존 executor mutex 아래 여러 node의 기회를 순환시킨다. 별도 ready 상태나 queue를 만들지 않는다. Worker drain은 여러 record를 이어서 처리한다. Record마다 별도 dispatch task나 supervisor wrapper를 추가하지 않는다. Worker 반환 시 남은 작업을 예약한 뒤 종료 대기가 quiescence를 관찰한다. Node의 ready 상태를 ingress I/O 대기 생략 조건으로 사용하지 않는다.

`channel_runtime.cpp:1441,1485`에서 source는 serializer의 기존 type plan과 transport의 기존 channel fragment를 send에도 사용한다. `envelope_codec.cpp:357,452,528,1316`에서 kind와 최종 buffer writer를 공통화한다. Type plan의 request 전용 kind를 제거하고 실제 header.kind를 공통 writer가 쓴다. 기존 application_envelope_encoder_t를 message_parts_t에서 public host까지 소유권과 함께 전달하고 take_application_payload()로 service writer에 넘긴다. Remote Node send는 중간 JSON header native buffer를 만들지 않고 최종 service payload에 쓴다. `public_host_runtime.cpp:3365`에서 encoder를 service payload로 이전한다. Unknown target의 classification은 `mesh_node_runtime.cpp:4342`의 기존 operation 소유 helper가 한 번 수행한다.

승인 범위의 판정 위치 수는 header·최종 service payload용 buffer 할당 2→1, Node owner 전체의 실행 진입 거절 3→0, 합계 5→1이다. Ready 조회와 credit 회계는 실행 배타권 판정으로 세지 않는다. 제품 runtime에는 새 scheduling queue, lock, cache, timer나 flag를 추가하지 않는다. Claim의 retained list는 기존 FIFO node를 인수하는 storage이며 새 record를 enqueue하는 실행 queue가 아니다. 기존 ready 통지용 list는 제거한다.

소유 계층은 Framework의 record·payload 표현과 logical execution이다. Physical admission·retry는 binding/Core에 남는다.
스펙 조항은 S02 §2, S04 §3, S05 §4와 S08 E1·E4·§4.3이다.
교차언어 근거는 csend 보고서의 기존 source 경로 대조와 공통 스펙이다. 기존 대조 위치는 .NET `ZLinkManagedMeshNode.cs:3413,12264,12344`, Java/Kotlin 공통 `ZLinkChannelRouteCalls.java:187`·`ZLinkJavaRawSpotNode.java:329`·`ZLinkJavaRawMeshNode.java:2199`, Node `channel-clients.ts:202`·`raw-service-mesh-runtime.ts:641,2124`다. 다른 언어 runtime과 교차언어 실행 결과는 이 변경의 측정 수치로 사용하지 않는다.
변경 분류는 B다. 기존 Node 독립 실행 계약에 맞추며 public 계약을 추가하지 않는다.

## 중복 작업 점검

다음 경로는 `framework/languages/cpp/framework/` 기준이다. Native buffer를 보존하는 것과 payload byte를 복사하는 것을 구분했다.

| 관찰 | 위치 | 처리 |
|---|---|---|
| Header native buffer 작성 뒤 service payload로 복사 | `src/runtime/messaging/envelope_codec.cpp:496,528`; `src/runtime/protocol/service_wire_codec.cpp:3224` | Remote Node send에서 제거했다. 크기 측정은 byte 복사가 아니며 최종 service buffer 쓰기 한 번만 남는다. |
| 같은 owner의 active claim·drain 진입 거절 | 변경 전 `src/runtime/mesh/service_mailbox.cpp:158,181,453` | Node·ChannelName에서 제거했다. FIFO take와 record credit은 남긴다. |
| Ready owner list를 통지 batch로 이전한 뒤 재등록 | `src/runtime/mesh/service_mailbox.cpp:577,590` | 통지용 소유 list를 없애고 canonical ready 집합 하나를 사용한다. 기존 swap 자체를 deep copy로 세지 않았다. |
| Executor 내부 작업의 shutdown admission 식 중복 | `src/runtime/dispatch/offload_executor.cpp:20,99,145` | 기존 internal submit과 새 통지의 판정을 helper 한 곳에 모았다. |
| Send·request에 중복된 target classification 코드 | `src/runtime/mesh/mesh_node_runtime.cpp:4252,4342` | 기존 operation helper를 공통 사용한다. Operation별 classification task는 한 번만 만들며 physical 분류의 소유권은 유지한다. |
| Peer snapshot 반복 조회 | `src/runtime/mesh/raw_mesh_node_owner.cpp:4369,4373,1765`; `src/runtime/mesh/service_topology_registry.cpp:515` | 유지했다. Role과 readiness의 보호 대상이 다르며 deep copy는 아니다. |
| RID bytes 변환 뒤 native RID 재작성 | `src/runtime/stateful/public_host_runtime.cpp:3365`; `src/runtime/backend/raw_route_port.cpp:196,481` | 유지했다. 이번 변경은 payload·record 소유권에 한정한다. |
| 즉시 결과의 coroutine·nested result·public projection | `src/runtime/host/app.cpp:2602,2617`; `src/runtime/channels/channel_runtime.cpp:1467` | 유지했다. Public 완료와 pending 수명을 바꾸지 않는다. |
| Service payload의 external part 투영과 body retain | `src/runtime/protocol/service_wire_codec.cpp:3400`; `src/runtime/channels/route_packet_dispatcher.cpp:262` | 유지했다. Backing storage를 공유하는 참조이며 payload deep copy가 아니다. |
| Spot/internal 분류 뒤 Node dispatch | `src/runtime/mesh/mesh_node_host_service.cpp:3812`; `src/runtime/spots/spot_runtime.cpp:15692` | 유지했다. Internal packet 소유권 판정을 생략하지 않는다. |
| One-way void 결과의 빈 native message·optional reply | `include/zlink/framework/contracts/configuration/mesh_node.hpp:594`; `src/runtime/channels/route_handler_invoker.cpp:36,49` | 유지했다. 이번 claim·encoder 변경과 독립인 기존 terminal 표현이다. |
| Record byte 합계를 enqueue와 claim에서 각각 계산 | `src/runtime/mesh/service_mailbox.cpp:96,211` | 유지했다. Queue 회계와 claim byte budget에 필요하며, 계산값 보관용 cache를 추가하지 않았다. |
| Gated owner의 읽히지 않는 byte·domain active 합계 회계 | `src/runtime/mesh/service_mailbox.hpp:323,333`; `src/runtime/mesh/service_mailbox.cpp:247,456` | Node에서는 갱신을 제거했다. Gated prefix 경로의 기존 회계는 유지했다. 이 값들은 새 Node 실행 권한 판정에 사용하지 않는다. |

점검 13항목 중 6항목은 전체 또는 Node 범위에서 정리했고 7항목은 유지 이유를 위에 적었다. 새 issue나 별도 리팩토링 문서를 만들지 않았다.

## 3. 검증

집중 CTest 15개와 전체 C++ unit·contract CTest 306개가 통과했다. 전체 선택식은 `-L 'unit|contract'`이며 `--parallel 1 --output-on-failure`로 실행했다. 메모리 감시로 중단된 전체 실행은 통과한 target을 반복하지 않고 남은 target부터 이어갔다. 실행별 실제 통과 결과 306개의 합집합을 `unit-contract-coverage.json`에서 대조했다. 실행 로그는 `logs/ctest-focused.log`, `logs/ctest-unit-contract-combined.log`이며, 원본 실행별 로그 경로도 coverage 파일에 있다. 집중 검증의 상세 출력은 `logs/ctest-focused-LastTest.log`에 보존했다.

CTest target 내부의 조건부 GTest case 5개는 skip됐다. IPv6 bind를 지원하지 않는 환경 1개, ASan 전용 1개, Debug stack 분류 전용 1개, 명시적으로 켜는 actor/STREAM microbenchmark 2개다. 이름은 `unit-internal-skips.json`에 보존했다.

새 테스트는 같은 Node의 두 worker 동시 claim, 4개 thread의 4,096개 record 배타 take, 역순 terminal, close, 중복 credit 반환, untaken claim의 순환 참조 부재와 gated Actor 음성 대조를 검증한다. 실제 hosted Node 테스트도 첫 동기 handler가 막혀 있는 동안 같은 Node의 다음 handler들이 끝나는지 확인한다. 기존 Actor·Spot 실행, taken-before-entry, BACKPRESSURED와 비동기 handler cleanup 검증은 위 집중·전체 suite에 포함됐다.

새 동시 claim 검사는 main에서 의도한 실패를 재현했고 변경본에서 통과했다. 근거는 `logs/main-independent-records-negative.log`, `logs/main-independent-records-negative.json`, `logs/ctest-ownership-final.log`다. Command kind와 wire byte 대조, 기존 request golden fixture 및 byte 변경 음성 대조도 통과했다.

측정 75개 cell은 errors·abandoned·header validation failure·target reject·drain 초과·contamination이 없고 submitted=completed=received였다. `quality.json`의 나쁜 입력 10종은 모두 거부됐다. 남은 테스트 실패는 없다.

## 4. 측정

Condition B를 사용했다. Source의 모든 thread는 CPU 0–7, target은 CPU 8–15에 제한했고 capture에서 affinity를 확인했다. Main과 변경본은 같은 compiler와 `-O3 -DNDEBUG -g -fno-omit-frame-pointer` 옵션으로 빌드했다. Warmup은 30초·10 segment, 무제한 active는 5초다. Exclusive `measure.lock` 안에서 main·변경본·binding 순서를 회차마다 바꾸며 측정했다. 아래 값은 각각 RUNS=5 중앙값이다.

예산은 이번 binding 중앙값의 µs/message를 0.6으로 나눈 값이다. 초과분은 Framework µs/message에서 이 예산을 뺀 값이다. 음수는 그만큼 예산 안에 있다는 뜻이다.

| 패턴·크기 | 구현 | Framework msg/s | Binding msg/s | Framework÷binding | Framework µs/message | 예산 µs/message | 예산 초과 µs/message |
|---|---|---:|---:|---:|---:|---:|---:|
| send-saturation 1 KiB | main | 178,387.2 | 628,391.8 | 0.2839 | 5.606 | 2.652 | 2.954 |
| send-saturation 1 KiB | change | 171,989.6 | 628,391.8 | 0.2737 | 5.814 | 2.652 | 3.162 |
| send-saturation 4 KiB | main | 158,386.3 | 288,394.3 | 0.5492 | 6.314 | 5.779 | 0.535 |
| send-saturation 4 KiB | change | 162,637.1 | 288,394.3 | 0.5639 | 6.149 | 5.779 | 0.370 |
| request-backpressure 1 KiB | main | 52,855.4 | 250,810.9 | 0.2107 | 18.920 | 6.645 | 12.274 |
| request-backpressure 1 KiB | change | 55,485.0 | 250,810.9 | 0.2212 | 18.023 | 6.645 | 11.378 |
| request-serial 1 KiB | main | 7,898.7 | 16,086.9 | 0.4910 | 126.603 | 103.604 | 22.999 |
| request-serial 1 KiB | change | 7,467.0 | 16,086.9 | 0.4642 | 133.923 | 103.604 | 30.319 |

회차별 원시 처리량은 `summary.json`의 `rates`와 `runs5/`의 각 `results.json`에 보존했다. 좋은 회차만 골라 계산하지 않았다.

1 KiB send에서 변경판은 예산을 메시지당 3.162 µs 초과했다. Main보다 메시지당 wall 처리시간이 0.209 µs 늘었다. 이번 구조 변경은 이 처리량 목표를 달성하지 못했다. Request-serial도 main보다 메시지당 7.320 µs 느려졌다. 동시 실행과 소유권 검증 통과를 성능 기준 충족으로 판정하지 않았다.

동일 전송률 CPU 측정은 send-saturation 1 KiB에서 공통 136,889 msg/s를 사용했다. 각 구현 3회, active 20초와 약 12초의 capture를 사용했다. 두 프로세스의 전체 thread를 포함하는 process utime+stime의 차이를 실제 target 수신 건수로 나눴다. 아래 각 열은 3회 중앙값이다. 총 CPU를 wall 처리시간 예산 초과분에 더하지 않았다.

| 구현 | 실제 msg/s | Source CPU µs/message | Target CPU µs/message | 총 CPU µs/message |
|---|---:|---:|---:|---:|
| main | 136,889.4 | 8.875 | 13.176 | 22.051 |
| change | 136,889.6 | 12.539 | 32.537 | 45.123 |
| binding | 136,888.2 | 2.595 | 2.353 | 4.948 |

변경판과 main의 총 CPU 중앙값 차이는 메시지당 +23.072 µs다. 이 값은 같은 전송률에서 관찰한 프로세스 CPU 비용이며, 원인을 분리한 profiler 결과로 해석하지 않는다.

기존 direct floor binary도 target만 main·변경본으로 바꿔 각 3회 재사용했다. 이 도구는 source Framework를 생략한 같은 wire 재생이며, 원래 sequence·timestamp를 재사용하므로 latency 비교에는 사용하지 않았다.

| Direct floor target | 중앙값 msg/s | µs/message | 같은 binding 대비 비 | 예산 초과 µs/message |
|---|---:|---:|---:|---:|
| main | 210,413.4 | 4.753 | 0.3348 | 2.100 |
| change | 197,864.2 | 5.054 | 0.3149 | 2.402 |

측정 스크립트는 `scripts/`, 원시 자료는 `runs5/`, `paced/`, `floor/`, 집계는 `summary.json`이다. 실행 파일 hash는 `binary-hashes.json`, 측정 시작 시 Core·binding hash는 `package-hashes-at-lock.json`에 있다. 측정 후에도 실행 파일 6개와 라이브러리 5개의 hash가 유지됐음을 확인했고 라이브러리 확인값은 `package-hashes-after.json`에 보존했다. 제품 diff는 `change.diff`, 기존 CPU pin과 재사용 pacing 변경은 `bench.diff`에 분리했다. 두 diff의 역방향 적용 검사와 `git diff --check`는 통과했다.

관찰한 최소 MemAvailable은 빌드·검증 12.489 GiB, 측정 24.778 GiB다. 자원 기록은 `memory-build.jsonl`, `memory-samples.jsonl`에 있다.
