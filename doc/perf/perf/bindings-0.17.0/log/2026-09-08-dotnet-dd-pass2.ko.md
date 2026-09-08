# .NET Multi DEALER_DEALER pass 2 — P/Invoke 융합과 operation 재사용

고정 Core prefix `0.17.3-alpha`, `tcp`, clients 100, duration 5 s, 2-part 기본값,
64·4096 B, 1-run에서 C와 .NET을 같은 티켓 안에서 연속 측정했다.

**채택한 라이브러리 변경은 0건이다.** P/Invoke 융합은 DD 64 B 처리량이
922,956.0에서 840,330.2 msg/s로 **8.95% 감소**했고 C 대비 비율도
55.06%에서 49.77%로 **5.29%p 감소**해 원복했다. operation 객체 재사용은
제출 뒤에도 사용자가 public interface reference를 보유할 수 있어, pool에 반환하면 오래된
reference가 다음 operation을 조작하게 된다. one-shot 의미를 지킬 수 없으므로 구현하지 않았다.

최종 상태는 허용된 이 기록 파일만 새 파일이고 .NET binding source diff는 0이다.

## 1. before와 비용 지도

### 고정 Core paired before

| 크기 | C 처리량 | C ns/msg | .NET 처리량 | .NET ns/msg | 잔여 격차 | .NET/C |
|---:|---:|---:|---:|---:|---:|---:|
| 64 B | 1,676,261.6 | 596.566 | 922,956.0 | 1,083.475 | 486.910 ns | 55.06% |
| 4096 B | 648,297.2 | 1,542.502 | 511,149.6 | 1,956.374 | 413.872 ns | 78.84% |

64 B .NET 절대 처리량은 D-BP31 지도 값 922,799.8 msg/s와 0.02% 차이다.
아래 함수별 ns는 새 wall 측정에 D-BP31의 `perf`·EventPipe 함수 귀속을 대응시킨 값이다.
고정 Core alpha에서 함수별 profile을 새로 만들었다는 뜻은 아니다. `% 격차`만 이번 paired
before의 486.910 ns를 분모로 다시 계산했다. 함수별 CPU를 wall critical path로 단정하지
않으며, 원 지도의 `send builder·runner helper`는 인라인된 builder·동기 terminal과 runner
helper를 완전히 분리하지 못한다.

| .NET에만 있는 항목 | C 대응 | .NET ns/msg | 현재 격차 비중 | managed 할당 |
|---|---:|---:|---:|---:|
| send builder·runner helper | 0 | **177.671** | **36.49%** | `SocketSendOperation` 80.5 B/msg 추정 |
| message helper P/Invoke 경계 | 0 | **137.537** | **28.25%** | 0 |
| `Message` wrapper 생성·소멸 | 0 | 76.145 | 15.64% | wrapper pool 사용, 정상 64 B 추가 할당 0 |
| 2-part staging·collection | 0 | 17.138 | 3.52% | `TwoMessageReadOnlyList` 31.3 B/msg 추정 |
| native send P/Invoke 경계·인자 준비 | 0 | 8.244 | 1.69% | 0 |
| GC pause | 0 | 3.293 | 0.68% | Gen0/1/2 = 1/0/0 |
| async terminal·재개 | 0 | 0.651 | 0.13% | 정상 즉시 성공 경로 Task 추가 할당 0 |
| 위 항목 합 | 0 | 420.679 | 86.40% | — |

native send 본체는 D-BP31 profile에서 .NET 440.163 ns, C 467.541 ns로 같은 범위였다.
따라서 차이를 Core send 본체로 귀속하지 않았다.

### 요청당 횟수와 할당

| 항목, DD 64 B 정상 즉시 성공 | C | .NET before | 융합 실험 | 판정 |
|---|---:|---:|---:|---|
| managed 할당 byte | 0 | 112.010 B/msg | 동일 | managed heap 기준; native malloc 미측정 |
| managed 할당 객체 | 0 | 2개/msg | 동일 | operation 1개 + 2-part view 1개 |
| managed→native 호출 | 0 | 13회/msg | 9회/msg | C의 C API 호출은 managed 경계가 아님 |
| 일반 GC transition | 0 | 10회/msg | 9회/msg | `init`·`data`는 before에서 transition 억제 |
| binding이 추가한 thread handoff | 0 | 0회/msg | 0회/msg | 즉시 성공은 `Task.CompletedTask`; completion pump 없음 |

Core I/O thread로 넘기는 공통 내부 scheduling은 두 언어 모두에 있고 이번 지도에서 요청당
정확한 횟수를 측정하지 않았다. 표의 handoff는 binding/runtime이 C보다 추가하는 handoff다.
managed→native 호출 수는 D-BP31의 profiler stack과 Release 소스 경로를 함께 센 값이다.
Linux `perf stat`에는 P/Invoke 호출 수를 직접 세는 event가 없으므로 이를 context-switch 수로
대체하지 않았다.

## 2. 지배 항목

64 B 잔여 격차의 20%를 넘는 항목은 둘이다.

1. send builder·runner helper: 177.671 ns/msg, 현재 격차의 36.49%.
   정상 호출마다 실제 80 B `SocketSendOperation` 하나가 생성되고 public interface로 반환된다.
2. message helper P/Invoke 경계: 137.537 ns/msg, 현재 격차의 28.25%.
   D-BP31의 CLR profile에서 `init_size` 82.436 ns, `close` 49.895 ns,
   `copy` 5.206 ns로 귀속됐다.

둘의 합은 315.208 ns/msg, 현재 격차의 64.74%다. GC pause는 0.68%라 후보가 아니다.

## 3. 후보 1 — P/Invoke 융합

### 실험 diff

공개 API와 Core는 바꾸지 않고 실험 중에만 다음을 적용했다.

- `bindings/dotnet/src/Zlink/Runtime/Native/zlink_dotnet_glue.c`: binding 전용 Linux
  shared library에 `zlink_msg_init_size`+payload `memcpy`, 2-part의
  `zlink_msg_init`+`zlink_msg_copy` 두 묶음을 제공했다. 실패 errno를 cleanup 전에 보존하고
  초기화한 prefix만 닫았다.
- `Message.Native.cs`: `Message.From`의 연속된 `init_size`+payload copy를 helper 1회로
  바꿨다.
- `RequestReplySupport.cs`: 정상 2-part scratch 생성의 `init` 2회+`copy` 2회를 helper
  1회로 바꿨다. FINAL 전에 전체 part를 복사하고 실패 시 원본을 보존하는 순서는 유지했다.
- `NativeLibraryLoader.cs`, `NativeMethods*.cs`, `Zlink.csproj`: glue import·로딩·Linux
  package 출력을 배선했다.

공개 `Message` 생성과 이후 public send builder 제출은 서로 다른 관찰 가능한 수명 경계라
그 둘을 한 native 호출로 합치지 않았다. 실험은 각 경계 안의 연속 작업만 융합했다.

정상 DD 2-part의 managed→native 호출은 **13→9회**, 일반 GC transition은 **10→9회**다.
4회 감소는 payload `init_size+data` 2→1과 scratch `init+copy` 4→1의 합이다.
`close` 4회와 send 2회는 바꾸지 않았다. 할당과 thread handoff는 변하지 않았다.

### paired after

| 크기 | before C | before .NET | before .NET/C | after C | after .NET | after .NET/C | .NET 절대 변화 | 비율 변화 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 B | 1,676,261.6 | 922,956.0 | 55.06% | 1,688,331.4 | 840,330.2 | 49.77% | **−8.95%** | **−5.29%p** |
| 4096 B | 648,297.2 | 511,149.6 | 78.84% | 607,153.2 | 492,429.4 | 81.10% | −3.66% | +2.26%p |

mean latency는 .NET 64 B 0.480241→1.274041 ms(+165.29%), 4096 B
437.678701→430.524902 ms(−1.63%)였다. 4096 B의 C 대비 비율 상승은 같은 pair의 C
처리량도 6.35% 낮아진 결과가 섞이므로 후보 효과로 채택하지 않는다.

64 B가 채택 조건 `+5%p`와 반대 방향이고 대상 cell 자체가 −5%를 넘게 회귀했다.
따라서 후보를 전부 원복했고 PUBSUB·DR REQREP 64 B 확장 측정은 조건 미성립으로 실행하지
않았다. 원복 뒤 Release multi binary를 다시 빌드했고 candidate glue build artifact도 제거했다.

## 4. 후보 2 — operation 상태 객체 내부 재사용

**계약으로 기각했고 diff와 after 측정은 없다.**

- `DealerSocket.Send():16-19`는 새 `SocketSendOperation`을 public `SendOperation`
  interface로 반환한다.
- `SocketSendOperation:19-56`의 `Message`와 세 terminal은 동일 reference의
  `_parts`와 `_submission`을 사용한다.
- `OperationSubmissionGuard:9-20`은 제출 뒤 같은 reference의 모든 재사용을
  `InvalidState`로 거부해 one-shot 의미를 유지한다.
- public 계약 `OperationContracts.cs:22-41`은 builder reference가 `Message` 뒤에도
  `SendSubmitOperation`으로 계속 노출됨을 정한다. 사용자가 terminal 뒤 reference를
  해제했다는 신호나 `Dispose` 계약은 없다.

terminal 직후 객체를 socket/thread pool에 반환하고 `_submission`·`_parts`를 초기화하면,
사용자가 보유한 오래된 interface reference와 다음 호출자가 받은 operation이 같은 객체가 된다.
오래된 reference가 다음 operation에 part를 추가하거나 제출할 수 있어 one-shot 의미와 객체별
누적 상태가 깨진다. generation lease/proxy를 새로 할당하면 identity는 분리할 수 있지만 제거
대상인 메시지당 operation 할당을 다른 객체 할당으로 옮길 뿐이다. 따라서 공개 시그니처를
유지해도 공개 의미는 유지할 수 없다.

## 5. 검증·원본·최종 상태

- candidate 집중 ownership test: `test_hot_path_ownership_contract` **22/22 통과**.
- 후보 원복 뒤 `bash bindings/dotnet/tests/run_tests.sh`: unit·contract **232/232**,
  sample **7/7**, 실패 0.
- 후보 원복 뒤 .NET multi Release build: warning 0, error 0. perf output과 binding
  `Systems.Zlink.dll` SHA-256가 모두
  `78942c26b7e89d3c3a65e4e0b21edaab77725fe9e2988b9592930e541202e8c7`이다.
- before와 after 사이 HEAD 변화는 Node·Go·문서뿐이며 C DD runner와 .NET binding/perf
  multi source 변화는 후보 diff 외 0이었다. Core binary는 같은 고정 prefix를 사용했다.
- commit·push 0회. Core·Framework·다른 언어·정책·스펙·계획서 수정 0회.

원본 report:

- before C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_134012_dotnet-dd-pass2-before-c.txt`
- before .NET: `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260908_134023_dotnet-dd-pass2-before-net.txt`
- fusion after C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_135000_dotnet-dd-pass2-fusion-after-c.txt`
- fusion after .NET: `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260908_135011_dotnet-dd-pass2-fusion-after-net.txt`
- before ticket: `.artifacts/perf-queue/log/2-1788842341-60344-codex-dotnet-dd-pass2_paired_before_C_then_dot.log`, rc=0
- after ticket: `.artifacts/perf-queue/log/2-1788842923-22057-codex-dotnet-dd-pass2_pinvoke_fusion_paired_af.log`, rc=0

## 6. 남은 격차의 성격

| 범주 | 남은 항목 |
|---|---|
| 계약 | public operation 객체 80 B/msg. terminal 뒤 사용자가 reference를 해제했음을 알 수 없어 내부 재사용 불가 |
| runtime/FFI | message helper P/Invoke 137.537 ns/msg. 계약 안전한 연속 구간 융합은 호출 13→9에도 64 B 성능이 하락해 기각 |
| runtime | `Message` wrapper 생성·소멸 76.145 ns/msg와 CLR/JIT 잔여 |
| 미확인 | alpha Core에서 함수별 wall critical path, Core 공통 I/O scheduling의 요청당 handoff 수, native allocation bytes |

수정 전 규칙 수: public builder one-shot 1개, Message ownership 1개, part별 native
초기화·복사 1개. 수정 후 규칙 수: **동일**. 두 후보 모두 최종 코드에 남지 않았다.
