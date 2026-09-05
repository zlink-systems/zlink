# .NET binding hot-path pass 2

## 결과와 비교 제한

- 내부 closure 비용을 줄이는 후보를 채택했다. 최소 public API REQREP의 managed allocation은 **1,024→640B/op(-37.5%)**다. reply 제출은 **384→32B**, public drain owner가 있는 request 제출은 **320→288B**다.
- 공식 after는 요청한 multi/tcp, 100 clients, 64/256/1024/4096/65536B, 5초, 1 run으로 한 번 실행했다. **20/20 complete, 실패 0**. 반복 측정으로 값을 고르지 않았다.
- **pass 1 및 기존 C와 Core artifact가 다르다.** pass 1 summary의 SHA-256은 `7a2daf1dc636cc1329f34caec6fde331a7686af7739e6eb86ab9abaa566eb1ca`, 이번 after의 shared local Core는 `93ad7f1161156d34aecf5550f25d1d8630d18cb657eee24eecc135e578050918`이다. 공유 파일 mtime은 2026-09-05 11:44:48 KST다. 이번 작업에서 Core를 build하거나 수정하지 않았다. 아래 throughput/latency 변화는 **서로 다른 artifact 사이의 관측값**이며 순수한 binding 효과나 성능 목표 충족 판정으로 확정하지 않는다.
- before mini 로그는 11:46:41 KST에 작성되었으며 공식 after 전 확인한 shared binary는 위 11:44:48 artifact였다. mini 시간도 당시 외부 부하가 달라 인과적 처리량 개선 근거로 삼지 않는다. 고정적인 allocation 차이를 후보 채택 근거로 사용했다.

## 후보 검토

범위: CompletionOwner의 submit/register/drain/runtime pump/lifecycle, RequestReplySupport의 staging/ownership, SocketKernel의 receive/send/multipart/close 경계, Poller ownership, SocketCallbacks의 공개 socket 검사, NativeMethods의 message/submit/completion import, 실제 multi DD·REQREP·PUBSUB 호출 경로. perf scheduler/drain/fairness는 변경하지 않았다.

| 후보 | 계측 근거·예상 효과 | 계약 보존 방법 | 가이드 §4 해당 | 판정 |
|---|---|---|---|---|
| reply native submit closure를 struct로 전달 | reply 제출 384→32B/op: closure·delegate 352B 제거(-91.7%). 전체 REQREP 1,024→672B(-34.4%) | 기존 INativePartSubmitter와 SubmitPreservingOnFailure를 사용. RID/token, part별 native 함수, MORE/FINAL, original 실패 보존을 유지 | 없음: direct 2-part submit이 아니며 모든 part 수에 같은 generic 경로 | **채택** |
| 이미 pump가 있거나 public owner가 있는 StartRuntimePump 조기 반환의 closure 지연 | 실제 private 메서드 delegate를 통한 2,000,000회 계측 32→0B, 76.96→69.68ns/call(-9.5%). public API request 320→288B(-10%). 최종 전체 640B/op | capture 변수의 lexical scope만 조기 반환 뒤로 옮김. 기존 Task.Run, owner/epoch 값, lock, 시작/종료 조건과 wake를 유지 | 없음: scheduler 교체·spin·public Task 재사용 없음 | **채택** |
| C++ inline 첫 completion entry 대응 | Dictionary 200,000 add/lookup/remove가 정상 상태 **0B**, 17.61ns/cycle. baseline REQREP 11,122.70ns 중 약 0.16% | 기존 Dictionary가 이미 capacity/node storage를 재사용. 새 slot은 모든 lifecycle snapshot/Count를 이중 저장소로 바꿔야 함 | 없음 | **no-go: 5% 근거 없음·상태 증가** |
| C++ resume-slot bundle에 대응한 entry/TaskCompletionSource 합침 | TCS+Task 96B, 일반 Task 72B를 별도 계측. TCS wrapper만 제거하는 이론적 상한 24B는 baseline 1,024B의 2.34% | 공개 pending Task identity·RunContinuationsAsynchronously·취소 수명은 유지해야 함. C++ control block/aliasing shared_ptr에 대응하는 구조가 .NET에는 없음. 일반 Task 측정은 allocation 분해용이며 대체 구현이 아님 | Task/entry pool은 §4 금지 | **no-go: 5% 근거 없음; pool은 재시도하지 않음** |
| C++ reply adopt-in-place 대응 | native empty init+move는 0B, 21.33ns/pair. 2-part에서 전부 없애도 약 42.66ns(전체 baseline의 0.38%)라는 낮은 표본 비용 | .NET MoveReply는 이미 result 배열 원소에 wrapper를 직접 넣음. native vector는 completion_close가 남은 part와 allocator base를 소유하므로 opaque header를 단순 zero로 두는 대체는 검증된 move 계약과 같다고 볼 수 없음 | wrapper pool 확대는 §4 금지 | **no-go: 5% 근거 없음; native ownership 변경 불필요** |
| 소비된 scratch의 empty msg_close 제거 | empty close+init 0B, 19.54ns/pair. 2-part request+reply의 close 4회를 이 표본으로 넉넉히 잡아도 약 78.16ns(전체 baseline의 0.70%) | 실패 전 미제출 scratch는 반드시 close해야 함. 현재 일괄 cleanup은 예외 경계를 단순하게 유지 | 없음 | **no-go: 5% 근거 없음; submitted-count 상태 추가 불필요** |
| DD/PUBSUB 수신 collection 재사용·direct 2-part·lock 삭제·러너 재편 | pass 1 계측 수신 72B/msg, 이번 reply drain 40B/op는 공개 collection/배열 비용. native staging은 이미 stack/ArrayPool | public collection identity, submit→token publication→drain 및 close 직렬화 보존. 필수 ownership synchronization을 삭제하지 않음 | public wrapper pool·direct 2-part·scheduler/drain 변경은 금지 | **contract no-go: 금지 후보는 재실험하지 않음** |

micro의 ns 값은 비용 규모 참고이며 다른 경로의 시간으로 선형 환산한 효과를 확정하지 않는다. 금지 후보는 구현하거나 benchmark로 재시도하지 않았다. 정상 경로의 mutable 상태·timer·registry를 추가하지 않았다.

## 변경 파일

- `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:253`: reply의 capturing delegate를 기존 generic 제출 경로의 private ReplyPartSubmitter로 교체. scalar/token/256-byte RID는 stack에 있고 submit 동안만 유효하다. shared snapshot, native 호출 수와 반환값 처리, 실패 시 관리 원본 보존은 동일하다.
- 같은 파일 `:655`: pump callback captures를 실제 Task.Run이 필요한 블록으로 제한. 기존 Task.Run 호출 자체와 epoch 기반 lifecycle 동작은 동일하다. 새 helper 계층 없이 lexical block만 사용했다.
- `bindings/dotnet/tests/Zlink.Tests/test_hot_path_ownership_contract.cs`: 1/2/9/33-part 반복 원본 reply의 성공 소비·wire part 수·spent-token typed 실패/원본 보존, 2/9/33-part invalid tail의 prefix·token 보존과 정상 재제출을 검증하는 7건 추가.

ReplyPartSubmitter는 pass 1에 있는 INativePartSubmitter를 재사용한다. callback 인자와 같은 의미를 generic value로 전달해야 heap closure를 없앨 수 있어 private struct를 사용했다. 새로운 public API나 별도 ownership policy를 만들지 않았다.

## 최소 경로 before / after

같은 스레드의 inproc, 64B+빈 tail, public poller completion, warmup 20,000회 뒤 200,000회. GC.GetAllocatedBytesForCurrentThread로 builder 생성 밖의 submit과 poller drain을 분리했다. 전체 범위에는 builder/receive/result disposal이 포함되고 scope는 중첩되므로 더하지 않는다.

| 범위 | before B/op | reply 변경 뒤 B/op | 최종 B/op | 최종 변화 |
|---|---:|---:|---:|---:|
| request SubmitAsync | 320 | 320 | 288 | -10.0% |
| reply Submit | 384 | 32 | 32 | -91.7% |
| public poller Wait/drain | 40 | 40 | 40 | 0% |
| 전체 REQREP | 1,024 | 672 | 640 | -37.5% |

| 범위 | before ns/op | reply 변경 뒤 ns/op | 최종 ns/op |
|---|---:|---:|---:|
| request | 2,497.27 | 3,086.79 | 1,702.76 |
| reply | 1,886.18 | 2,272.51 | 1,244.33 |
| drain | 3,590.66 | 4,389.12 | 2,481.67 |
| 전체 REQREP | 11,122.70 | 13,845.79 | 7,776.37 |

중간 run 시간 악화를 포함해 전부 기록했다. 시간 차이는 외부 부하와 run 분산을 분리하지 못해 성능 판정에서 제외한다. 할당 감소는 확인했지만 실제 TCP 처리량 5% 개선을 미리 확정하지 않는다. 최종 native message 호출 수는 pass 1과 같고, Task·entry·public collection pool을 추가하지 않았다.

## 공식 after 관측값

아래 변화와 after/C는 artifact가 다른 historical 비교다. DD/PUBSUB 단위는 msg/s, REQREP는 ops/s. 패턴 평균은 size별 ratio의 산술평균이다.

| 패턴 | pass1/C 평균 | pass2/C 평균 | pass2/pass1 평균 | 목표 | 판정 |
|---|---:|---:|---:|---:|---|
| DEALER_DEALER | 66.67% | 67.94% | 108.06% | 85% | artifact 불일치로 인과·목표 판정 보류 |
| DEALER_ROUTER_REQREP | 58.09% | 55.28% | 94.21% | 70% | artifact 불일치로 인과·목표 판정 보류 |
| ROUTER_ROUTER_REQREP | 57.15% | 52.26% | 99.39% | 70% | artifact 불일치로 인과·목표 판정 보류 |
| PUBSUB | 71.40% | 63.51% | 89.01% | 85% | artifact 불일치로 인과·목표 판정 보류 |

| 패턴 | B | 기존 C | pass1 after | pass2 after | 변화 | pass2/C |
|---|---:|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 1,121,870.2 | 493,070.2 | 399,981.0 | -18.88% | 35.65% |
| DEALER_DEALER | 256 | 1,056,107.4 | 444,870.0 | 768,320.2 | +72.71% | 72.75% |
| DEALER_DEALER | 1024 | 951,863.2 | 448,516.4 | 460,605.4 | +2.70% | 48.39% |
| DEALER_DEALER | 4096 | 376,547.0 | 299,868.2 | 283,088.4 | -5.60% | 75.18% |
| DEALER_DEALER | 65536 | 79,794.4 | 96,157.6 | 85,951.2 | -10.61% | 107.72% |
| DEALER_ROUTER_REQREP | 64 | 178,391.6 | 71,684.4 | 69,751.0 | -2.70% | 39.10% |
| DEALER_ROUTER_REQREP | 256 | 162,481.6 | 69,629.4 | 62,042.4 | -10.90% | 38.18% |
| DEALER_ROUTER_REQREP | 1024 | 160,834.6 | 70,925.6 | 66,343.0 | -6.46% | 41.25% |
| DEALER_ROUTER_REQREP | 4096 | 133,568.8 | 67,954.2 | 62,972.4 | -7.33% | 47.15% |
| DEALER_ROUTER_REQREP | 65536 | 23,913.4 | 26,890.2 | 26,476.0 | -1.54% | 110.72% |
| ROUTER_ROUTER_REQREP | 64 | 178,778.4 | 58,694.0 | 64,325.0 | +9.59% | 35.98% |
| ROUTER_ROUTER_REQREP | 256 | 160,512.6 | 56,113.0 | 67,587.8 | +20.45% | 42.11% |
| ROUTER_ROUTER_REQREP | 1024 | 145,843.8 | 68,768.2 | 69,452.0 | +0.99% | 47.62% |
| ROUTER_ROUTER_REQREP | 4096 | 112,588.0 | 58,802.8 | 54,187.8 | -7.85% | 48.13% |
| ROUTER_ROUTER_REQREP | 65536 | 19,618.8 | 23,263.0 | 17,156.4 | -26.25% | 87.45% |
| PUBSUB | 64 | 741,620.0 | 469,089.0 | 423,486.4 | -9.72% | 57.10% |
| PUBSUB | 256 | 645,984.0 | 443,355.8 | 461,971.2 | +4.20% | 71.51% |
| PUBSUB | 1024 | 731,889.6 | 498,161.2 | 380,687.6 | -23.58% | 52.01% |
| PUBSUB | 4096 | 558,257.8 | 430,828.4 | 349,168.6 | -18.95% | 62.55% |
| PUBSUB | 65536 | 64,123.2 | 51,232.6 | 47,704.4 | -6.89% | 74.39% |

| 패턴 | B | 기존 C mean ms | pass1 mean ms | pass2 mean ms | pass2/C latency |
|---|---:|---:|---:|---:|---:|
| DEALER_DEALER | 64 | 0.282498 | 0.663 | 0.623 | 2.21x |
| DEALER_DEALER | 256 | 1.871429 | 0.478 | 1139.874 | 609.09x |
| DEALER_DEALER | 1024 | 1.162395 | 498.454 | 778.483 | 669.72x |
| DEALER_DEALER | 4096 | 641.529613 | 296.083 | 339.727 | 0.53x |
| DEALER_DEALER | 65536 | 16.944583 | 10.061 | 7.360 | 0.43x |
| DEALER_ROUTER_REQREP | 64 | 1.025241 | 0.456 | 0.480 | 0.47x |
| DEALER_ROUTER_REQREP | 256 | 0.952214 | 0.463 | 0.614 | 0.64x |
| DEALER_ROUTER_REQREP | 1024 | 0.952694 | 0.453 | 0.472 | 0.50x |
| DEALER_ROUTER_REQREP | 4096 | 1.216478 | 0.483 | 0.494 | 0.41x |
| DEALER_ROUTER_REQREP | 65536 | 2.285665 | 1.550 | 1.698 | 0.74x |
| ROUTER_ROUTER_REQREP | 64 | 0.728936 | 0.638 | 0.490 | 0.67x |
| ROUTER_ROUTER_REQREP | 256 | 0.901242 | 0.627 | 0.488 | 0.54x |
| ROUTER_ROUTER_REQREP | 1024 | 0.850048 | 0.490 | 0.480 | 0.56x |
| ROUTER_ROUTER_REQREP | 4096 | 1.145677 | 0.533 | 0.617 | 0.54x |
| ROUTER_ROUTER_REQREP | 65536 | 2.765579 | 1.713 | 2.126 | 0.77x |
| PUBSUB | 64 | 1477.281274 | 1542.151 | 1537.123 | 1.04x |
| PUBSUB | 256 | 1851.302441 | 1804.502 | 1634.467 | 0.88x |
| PUBSUB | 1024 | 1254.081383 | 1480.227 | 1825.245 | 1.46x |
| PUBSUB | 4096 | 414.599545 | 618.570 | 733.823 | 1.77x |
| PUBSUB | 65536 | 190.925062 | 301.896 | 250.976 | 1.31x |

## Report와 환경

- 공식 after 원본: `/home/hep7hep7/project/zlink-wt-dotnet-perf2/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260905_115300.txt`
- 요청한 사본: `/home/hep7hep7/project/zlink-work/c016/reports/perf_dotnet_multi_linux_20260905_115300.txt`
- pass1 원본 report는 기존 worktree 경로에 없어 보존된 실행 로그 `/home/hep7hep7/project/zlink-work/c016/dotnet-profile/official-after.log`의 RESULT 100개를 사용했다. pass1 summary의 throughput/latency 값과 일치한다.
- C 원본:
  - `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_110735_p1dotnet.txt`
  - `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_110836_p1dotnet.txt`
  - `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_110955_p1dotnet.txt`
  - `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_111113_p1dotnet.txt`
- after 구간 2026-09-05 **11:53:00~11:55:52 KST**(172초), 시작 uptime load average **2.17 / 2.22 / 4.06**. 측정 시작 직전 다른 Java/.NET benchmark 프로세스가 없음을 확인했다. 자신의 build/test/mini와 공식 after를 겹치지 않았다.
- 실행: `export ZLINK_CORE_SOURCE=local; source bindings/tools/local_core_runtime.sh; zlink_export_local_core_runtime` 뒤 `bash bindings/dotnet/perf/multi/run_benchmarks.sh --pattern DEALER_DEALER,DEALER_ROUTER_REQREP,ROUTER_ROUTER_REQREP,PUBSUB --transports tcp --msg-sizes 64,256,1024,4096,65536 --clients 100 --duration 5 --runs 1`.
- 모든 직접 .NET build와 테스트 스크립트 build는 `-m:1`; JOBS/ZLINK_BUILD_JOBS/CMAKE_BUILD_PARALLEL_LEVEL은 3 이하. Core configure/build/clean은 실행하지 않았다.
- detached HEAD `1961c950eb32b8fd4f071cc81344a7dc2b23917a` 유지. git commit/push/reset/checkout/stash 없음. 기존 untracked `core/build`, `core/build-dev` symlink 유지. repository 수정은 위 .NET 파일뿐이며 spec/doc/다른 binding/Core/perf runner 변경 없음.
- after 종료 뒤 Core SHA-256은 시작 때와 동일했다. 확인된 다음 Java profile 프로세스(PID 2901205)의 시작은 11:56:01 KST로 이 after 종료 뒤였다.
- mini 소스·baseline source backup·빌드·gate·반복·after 로그: `/home/hep7hep7/project/zlink-work/c016/dotnet-pass2-profile/`.
- 진행 로그: `/home/hep7hep7/project/zlink-work/c016/dotnet-perf-pass2-progress.md`; 60초 heartbeat와 주요 판단·after 시작/종료를 append했다.

## Gate

- `bash bindings/dotnet/tests/run_tests.sh`: **217/217 PASS**, sample **7/7 PASS**, exit 0.
- 관련 contract `test_hot_path_ownership_contract`, `test_pull_completion_contract`, `test_request_writable_contract`, `test_completion_lifecycle_regressions`, `test_contract_b_regressions`: **38건 × 5회 전부 PASS**.
- 관련 ownership 최소 검증: **17/17 PASS**. 새 테스트 7건은 기존 assertion/fixture/timeout을 완화하지 않고 별도로 추가했다.
- `git diff --check`: PASS. public contract 선언 diff 없음. 변경된 production 타입은 internal CompletionOwner와 그 private nested struct이며 공개 API signature 변경 없음.
- 기능 gate의 남은 실패 없음.

## 소유 계층·spec·교차언어·분류

- 소유 계층: .NET binding의 동기 native reply 인자 수명 및 runtime pump callback 할당. Core의 admission/token/timeout/error 결정을 변경하지 않았다.
- Spec: `core/doc/spec/core/socket/README.ko.md:921` Part send 입력 소비/전체 snapshot, `:1074` synchronous reply admission과 token, `:1105` Completion pull과 ownership; `bindings/doc/spec/dotnet/README.ko.md` 표준 인터페이스 규칙. submit/drain·close synchronization과 Task 관찰 의미는 동일하다.
- 교차언어: C++ pass2 resume-slot bundling은 별도 allocation 제거라는 방향만 대응한다. .NET에서는 기존 TaskCompletionSource 구조를 합치기보다 불필요한 closure를 지연했다. C++ inline map node는 .NET Dictionary의 allocation-free 정상 상태와 다르며, reply vector 임시 wrapper 제거도 .NET에 이미 직접 배열 대입이 있어 효과가 작다. 구조 차이로 .NET 후보를 그대로 복제하지 않았다.
- 변경 분류: **B — 기존 hot-path 내부 할당 비용 개선**. **spec gap 없음**, 공개 API·ownership·error contract 변경 없음.

## BLOCKERS

- 작업/기능 gate blocker 없음. 성능 인과 판정에는 **shared Core artifact와 historical baseline 불일치**가 남는다. 동일 binary의 새로운 C/바인딩 before-after를 이 pass에서 추가 실행하지 않았다(요청한 after 1회 유지).
- historical C 대비 관측 평균은 DD **67.94%**, DR REQREP **55.28%**, RR REQREP **52.26%**, PUBSUB **63.51%**로 모두 목표 미달이다. DD 256/1024B mean latency는 기존 C의 **609.09x/669.72x**다. 이 격차를 해결했다는 주장을 하지 않는다.
- 5% 이상은 allocation 감소와 pump 조기 반환 mini 경로에서 확인했다. 실제 TCP 처리량에 대한 각 후보의 독립 기여도는 확정하지 않았다.
- 작은 메시지 격차, runtime Task/entry와 수신 collection 비용은 남는다. 아래 하락 셀은 artifact/부하/run 분산을 분리하지 않은 관측값으로 그대로 보존한다.
- DEALER_DEALER 하락: 64B -18.88%, 4096B -5.60%, 65536B -10.61%.
- DEALER_ROUTER_REQREP 하락: 64B -2.70%, 256B -10.90%, 1024B -6.46%, 4096B -7.33%, 65536B -1.54%.
- ROUTER_ROUTER_REQREP 하락: 4096B -7.85%, 65536B -26.25%.
- PUBSUB 하락: 64B -9.72%, 1024B -23.58%, 4096B -18.95%, 65536B -6.89%.
