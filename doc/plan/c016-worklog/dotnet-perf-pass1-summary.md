# .NET 바인딩 hot-path pass 1 결과

- 작업 트리: `/home/hep7hep7/project/zlink-wt-dotnet-perf`, detached HEAD 유지. commit/push/reset/checkout/stash 없음.
- 소스 변경: `bindings/dotnet/**` 5개 파일. Core, 다른 binding, spec, repository doc 변경 없음. 기존 `core/build`, `core/build-dev` symlink 유지; Core configure/build/clean 없음.
- 최종 Core: `core/build/lib/libzlink.so.0.17.0`, SHA-256 `7a2daf1dc636cc1329f34caec6fde331a7686af7739e6eb86ab9abaa566eb1ca`.
- 공식 after: multi/tcp, 100 clients, 각 64/256/1024/4096/65536B, 5초, 1 run, **20/20 complete**. 공식 after는 한 번만 수행했다. 아래 별도 계측 run은 성능 판정에 사용하지 않았다.
- 정상 noncancelable SEND의 CompletionOwner 할당은 약 856B/호출에서 0B로 감소했다. 공개 builder, pending Task, 수신 collection의 수명은 유지한다.
- 처리량 목표는 모두 미달이다. DD는 완화 최소 64%를 넘었지만 목표 85%와 latency 상한을 충족하지 못했다.

## 원인과 변경

| 비용 위치 | before 계측·코드 근거 | 변경 및 남은 비용 |
|---|---|---|
| 즉시 SEND completion 등록 | `CompletionOwner.cs` before 45–49: 메시지마다 entry/Task/GCHandle/registry 등록. TCP 64B binding 856B/호출 | 현재 36–107: noncancelable SEND는 ID 0 성공 때 `Task.CompletedTask`; 실제 토큰이 나온 뒤에만 entry 등록. 취소 가능한 호출은 기존 선등록 유지 |
| native submit closure | `CompletionOwner.cs` before 457–528의 capturing delegate | 현재 477–576의 struct submitter가 callback 상태를 stack에 보관. part별 Core 함수·flags·target·FINAL 순서 유지 |
| 실패 소유권 보존 | `RequestReplySupport.cs` before 211–225: `CloneParts`의 Message 배열·wrapper와 추가 init/move. 2-part 248B, native 12회/submit | 현재 219–255: 전체 part를 native scratch에 `zlink_msg_copy`한 뒤 기존 MORE/FINAL 제출. wrapper 할당 0B, native 10회. 실패 시 원본 보존, 성공 때만 원본 소비 |
| 수신 임시 native 배열 | `SocketKernel.ReceiveCore.cs` before 402–407: 각 multipart에 Array.Resize. 최소 DD 수신 부분은 352B/msg | native header 배열만 ArrayPool로 관리하고 성공 adopt/실패 close 뒤 반환. 공개 collection identity 유지. 수신 부분 72B/msg |
| completion identity | `CompletionOwner.cs` before 417–419, 453: GCHandle alloc/free | 현재 425–474: socket 수명 동안 증가하는 opaque context. registry가 entry를 참조하며 context를 재사용하지 않음 |
| 러너 환경변수 조회 **별도 버그** | `PerfSocketIo.cs:7` getter가 PERF_PART_COUNT를 호출마다 조회. 값 `2`일 때 24B/조회 | 프로세스마다 고정하는 getter 초기값으로 변경. scheduler/drain/fairness 변경 없음. 이 버그 수정의 효과를 바인딩 할당 감소에 합산하지 않음 |

### 대안 판정과 계약 보존

- 즉시 SEND에는 기존 선등록 유지와 토큰 반환 뒤 등록을 비교해 후자를 채택했다. `.NET`의 기존 `_submitSync`가 submit 및 drain을 모두 직렬화하므로 선행 WRITABLE을 별도 map에 보관할 필요가 없다. close가 시작된 뒤에도 이미 진입한 submit의 토큰을 등록하고 기존 lifecycle 정리로 넘긴다.
- wrapper를 pool에 넣는 대안은 사용하지 않았다. 내부 native scratch를 재사용하면서 public wrapper/Task/completion identity의 재사용을 늘리지 않았다. .NET direct 2-part native submit은 시도하지 않았고, 모든 part 수에 같은 generic 보존 알고리즘을 사용한다.
- Core는 실패한 part도 소비하므로 **제출 전 공유 native snapshot 자체는 필요**하다(Core socket spec 919–923). 제거한 것은 managed clone wrapper와 추가 init/move다. caller 원본을 직접 파괴해서 실패 소유권을 바꾸지 않았다.
- `_submitSync` 및 close 확인 락은 유지한다. 따라서 전체 공개 SEND가 무락이라고 주장하지 않는다. O(1) registry, 기존 drain-to-NO_DATA와 runtime/public owner 전환, WRITABLE 재제출 및 error 분류도 유지한다.
- REQREP의 필수 pending Task/entry는 남는다. 최소 public-poller 경로 binding submit은 656→288B/호출, 실제 러너에서는 runtime pump 등의 비용을 포함해 약 781→414B/호출이다.

## 비용 계측

`dotnet-trace`/`dotnet-counters`가 PATH에 없어 EventPipe 없이 임시 `GC.GetAllocatedBytesForCurrentThread`, `GC.CollectionCount`, native 메시지 함수 호출 카운터를 사용했다. synchronous 제출 구간만 thread-local 할당량으로 측정했다. 계측 소스는 최종 tree에서 제거했다. 계측 run의 처리량은 공식 before/after와 섞지 않는다.

### TCP 64B 실제 러너의 제출 구간

러너 환경변수 캐시를 적용하지 않은 상태로 library before/after를 대조했다. 중첩 scope이므로 아래 할당값을 서로 더하면 안 된다. Gen0는 해당 프로세스 전체 값이며 진단 중 완료한 메시지 수가 다르다.

| 경로 | before 호출 수 | before B/호출 | after 호출 수 | after B/호출 | 메시지 API P/Invoke before→after | client Gen0 before→after |
|---|---:|---:|---:|---:|---:|---:|
| DD binding SendAsync | 1,179,625 | 856.0 | 1,683,876 | 0.0 | 12→10 | 6→1 |
| DD runner send helper | 1,179,625 | 992.0 | 1,683,876 | 136.0 | 13→11 | 6→1 |
| DR REQREP binding RequestAsync | 199,400 | 781.0 | 303,100 | 414.1 | 12→10 | 1→1 |
| DR REQREP runner SubmitAsync | 199,400 | 901.1 | 303,100 | 534.1 | 15→13 | 1→1 |

before 제출 구간 할당의 약 86–87%가 binding 안에 있었다. DD 서버 전체 할당도 진단 수신 건수로 정규화하면 약 400→120B/msg로 줄었다(환경변수 캐시 제외).

### 최소 C-like 2-part 경로

같은 Core, inproc, 64B+빈 tail, 같은 스레드에서 send/recv 또는 request/직접 reply를 수행했다. 각 모드 1,000회 준비 후 20,000회를 집계했다. .NET REQREP는 public poller로 completion을 진행해 multi 러너의 scheduler 비용과 분리했다.

| 경로 | managed B/msg 또는 B/op before→after | 메시지 API P/Invoke before→after | C 직접 API 호출/건 |
|---|---:|---:|---:|
| .NET DD Async, 직접 public builder | 1,320→184 | 29→27 | 13 |
| .NET DD, perf send helper | 1,320→184 | 29→27 | 13 |
| .NET DD, 동기 Submit | 1,080→184 | 29→27 | 13 |
| .NET DR REQREP, public poller | 1,920→1,024 | 48→44 | 17 |

C mini의 managed allocation/Gen0/P/Invoke는 각각 0이다(C 코드이므로 managed runtime 경계가 없음). **C native heap allocation을 0으로 측정했다는 뜻이 아니다.** native heap malloc 횟수는 이 pass에서 계측하지 않았다. P/Invoke 카운터는 msg/send/recv/request/reply/completion 함수 23개 overload를 집계하며 poller 생성·대기 및 errno 조회는 제외한다. .NET 최소 REQREP에는 표 외에 public poller wait가 1회/op 있다. 이 범위 차이 때문에 native API 수의 비율을 전체 CPU 비용 비율로 해석하지 않는다.

동일한 .NET mini 전체 실행의 Gen0는 12→3이었다. SEND Async와 perf helper의 할당량이 같고 바인딩 개선으로 함께 감소했으므로, 제출 구간의 큰 할당 회귀는 러너 helper보다 binding에 있었다.

### 러너 버그의 독립 계측

`PERF_PART_COUNT=2`, getter 1,000,000회: managed allocation 24,001,976→1,976B. 차이는 정확히 **24B/조회 제거**다. 공통 1,976B는 출력 준비 등 고정 비용이다. 소요 시간은 186.63→1.05ms(단일 getter micro 측정)였으며 이를 library 처리량 이득으로 합산하지 않는다. 공식 after에는 library 변경과 이 러너 버그 수정이 함께 포함된다. 각 library 변경의 독립 처리량 기여도는 분리 측정하지 않았다.

## 공식 before / after

처리량은 DD/PUBSUB msg/s, REQREP ops/s다. latency 단위는 ms다. aggregate는 사용자 before와 같은 **크기별 C 비율의 산술평균**이며 전체 latency 합의 비율이 아니다.

| 패턴 | before/C 처리량 평균 | after/C 처리량 평균 | 목표 | before/C latency 평균 | after/C latency 평균 |
|---|---:|---:|---:|---:|---:|
| DEALER_DEALER | 44.70% | 66.67% | 85% | 18.22x | 86.49x |
| DEALER_ROUTER_REQREP | 51.55% | 58.09% | 70% | 0.54x | 0.50x |
| ROUTER_ROUTER_REQREP | 53.60% | 57.15% | 70% | 0.63x | 0.65x |
| PUBSUB | 44.78% | 71.40% | 85% | 1.41x | 1.25x |

| 패턴 | B | C | .NET before | .NET after | 변화 | after/C |
|---|---:|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 1,121,870.2 | 269,734.4 | 493,070.2 | +82.8% | 44.0% |
| DEALER_DEALER | 256 | 1,056,107.4 | 289,147.4 | 444,870.0 | +53.9% | 42.1% |
| DEALER_DEALER | 1024 | 951,863.2 | 295,819.0 | 448,516.4 | +51.6% | 47.1% |
| DEALER_DEALER | 4096 | 376,547.0 | 192,053.6 | 299,868.2 | +56.1% | 79.6% |
| DEALER_DEALER | 65536 | 79,794.4 | 71,806.4 | 96,157.6 | +33.9% | 120.5% |
| DEALER_ROUTER_REQREP | 64 | 178,391.6 | 48,854.4 | 71,684.4 | +46.7% | 40.2% |
| DEALER_ROUTER_REQREP | 256 | 162,481.6 | 60,749.6 | 69,629.4 | +14.6% | 42.9% |
| DEALER_ROUTER_REQREP | 1024 | 160,834.6 | 59,631.2 | 70,925.6 | +18.9% | 44.1% |
| DEALER_ROUTER_REQREP | 4096 | 133,568.8 | 59,853.8 | 67,954.2 | +13.5% | 50.9% |
| DEALER_ROUTER_REQREP | 65536 | 23,913.4 | 26,565.0 | 26,890.2 | +1.2% | 112.4% |
| ROUTER_ROUTER_REQREP | 64 | 178,778.4 | 42,084.2 | 58,694.0 | +39.5% | 32.8% |
| ROUTER_ROUTER_REQREP | 256 | 160,512.6 | 53,113.2 | 56,113.0 | +5.6% | 35.0% |
| ROUTER_ROUTER_REQREP | 1024 | 145,843.8 | 53,831.4 | 68,768.2 | +27.7% | 47.2% |
| ROUTER_ROUTER_REQREP | 4096 | 112,588.0 | 53,548.0 | 58,802.8 | +9.8% | 52.2% |
| ROUTER_ROUTER_REQREP | 65536 | 19,618.8 | 24,897.6 | 23,263.0 | -6.6% | 118.6% |
| PUBSUB | 64 | 741,620.0 | 189,161.4 | 469,089.0 | +148.0% | 63.3% |
| PUBSUB | 256 | 645,984.0 | 242,313.6 | 443,355.8 | +83.0% | 68.6% |
| PUBSUB | 1024 | 731,889.6 | 269,122.4 | 498,161.2 | +85.1% | 68.1% |
| PUBSUB | 4096 | 558,257.8 | 248,704.8 | 430,828.4 | +73.2% | 77.2% |
| PUBSUB | 65536 | 64,123.2 | 51,017.6 | 51,232.6 | +0.4% | 79.9% |

| 패턴 | B | C mean ms | before mean ms | after mean ms | after/C latency |
|---|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 0.282498 | 0.924 | 0.663 | 2.35x |
| DEALER_DEALER | 256 | 1.871429 | 1.943 | 0.478 | 0.26x |
| DEALER_DEALER | 1024 | 1.162395 | 99.840 | 498.454 | 428.82x |
| DEALER_DEALER | 4096 | 641.529613 | 393.318 | 296.083 | 0.46x |
| DEALER_DEALER | 65536 | 16.944583 | 5.067 | 10.061 | 0.59x |
| DEALER_ROUTER_REQREP | 64 | 1.025241 | 0.604 | 0.456 | 0.44x |
| DEALER_ROUTER_REQREP | 256 | 0.952214 | 0.465 | 0.463 | 0.49x |
| DEALER_ROUTER_REQREP | 1024 | 0.952694 | 0.476 | 0.453 | 0.48x |
| DEALER_ROUTER_REQREP | 4096 | 1.216478 | 0.483 | 0.483 | 0.40x |
| DEALER_ROUTER_REQREP | 65536 | 2.285665 | 1.631 | 1.550 | 0.68x |
| ROUTER_ROUTER_REQREP | 64 | 0.728936 | 0.681 | 0.638 | 0.88x |
| ROUTER_ROUTER_REQREP | 256 | 0.901242 | 0.506 | 0.627 | 0.70x |
| ROUTER_ROUTER_REQREP | 1024 | 0.850048 | 0.532 | 0.490 | 0.58x |
| ROUTER_ROUTER_REQREP | 4096 | 1.145677 | 0.526 | 0.533 | 0.47x |
| ROUTER_ROUTER_REQREP | 65536 | 2.765579 | 1.564 | 1.713 | 0.62x |
| PUBSUB | 64 | 1477.281274 | 1382.220 | 1542.151 | 1.04x |
| PUBSUB | 256 | 1851.302441 | 1530.340 | 1804.502 | 0.97x |
| PUBSUB | 1024 | 1254.081383 | 1975.714 | 1480.227 | 1.18x |
| PUBSUB | 4096 | 414.599545 | 891.117 | 618.570 | 1.49x |
| PUBSUB | 65536 | 190.925062 | 296.114 | 301.896 | 1.58x |

### 보고서와 실행 환경

- 공식 after: `/home/hep7hep7/project/zlink-wt-dotnet-perf/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260905_113139.txt`.
- 실행: `export ZLINK_CORE_SOURCE=local; source bindings/tools/local_core_runtime.sh; zlink_export_local_core_runtime` 뒤 요청한 명령 그대로 실행했다. 모든 .NET build는 -m:1, JOBS/ZLINK_BUILD_JOBS/CMAKE_BUILD_PARALLEL_LEVEL 상한 3.
- 공식 측정 동안 다른 build/test/profile을 실행하지 않았다. 30초마다 기록한 load average는 `dotnet-perf-pass1-progress.md`에 있다. 공식 측정은 2026-09-05 11:31:39부터 약 169초였다.
- paired before는 ignored report가 메인 트리에 있어 해당 경로에서 읽었다. 재측정하지 않았다.
  - C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_110735_p1dotnet.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260905_110803.txt`.
  - C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_110836_p1dotnet.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260905_110902.txt`.
  - C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_110955_p1dotnet.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260905_111021.txt`.
  - C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_111113_p1dotnet.txt`; .NET: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260905_111140.txt`.

## Gate

- `bash bindings/dotnet/tests/run_tests.sh`: **208/208 PASS**, sample **7/7 PASS**, exit 0.
- 변경 관련 contract 31건 × 5회: 모두 PASS. 대상: `test_hot_path_ownership_contract`, `test_pull_completion_contract`, `test_request_writable_contract`, `test_completion_lifecycle_regressions`, `test_contract_b_regressions`.
- 새 ownership test 10건: 1/2/9/33-part 반복 원본의 성공 소비, invalid 마지막 part의 prefix 보존·미staging, native FINAL no-route 실패의 원본 보존. 기존 테스트는 writable 재거절, 취소/close/context 종료 및 public poller 소유권을 검증한다.
- `git diff --check`: PASS. `Contracts/**` 및 공개 API signature diff 없음. 임시 계측·미니벤치 소스 제거. 계측 제거 후 최종 Release perf artifact 재빌드도 PASS(경고·오류 0).
- gate·반복·계측 로그: `/home/hep7hep7/project/zlink-work/c016/dotnet-profile`.

## 변경 파일

- `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs`
- `bindings/dotnet/src/Zlink/Runtime/Messaging/RequestReplySupport.cs`
- `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.ReceiveCore.cs`
- `bindings/dotnet/perf/common/Zlink.BindingBench.Common/PerfSocketIo.cs` — 별도 러너 버그
- `bindings/dotnet/tests/Zlink.Tests/test_hot_path_ownership_contract.cs`

## 소유 계층과 spec gap

- 소유 계층: .NET binding의 completion registry·native 임시 메시지 저장소. Core가 admission, wait token 발행, WRITABLE 및 terminal 결과를 계속 소유한다.
- Spec: `core/doc/spec/core/socket/README.ko.md:897` “Part send와 pending admission”, `:1101` “Completion pull과 ownership”; `bindings/doc/spec/dotnet/README.ko.md` “표준 인터페이스 규칙”. 결과불문 native part 소비를 전제로 관리 원본의 실패 소유권을 보존한다.
- 교차언어 대조: C++ pass 1의 즉시 admission 등록 지연 및 pass 2의 내부 저장소 개선과 같은 비용을 제거했다. .NET은 기존 submit/drain 공통 lock 덕분에 C++의 선행 WRITABLE 보류 map이 필요 없으며, GCHandle 대신 registry-rooted 고유 context가 구조적 차이다.
- 변경 분류: **B — 기존 hot-path 비용 결함의 내부 개선**. 공개 API, ownership, error, timeout, cancellation 및 측정 의미 변경 없음. **spec gap 없음**.

## BLOCKERS

- 작업·기능 gate blocker 없음. 성능 목표 충족으로 판정을 닫을 수는 없다.
- 진행 로그의 3분 주기는 조사·요약 구간에서 4회 초과했다(182/217/224/268초, 최대 4분 28초). 공식 after 측정 구간은 30초마다 load average를 기록했다.
- DD 처리량 평균 66.67%는 완화 최소 64%를 넘었지만 목표 85% 미달. 1024B 평균 latency 99.840→498.454ms, C 대비 428.82x 때문에 aggregate latency 86.49x로 상한 3.0x를 초과한다.
- DR REQREP 58.09%, RR REQREP 57.15%는 목표 70% 미달. PUBSUB 71.40%는 목표 85% 미달.
- RR REQREP 65536B 처리량 -6.6% 회귀를 그대로 남긴다. DD 65536B, RR 256/4096/65536B, PUBSUB 64/256/65536B latency 악화도 위 표에 남겼다. 단일 after의 분산과 원인별 기여도는 확정하지 않았다.
- 아직 전체 공개 경로의 무할당·무락을 달성하지 않았다. builder/collection, 필수 pending entry/Task 및 runtime pump 비용은 남는다. GC 계측은 CPU 전체 profile과 native malloc profile을 대신하지 않는다.

### DD 러너 분포 진단

공식 after의 DD latency 악화를 확인한 뒤, 처리량을 다시 고르기 위한 재측정 대신 client별 send/await 분포만 임시 계측했다(64/1024B, 100 clients, 5초). 진단 report의 처리량은 위 표에 사용하지 않았다.

| 크기 | 실제 송신 client 수 | 총 송신 | 첫 client 송신 | 첫 client 비중 | 미완료 Task를 만난 횟수 |
|---|---:|---:|---:|---:|---:|
| 64B | 1/100 | 2,485,161 | 2,485,161 | 100.0% | 0 |
| 1024B | 100/100 | 2,649,822 | 651,305 | 24.6% | 2,602 |

원인은 `bindings/dotnet/perf/multi/Zlink.BindingBench.Multi/src/PerfMultiDealerDealerClient.cs:103`의 await와 `:109`의 task 생성 루프다. 첫 `SendLoopAsync`가 즉시 완료 Task만 await하면 중단하지 않고 deadline까지 실행하여 뒤 client의 시작을 늦춘다. 64B profile에서도 실제 backpressure 대기가 없었고, 분포 진단으로 1개 client 편중을 확인했다. 1024B는 backpressure 이후 다른 client가 시작되지만 송신량이 불균등하다.

**이 분포 결함의 존재는 확인했지만, 1024B latency 악화의 전부를 이 원인 하나로 확정하지는 않았다.** 스케줄링, 수신 처리량 및 backlog 상호작용의 기여도는 별도 검증이 필요하다. 사용자가 library 효과와 합산하지 말라고 한 scheduler/fairness 변경에 해당하므로 이번 pass에서 고치지 않았다. 100개 연결을 만든다는 설정만으로 100개가 공평하게 송신하는 것은 아니며, C 대비 throughput·latency gap 해석에 이 한계가 남는다.

## 계측 자료

- `/home/hep7hep7/project/zlink-work/c016/dotnet-profile/mini-before.log`, `mini-after-library.log`, `native-mini.log`
- `/home/hep7hep7/project/zlink-work/c016/dotnet-profile/before-run.log`, `after-library-run.log` 및 서버/클라이언트 PROFILE 로그
- `/home/hep7hep7/project/zlink-work/c016/dotnet-profile/env-before.log`, `env-after.log`
- `/home/hep7hep7/project/zlink-work/c016/dotnet-profile/distribution/` — 공식 after에 합산하지 않는 DD 분포 진단
- 재현용 임시 mini 소스와 계측 스크립트는 위 외부 디렉터리에만 보관했다. `before-source.json`은 계측 이전 원본, `backup.json`은 library 변경 뒤 계측 제거용 원본이다.
