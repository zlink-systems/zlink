# .NET binding send 경로 pass 1 — builder와 native message 경계

감독자가 고정 Core 0.17.2에서 .NET send 후보의 계약 유지와 성능 채택 여부를 판단하기 위한 기록이다.

**채택 후보 0건.** 기존 native array helper를 사용하면 정상 DD 2-part send의
P/Invoke는 **13→12회**, 일반 GC 전환은 **10→9회**로 줄일 수 있었다.
그러나 공식 after 5회 median의 전체 after/C는 **61.45%**였고, 지정 before 대비
처리량 변화 평균은 **−0.13%**여서 유의미한 개선이 없었다. 라이브러리 후보는 기각·원복했다.
.NET 단순 one-way 목표 85%는 DD·PUBSUB 모두 미달이다.

단위·contract **232/232**, sample **7/7**, 공식 perf **100 case 성공·실패 0**이다.
pattern aggregate 기준 회귀 threshold는 통과하지만, 개별 size에는 회귀가 있다.
기존 r1net을 포함한 .NET report의 최종 RESULT가 5회 median이 아닌 마지막 반복을
기록하는 집계 결함도 확인했다. 지정 before 수치는 주표에 유지하고, 같은 r1net 원시
5회 자료를 재집계한 보조 비교를 함께 제시한다. 러너를 수정하거나 before를 새로 측정하지 않았다.

62.51%는 DD의 5개 size aggregate다. 비용 지도의 **64 B 한 size**는
922,799.8 / 1,741,225.4 = **53.00%**다. 두 기준을 혼용하지 않는다.

## 1. 비용 해석과 호출자별 할당

[비용 지도](2026-09-08-dotnet-dd-cost-map.ko.md)의 80.511 B/msg는 AllocationTick의
byte weight 추정이다. 64 B 정상 DD client 전송에서 실제 생성되는 객체는
**80 B `SocketSendOperation` 하나**다. 별도 `TwoMessageReadOnlyList` 32 B를 더한
정상 경로의 고정 할당은 112 B/msg이며, 원 지도의 정확한 합계는 112.010 B/msg다.

| AllocationTick에 기록된 SocketSendOperation 호출자 | 64 B 추정 B/msg |
|---|---:|
| `PerfSocketIo.SendMeasurementAsync(IDealerSocket, Message, SendFlags)` — 인라인된 factory | 78.804545 |
| `DealerSocket.Send()` | 1.706190 |
| 합계 | 80.510735 |

두 행은 서로 다른 객체 종류가 아니다. 동일한 factory 할당이 인라인 여부에 따라
다른 stack 이름으로 귀속된 것이다. 원본은
`/tmp/zlink-dotnet-dd-cost-map/perf-dotnet-64.alloc.json`의 `callers`다.

public API factory를 각각 10,000회 준비한 뒤 100,000회 실행하고
`GC.GetAllocatedBytesForCurrentThread()`의 차이를 집계했다. factory delegate는 집계 밖에서
생성하고, 반환 객체를 static sink에 저장했다. 이는 할당 검증이며 처리량 판정용 microbenchmark가 아니다.

| public factory | 실제 반환 구현 | B/factory |
|---|---|---:|
| `IDealerSocket.Send()` | `SocketSendOperation` | 80.000 |
| `IRouterSocket.Send(rid)` | `SocketSendOperation` | 80.000 |
| `IPubSocket.TryPublish(topic)` | `PublisherTryPublishOperation` | 72.000 |
| `IDealerSocket.Request()` | `DealerRequestOperation` | 64.000 |
| `IRouterSocket.Request(rid)` | `RouterPeerRequestOperation` | 80.000 |

64-bit CLR의 DD builder 구성은 object header 16 B, socket reference 8 B,
`RoutingId?` 24 B, `OperationMessageBuffer` 24 B, submission guard 1 B와 정렬 여백 7 B다.
public `RoutingId`/`RoutingId?`의 `Unsafe.SizeOf`는 16/24 B다. 서로 다른 `Send()` 결과의
identity는 다르고, `Message(message)`는 동일 builder를 반환한다.

### 관리 코드의 실행 범위

- `DealerSocket.cs:22`의 factory는 builder를 생성한다.
- `SocketOperations.Send.cs:19`는 각 `Message(...)`에서 submission guard와 null을 검사하고,
  기존 `OperationMessageBuffer`의 첫 part·둘째 part·다수 part 저장소에 누적한다.
- 같은 파일 `:40`의 `Async`는 guard·empty 검증, 제출 상태 전이, `Parts` view 생성 뒤
  `CompletionOwner.SendAsync`에 전달한다.
- `CompletionOwner.cs:487`은 이미 generic value submitter인 `SendPartSubmitter`를 사용한다.
  정상 noncancelable 성공은 `Task.CompletedTask`를 반환한다. 새 entry·Task는 이 정상 경로에 없다.
- runner의 `CompleteMeasurementSend`는 Task 성공 상태 검사와 Message disposal을 수행한다.
  state machine은 대기할 Task가 있을 때에만 별도 메서드에서 생성된다.

**이전 pass 2의 reply closure 352 B와 같은 제거 가능한 send closure는 확인되지 않았다.**
현재 send builder에 capture lambda·delegate·async state machine이 없고,
제출은 이미 struct다. public `SendOperation`·`SendSubmitOperation`은 interface이므로
구현을 struct로 바꾸기만 하면 factory 반환 시 boxing이 필요하다. builder 단계를 오가며
복사된 struct는 동일한 one-shot 제출 상태를 보장하지도 못한다.
기존 reference builder는 이 상태를 한곳에서 소유한다.

177.671 ns/msg 전체를 builder 생성 비용이나 제거 가능한 closure 시간으로 볼 수 없다.
원 지도의 함수 표를 재집계하면 `managed_builder`는 3.254 ns,
`managed_send_helper`는 174.417 ns다. 후자에는 인라인된 binding 코드가 포함된
`SendMeasurementAsync` self 90.462 ns와 allocator·page fault 등 호출 문맥이 함께 있다.
**3.254 ns만 binding 비용이라는 뜻도 아니다.** 인라인된 명령의 세부 귀속이 분리되지 않아,
80 B를 없애면 177.671 ns가 사라진다는 인과적 환산은 성립하지 않는다.

## 2. P/Invoke 호출 횟수와 통합 경계

다음은 **성공·재시도 없는 DD client 2-part send 한 건**의 소스상 호출 수다.
`P/Invoke 1회`는 managed→native 진입과 managed 복귀 한 쌍을 뜻한다.

| 함수 | before | 후보 after | 일반 GC 전환 |
|---|---:|---:|---|
| `zlink_msg_init_size` | 2 | 2 | 있음 |
| `zlink_msg_data` | 1 | 1 | SuppressGCTransition |
| `zlink_msg_init` | 2 | 2 | SuppressGCTransition |
| `zlink_msg_copy` | 2 | 2 | 있음 |
| `zlink_send_part` | 2 | 2 | 있음 |
| 원본 `zlink_msg_close` | 2 | 2 | 있음 |
| scratch `zlink_msg_close` | 2 | 0 | 있음 |
| scratch `zlink_multipart_close` | 0 | 1 | 있음 |
| **합계** | **13** | **12** | **10→9회** |

Core 내부의 scratch close는 여전히 2회다. helper 안에서 반복하므로 native 자원 정리,
part 순서, original 실패 보존을 유지하면서 managed↔native 진입만 합친다.
일반 n-part scratch cleanup의 진입 횟수는 n→1이다. 초기화에 실패한 경우에는 `built`로
검증된 prefix만 정리한다. 0-part prefix는 helper의 count 0 호출이다.

사용한 함수는 이미 `NativeMethods.Core.cs`에 import된 Core 공개 함수다.
[Core message spec §zlink_multipart_close](../../../../../core/doc/spec/core/02-message.ko.md#zlink_multipart_close)는
연속 배열의 각 part에 `zlink_msg_close`를 호출한다고 정한다.
고정 패키지 `libzlink.so.0.17.2`의 disassembly에서도 비-TLS 배열 경로는 close loop만 수행하며
배열 저장소를 free하지 않는다. 새 Core 함수·bridge binary·public import를 추가하지 않았다.

### pattern별 적용 범위

DD와 REQREP의 `CompletionOwner` send/request/reply는 동일한
`SubmitPreservingOnFailure<T>`를 사용한다. REQREP client의 2-part 생성·request 제출도
13→12회이며, 이미 받은 2-part Message를 reply에 쓰는 **reply 제출 구간만**은
init 2 + copy 2 + reply 2 + original close 2 + scratch close 2 = 10→9회다.
이 수치를 receive·completion drain을 포함한 전체 REQREP op의 호출 수로 사용하지 않는다.

PUBSUB은 `PublisherTryPublishOperation` → `SocketKernel.PublishNoWaitParts` →
`SubmitMultipartCore`의 move 제출 경로다. 정상 2-part 생성·publish는
init_size 2 + data 1 + init 2 + move 2 + publish 2 = 9회이고 이번 후보로 바뀌지 않는다.
따라서 네 pattern의 약 60% aggregate만으로 동일 builder·동일 native 경계 비용이라고
단정할 수 없다. 큰 메시지의 retry·completion 추가 호출 수는 이번 static count에 포함하지 않는다.

## 3. 후보 판정과 계약 근거

| 후보 | 수치·계약 근거 | 판정 |
|---|---|---|
| send capture closure→struct | DD builder 80 B는 public interface로 반환되는 operation 자체다. send submitter는 이미 struct이며 정상 경로의 추가 closure는 0개다. 공통 binding spec의 builder one-shot·동일 누적 상태를 유지해야 한다 | 기각: pass 2와 같은 제거 대상 없음 |
| `init+copy` 내부 native helper | 한 part의 호출 2→1이지만 init은 이미 전환 억제여서 일반 전환 1→1, DD 기준 일반 전환 절감 0회다. 지도의 copy 경계는 5.206 ns/msg, allocating init_size의 82.436 ns/msg를 제거하는 후보가 아니다 | 추가 구현하지 않음: 새 native bridge보다 기존 cleanup helper 통합을 우선 |
| `init_size+data/payload-copy` 내부 helper | Message 생성당 호출 2→1, 일반 전환 1→1. 이미 공개적으로 반환된 개별 Message의 init_size 2회를 한 호출로 합치려면 eager 생성 경계와 상태가 달라진다 | 추가 구현하지 않음: 지도의 지배적 일반 전환 횟수 절감 없음 |
| close를 다음 init에 합침 | 원본 ownership release와 wrapper 무효화는 이번 terminal에서 끝나고 다음 Message 생성은 별도 public 호출이다. release를 지연하면 별도 retained 상태와 lifetime 규칙이 필요하다 | 기각: 명시된 close·성공 소비 경계 변경 |
| 기존 `zlink_multipart_close`로 scratch 정리 통합 | DD 13→12 호출, 일반 전환 10→9. scratch close를 생략하지 않고 기존 prefix·finally 정리 규칙을 유지한다 | **기각·원복: 유의미한 개선 없음, size별 회귀 관측** |

공통 binding spec의 [operation builder](../../../../../bindings/doc/spec/README.ko.md)는
`message(...)` 단계의 실패 시 원본 보존과 재제출 금지를 정한다.
[.NET spec](../../../../../bindings/doc/spec/dotnet/README.ko.md)의
「Send·request terminal」과 「라이브러리 형태」는 interface builder 및 Async 완료 경계를 소유한다.
이전 [pass 2 후보표](../../../../plan/c016-worklog/dotnet-perf-pass2-summary.md#후보-검토)와
[최적화 가이드 §4](../../../BINDINGS_OPTIMIZATION_GUIDE.ko.md#4-기각한-후보-다시-시도하지-않을-것)의
기각 후보는 재실험하지 않았다. GC 감소를 개선 가설로 사용하지 않았다.

close 경계 49.895 ns를 네 호출에 균등 배분하면 한 호출은 12.474 ns,
DD 전체 1,083.659 ns의 1.15%다. 이는 **균등 비용을 가정한 규모 참고**이며,
scratch와 원본 close의 실제 비용이 같다는 근거나 개선 상한은 아니다.
공식 5-run에서 유의미한 개선이 확인되지 않아 채택하지 않았다.
전환 횟수를 줄일 수 없다는 결론이 아니라, 이번 통합 후보가 채택 조건을 충족하지 못했다는 결론이다.

## 4. 공식 after와 aggregate

### 탐색 1-run

`perf_dotnet_multi_linux_20260908_095610_ddpass1explore.txt`: `status: complete`, success 5,
fail 0. C 대비 DD aggregate는 61.20%로 before 62.51%보다 1.31%p 낮다.

| B | before 대비 처리량 | before 대비 latency | after/C |
|---|---:|---:|---:|
| 64 | +2.10% | +47.82% | 54.11% |
| 256 | −2.16% | −8.45% | 53.23% |
| 1024 | +0.39% | −2.33% | 59.45% |
| 4096 | −5.39% | +50.34% | 83.05% |
| 65536 | −3.38% | −11.65% | 56.16% |

§7.2에 따라 이 결과를 최종 채택·회귀 판정으로 사용하지 않는다.
탐색 시작 load 2.30, 실행 전후 runner/Core hash 동일. 실행 시간 28초.

### 판정 5-run

### report 집계 경계

`bindings/dotnet/perf/multi/run_benchmarks.sh:2246-2264`는
`rows[(pattern, transport, size_i, metric)] = (size, value)`로 매 반복의 같은 key를
덮어쓴다. 이 때문에 **r1net과 이번 after의 최종 RESULT는 다섯 번째 run 값**이다.
`META,runs,5`·`status: complete`는 5회 실행 완료를 증명하지만 median 집계를 증명하지 않는다.
C는 `bindings/c/perf/run_comparison.py:3695`의 `statistics.median(vals)`를 사용한다.

원시값은 각 complete report에 대응하는 `results/multi/tmp/*.result_data.csv`에 남아 있다.
이번 after는 **100개 metric key × 5개 표본 = 500개 원시 RESULT**를 모두 확인한 뒤
metric별 median을 계산했다. 최종 report에 출력된 RESULT는 100줄이며, Completion의
`actual_result_lines: 500`은 원시 수집 수다. 동일 검사를 r1net 원시 자료에도 적용했다.

러너 수정 제안은 같은 key의 값을 목록으로 모으고 표본 수 5를 확인한 뒤 C와 같은
median을 기록하는 것이다. 이 pass에서는 해당 러너 파일을 수정하지 않았다.
아래 주표의 before는 요청된 published r1net 수치를 유지한다. **따라서 주표는
before 마지막 반복과 after 5회 median의 비교라는 한계가 있다.** 이어지는 보조 표는
새 실행 없이 같은 r1net 표본 5개를 median으로 재집계해 경계를 맞춘다.

공식 after: `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260908_100735_ddpass1after5.txt`. `status: complete`, 5-run. 보존된 result_data.csv의 각 지표 5개 값의 median을 사용한다.

| pattern | before/C | after/C | after/before 처리량 변화 평균 | after/before latency 변화 평균 | after/C latency |
|---|---:|---:|---:|---:|---:|
| MULTI_DEALER_DEALER | 62.51% | 61.26% | -1.21% | +3.63% | 1.155x |
| MULTI_PUBSUB | 61.01% | 61.06% | +0.76% | +0.37% | 1.381x |
| MULTI_DEALER_ROUTER_REQREP | 60.45% | 61.35% | +1.60% | -3.46% | 2.260x |
| MULTI_ROUTER_ROUTER_REQREP | 63.10% | 62.15% | -1.66% | +4.91% | 2.880x |
| **전체 (20개 비율 산술평균)** | **61.77%** | **61.45%** | **-0.13%** | **+1.36%** | **1.919x** |

DD/PUBSUB 처리량은 msg/s, REQREP는 ops/s. latency는 **mean latency(ms)**다. pattern aggregate와 size별 threshold를 별도로 확인한다.

| pattern | B | before 처리량 | after 처리량 | 변화 | before latency | after latency | 변화 | after/C | size별 threshold |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| MULTI_DEALER_DEALER | 64 | 922,799.8 | 938,593.8 | +1.71% | 0.543048 | 0.342028 | -37.02% | 53.90% | 통과 |
| MULTI_DEALER_DEALER | 256 | 783,467.6 | 757,956.8 | -3.26% | 0.225853 | 0.212655 | -5.84% | 52.64% | 통과 |
| MULTI_DEALER_DEALER | 1024 | 707,535.0 | 758,056.4 | +7.14% | 0.221803 | 0.254210 | +14.61% | 63.45% | latency |
| MULTI_DEALER_DEALER | 4096 | 575,803.0 | 520,450.6 | -9.61% | 244.645636 | 389.409678 | +59.17% | 79.34% | 처리량,latency |
| MULTI_DEALER_DEALER | 65536 | 94,364.2 | 92,469.4 | -2.01% | 3.701712 | 3.228039 | -12.80% | 56.96% | 통과 |
| MULTI_PUBSUB | 64 | 1,021,947.0 | 1,082,689.0 | +5.94% | 1470.916127 | 1416.432135 | -3.70% | 46.05% | 통과 |
| MULTI_PUBSUB | 256 | 1,139,491.4 | 1,138,666.0 | -0.07% | 1490.596298 | 1548.775363 | +3.90% | 47.34% | 통과 |
| MULTI_PUBSUB | 1024 | 1,106,782.8 | 1,121,142.6 | +1.30% | 1380.880169 | 1341.638689 | -2.84% | 44.94% | 통과 |
| MULTI_PUBSUB | 4096 | 1,085,557.6 | 1,066,773.4 | -1.73% | 618.616924 | 610.835439 | -1.26% | 66.52% | 통과 |
| MULTI_PUBSUB | 65536 | 118,475.6 | 116,508.0 | -1.66% | 254.422496 | 269.062897 | +5.75% | 100.46% | 통과 |
| MULTI_DEALER_ROUTER_REQREP | 64 | 248,793.4 | 244,846.0 | -1.59% | 0.277080 | 0.264953 | -4.38% | 62.32% | 통과 |
| MULTI_DEALER_ROUTER_REQREP | 256 | 230,102.8 | 234,708.8 | +2.00% | 0.267460 | 0.265104 | -0.88% | 63.80% | 통과 |
| MULTI_DEALER_ROUTER_REQREP | 1024 | 231,749.0 | 235,981.4 | +1.83% | 0.276421 | 0.263674 | -4.61% | 63.41% | 통과 |
| MULTI_DEALER_ROUTER_REQREP | 4096 | 205,211.2 | 206,265.8 | +0.51% | 0.267301 | 0.270645 | +1.25% | 61.39% | 통과 |
| MULTI_DEALER_ROUTER_REQREP | 65536 | 31,992.4 | 33,674.4 | +5.26% | 6.151189 | 5.615617 | -8.71% | 55.83% | 통과 |
| MULTI_ROUTER_ROUTER_REQREP | 64 | 223,451.0 | 229,366.2 | +2.65% | 0.251279 | 0.251085 | -0.08% | 63.82% | 통과 |
| MULTI_ROUTER_ROUTER_REQREP | 256 | 227,939.2 | 219,078.0 | -3.89% | 0.260865 | 0.255745 | -1.96% | 64.66% | 통과 |
| MULTI_ROUTER_ROUTER_REQREP | 1024 | 216,875.8 | 224,044.8 | +3.31% | 0.274050 | 0.264981 | -3.31% | 68.21% | 통과 |
| MULTI_ROUTER_ROUTER_REQREP | 4096 | 190,762.2 | 187,206.6 | -1.86% | 0.278134 | 0.282511 | +1.57% | 62.65% | 통과 |
| MULTI_ROUTER_ROUTER_REQREP | 65536 | 32,414.4 | 29,654.8 | -8.51% | 5.648587 | 7.247766 | +28.31% | 51.41% | 처리량,latency |

### 같은 r1net 원시 5회 median을 사용한 보조 비교

공식 after: `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260908_100735_ddpass1after5.txt`. `status: complete`, 5-run. 보존된 result_data.csv의 각 지표 5개 값의 median을 사용한다.

| pattern | before/C | after/C | after/before 처리량 변화 평균 | after/before latency 변화 평균 | after/C latency |
|---|---:|---:|---:|---:|---:|
| MULTI_DEALER_DEALER | 61.73% | 61.26% | -0.57% | -0.79% | 1.155x |
| MULTI_PUBSUB | 61.44% | 61.06% | -0.32% | -0.06% | 1.381x |
| MULTI_DEALER_ROUTER_REQREP | 60.51% | 61.35% | +1.68% | -4.40% | 2.260x |
| MULTI_ROUTER_ROUTER_REQREP | 63.55% | 62.15% | -2.40% | +5.77% | 2.880x |
| **전체 (20개 비율 산술평균)** | **61.81%** | **61.45%** | **-0.40%** | **+0.13%** | **1.919x** |


보조 비교에서도 전체 처리량 변화는 **−0.40%**로 유의미한 개선이 없다.
DD 4096 B는 처리량 **−3.49%**, latency **+15.38%**이고,
RR REQREP 65536 B는 처리량 **−10.22%**, latency **+35.61%**다.
원시 5회 표본은 `/tmp/zlink-dotnet-dd-pass1/raw/`에 복사해 보존했다.
전체 보조 size 표는 `tables-median-before.md`, 수치와 실패 목록은
`comparison-{published,median}-before.json`에 있다.

공식 after 시작 load는 **3.59**, 실행 시간 **606초**다.
100개 size-run이 성공했으며 `status: complete`, `fail: 0`이다.
실행 전후 runner·Core hash와 후보 binding DLL의 바이트 일치를 확인했다.
실행을 추가 반복하거나 유리한 run을 선택하지 않았다.


## 5. 회귀 gate와 공개 API

- `ZLINK_CORE_SOURCE=release`, `ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.2`로
  `bash bindings/dotnet/tests/run_tests.sh` 실행: **232 passed, 0 failed, 0 skipped**, sample **7/7**.
- 기존 `test_hot_path_ownership_contract.cs`가 1·2·9·33-part, 반복 Message alias,
  invalid tail, native FINAL 실패, reply token 재사용과 원본 보존을 검증한다.
- Release build: warning/error 0. 후보의 public type·interface·member metadata **1,216줄 동일, diff 0줄**.
  `Contracts/` source diff도 0이다.
- 처리량 −5%·mean latency +10% threshold를 **pattern aggregate에 적용하면 네 pattern 모두 통과**한다.
  지정 before와 r1net median 보조 비교 모두 동일하다.
- **size별 threshold는 실패**다. 지정 before 대비 DD 1024/4096 B와 RR REQREP 65536 B,
  r1net median 대비 DD 4096 B와 RR REQREP 65536 B가 초과한다.
- aggregate gate만 사용하더라도 유의미한 처리량 개선이 없어 **기각**한다.
  관측 회귀를 이 cleanup 변경의 인과적 결과로 확정하지 않는다. 직접 변경되지 않은
  PUBSUB에서도 size별 변화가 관측되므로 시간대·run 변동을 분리하지 못한다.
- 최종 `bindings/dotnet/src` source diff **0줄**. 기각한 패치는
  `/tmp/zlink-dotnet-dd-pass1/rejected-candidate.patch`에 보존했다.
  후보 상태의 native 호출은 13→12/일반 전환 10→9였지만, **최종 원복 상태는 13→13/10→10회**다.

## 6. 비교 조건과 자료

before 자료는 사용자가 지정한 `r1net`의 .NET/C 4개 report씩이며 모두 `status: complete`, 5회 실행이다.
.NET의 마지막 반복값을 기록하는 집계 결함과 보조 재집계는 §4에 명시했다.
size별 after/C와 after/before 비율을 구하고 pattern aggregate는 5개 size 비율의 산술평균으로 계산한다.
전체 aggregate는 20개 size-pattern 비율의 산술평균이다. aggregate가 개별 size의 회귀를 가리지 않도록
모든 size에 회귀 threshold를 함께 적용한다.

- 실험한 라이브러리 파일: `bindings/dotnet/src/Zlink/Runtime/Messaging/RequestReplySupport.cs` — 기각 후 원복.
  이 작업이 남기는 repository 변경은 이 로그뿐이다. 기존·다른 job의 변경은 이 작업의 diff에 포함하지 않는다.
- builder·submission owner·원본 보존·scratch 정리 규칙은 전/후 동일하다. scratch cleanup loop의
  소유자는 후보에서 binding/Core 각 1개 → Core 1개로 통합했다.
  **수정 전/후 규칙 수: 후보의 배열 close 반복 정의 2→1, 기각·원복 후 2→2.**
  새 상태·pool·상한은 0개다.
- Core SHA-256: `72d508f73a5d261ff608c7ac49b5b55e7277e18e15fe4b5ac51affc0468012d3`.
- 새 Release binding before/after SHA-256:
  `da1c63535a6de242c09c03f249dca85add6cc62827bad1fbfbeda2a7bacaa499` /
  `ed768d04e00dae6865482db63a1168a9cfb41c3a65af7ef7c077ddd882dbc8e6`.
- 측정 자료: `/tmp/zlink-dotnet-dd-pass1/`의 `allocations.txt`, `builder-functions.tsv`,
  `probe/`, `api-{before,after}.txt`, `api.diff`, `tests.log`, `baseline.json`,
  `fixed-binaries.sha256`, `explore.sh`, `after.sh`와 각 실행 log.
- runner source는 수정하지 않았다. 별도 routed echo job으로 current DLL hash는 비용 지도 보존본과
  다르지만 Common 전체 IL 5,078줄과 DD·PUBSUB·REQREP 실행 클래스 IL 4,791줄은 동일하다.
  `runner-{Common,Multi}-{baseline,current}.il`에 metadata token을 해석한 비교본을 보존했다.
  r1net commit `63ec6187d9` 이후 해당 범위 source diff는 없고, 별도 변경은 routed SENDSEND 클래스다.
- perf 실행은 `--reuse-build`를 사용하며 이미 빌드된 runner의 binding DLL만 후보 build로 갱신한다.
  매 실행 전 `wait-for-idle-perf.sh` flock과 load≤5를 확인하고, runner/Core hash를 실행 전후 대조한다.
- Core·Framework·다른 언어·정책·spec·계획서·decisions 수정과 commit/push는 수행하지 않았다.
  다른 job의 source와 기존 untracked 파일을 보존한다.

소유 계층: binding의 temporary message lifetime, Core의 native array close.
Spec 조항: Core message §zlink_multipart_close, 공통 binding operation builder·실패 시 원본 보존.
교차언어 대조: Framework runtime 변경이 아니므로 의무 대조 대상이 아니다. 고정 Core의 공개 계약과 바이너리를 대조했다.
변경 분류: 실험 후보 B — 기존 내부 cleanup의 반복 P/Invoke 경계 통합. 공개 계약과 오류·ownership 동작 변화 없음. 최종 runtime 변경 없음.

첫 큐 진입은 대기 중 다른 job의 runner 재빌드로 시작 전 hash 검사가 실패했다.
benchmark를 시작하지 않았으며 report도 생성되지 않았다. 변경된 관련 IL의 동일성을
재확인하고, 중단된 이 작업의 실행이 남긴 lock grace holder를 종료한 뒤 공용 큐에 다시 진입했다.

### 최종 artifact 확인

소스 원복 뒤 perf 출력 DLL의 복구도 공용 lock을 기다려 수행하도록 했다.
그 사이 다른 job이 binding을 재빌드해 candidate DLL hash가 바뀌었으므로,
복구 guard가 덮어쓰기를 중단했다. 새 DLL을 검사한 결과 `RequestReplySupport`의
전체 method IL은 baseline과 같고, 공개 API 1,216줄도 동일했다. 따라서 이 출력을 보존했다.

- 최종 perf 출력 binding SHA-256: `1dec3e3b9983a5c7786c00142f4838be469ce300fa000b0a765c646f6fded112`.
- 근거: `/tmp/zlink-dotnet-dd-pass1/restore-verification.json`, `restored-{baseline,current}.il`,
  `api-restored.txt`, `restore.log`.
- 최종 `bindings/dotnet/src`는 현재 HEAD와 r1net commit `63ec6187d9` 양쪽에 대해 diff 0이다.
  새 runtime code·public API·Core binary 변경은 남아 있지 않다.
