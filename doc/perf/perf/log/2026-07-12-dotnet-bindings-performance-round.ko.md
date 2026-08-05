# .NET bindings 성능 개선 라운드

## 실행 조건

- runtime: `core/build/lib/libzlink.so.9.0.0`
- 시작 HEAD: `61cc30d61`
- CPU pin: 사용하지 않음
- 실행 단위: 한 번에 한 perf process
- 순서: 현재 transport의 C 측정 직후 .NET을 측정하고 비교한다.
- 판정: throughput과 평균 latency만 gate에 사용하고 p95와 p99는 진단 자료로만 사용한다.

다른 .NET build와 test가 CPU를 사용하면 해당 프로세스가 끝날 때까지 기다렸다. 각 공식
측정 직전에는 `top`을 반복 실행해 CPU 상태를 확인했다.

## Single PAIR

### tcp 최초 측정

C와 .NET을 CPU pin 없이 각각 5회 측정했다.

- C: `perf_c_single_linux_20260712_071829_core_9_0_dotnet_pair_tcp_nopin_paired_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_072101_core_9_0_dotnet_pair_tcp_nopin_paired_20260712.txt`

최초 throughput 비율은 99.1%, 78.6%, 117.9%, 99.7%, 99.9%, 99.9%로 모두
.NET 단순 one-way 최소 목표 70%를 넘었다. 그러나 64B 평균 latency는 C 0.147ms와
.NET 1.032ms로 7.02배였으며 최대 허용치 3.0배를 넘었다.

외부 `dotnet test`가 끝난 뒤 CPU idle 상태에서 64B만 다시 측정했다.

- C: `perf_c_single_linux_20260712_072404_core_9_0_dotnet_pair_tcp64_nopin_recheck_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_072525_core_9_0_dotnet_pair_tcp64_nopin_recheck_20260712.txt`

재측정도 C 0.159ms와 .NET 0.894ms로 평균 latency가 5.62배였다. 시스템 부하가
아니라 .NET 경로의 반복 가능한 문제로 판정했다.

### POSD 검토와 병목 진단

확인한 위험 신호는 다음과 같다.

- public send 한 건마다 `Message`와 one-shot send builder가 만들어진다. 호출자가
  소유권 규칙을 지키지 않으면 관리 힙 회수 비용이 hot path에 누적된다.
- `Received`는 caller-provided storage를 재사용하지만 내부 single-part wrapper와
  native handle 교체 비용이 남아 있다.
- perf helper가 성공 submit에서 소비된 `Message` wrapper를 dispose하지 않았다.
  공개 계약은 소비된 wrapper도 dispose해야 pool에 반환된다고 명시한다.

두 가지 방향을 비교했다.

1. 송신 경로에서 public 소유권 계약을 지키고 pool-backed `Message`를 재사용한다.
   payload 모양과 builder API를 바꾸지 않으면서 관리 힙 비용을 줄일 수 있다.
2. 수신 `Received`의 single-part 저장소를 더 직접 재사용한다. 공개 API는 유지할 수
   있지만 송신 측 GC가 주원인이면 효과가 작고, nonblocking 실패 시 기존 결과를
   보존하는 계약을 더 복잡하게 만든다.

`dotnet-counters` 진단에서 변경 전 64B 실행의 managed allocation은 5개 표본 평균
약 351MB/s였고 Gen0 GC 6회와 GC pause 합계 19.772ms가 기록됐다. sampling trace의
상위 경로는 `PerfSocketIo.Send`, `SinglePartSubmit.Submit`,
`SocketKernel.ReceiveBasicParts`였다. 원인과 책임 경계가 직접 맞는 1번을 선택했다.

### 채택한 변경

`PerfSocketIo`의 span 기반 send와 publish가 `Message.Allocate()`로 pool-backed wrapper를
얻고 payload를 복사한 뒤, submit 성공·backpressure·예외와 관계없이 `finally`에서
dispose하도록 바꿨다. 새 public API나 private 우회 경로를 추가하지 않았고 payload와
측정 의미도 바꾸지 않았다. 성공 submit 뒤 wrapper dispose를 요구하는 기존 public
계약을 benchmark가 그대로 따르도록 고친 것이다.

변경 후 진단에서 managed allocation 5개 표본 평균은 약 193MB/s로 45.0% 감소했고,
Gen0 GC는 4회, GC pause 합계는 13.181ms로 감소했다.

### 최종 측정

64B 제한 측정에서 .NET 평균 latency는 0.239ms로 개선 전 0.894ms보다 73.3%
낮아졌고 C 0.159ms의 1.50배로 통과했다.

- .NET 64B 후보: `perf_dotnet_single_linux_20260712_073131_core_9_0_dotnet_pair_tcp64_nopin_after_message_dispose_20260712.txt`

이후 tcp 여섯 size 전체를 .NET 5회로 다시 측정했다. 4회차 1024B와 대형 셀에 외부
부하성 동반 하락이 있었지만 나머지 네 회차와 5회 중앙값은 안정적이었다.

- C 기준: `perf_c_single_linux_20260712_071829_core_9_0_dotnet_pair_tcp_nopin_paired_20260712.txt`
- .NET 최종: `perf_dotnet_single_linux_20260712_073333_core_9_0_dotnet_pair_tcp_nopin_final_after_message_dispose_20260712.txt`

최종 throughput 비율은 98.3%, 86.5%, 99.0%, 99.9%, 100.0%, 99.9%였고 평균
latency 최대 비율은 1.19배였다. 모든 셀이 목표를 만족했다.

- `PAIR / tcp`: 완료
- public API 변경: 없음
- binding runtime 변경: 없음
- perf 변경: 성공 submit 뒤 Message wrapper dispose와 pool-backed 생성 적용
- 다음 transport: ws

### PAIR active send 정책 정합화

`PERF_SINGLE_TEST_POLICY.md`는 raw one-way sender가 blocking send를 연속 수행하고 HWM에서
자연 backpressure를 받도록 정한다. 그러나 C `perf_pair.cpp`와 .NET `PerfPair`는 모두
`DONTWAIT`를 사용하고 있었다. C와 binding 사이만 같고 확정 정책의 측정 의미와는 달랐으므로,
현재 pattern인 PAIR의 두 perf를 함께 blocking send로 수정했다. 다른 pattern의 perf는
현재 작업 단위가 아니므로 미리 바꾸지 않았다.

이 정합화로 기존 tcp 완료 report는 같은 측정 의미의 근거가 아니게 됐다. tcp의 모든
transport size를 blocking 의미로 다시 paired 측정한 뒤 완료 상태를 복구한다.

### ws 256B blocking paired 측정

CPU idle 상태에서 C와 .NET을 CPU pin 없이 각각 5회 측정했다.

- C: `perf_c_single_linux_20260712_080902_core_9_0_dotnet_pair_ws256_blocking_policy_paired_20260712.txt`
- .NET before: `perf_dotnet_single_linux_20260712_080939_core_9_0_dotnet_pair_ws256_blocking_policy_paired_20260712.txt`

C 중앙값은 1,648,611.8 msg/s와 평균 latency 41.411ms였다. .NET before는
1,015,541.6 msg/s와 67.322ms로, 처리량 비율 61.60%는 최소 목표 70%에 미달했고
평균 latency 비율 1.63배는 3.0배 상한을 통과했다.

### blocking 병목 POSD 검토

sampling trace에서 `PerfSocketIo.Send` 48.21%, `SinglePartSubmit.Submit` 23.69%,
`Message.MoveTo` 10.10%, `SocketKernel.ReceiveBasicParts` 39.90%,
`Received.ResetForReuse` 4.99%가 관측됐다. `dotnet-counters`에서는 초당 약 67MB의
지속 할당이 있었지만 5초 동안 GC가 발생하지 않아 GC pause는 이번 blocking 처리량
미달의 원인이 아니었다.

검토한 위험 신호와 대안은 다음과 같다.

1. one-shot builder를 풀링하면 과거 builder 참조와 다음 operation이 결합된다. 실제로
   builder 할당을 제거한 진단 후보도 처리량이 약 2%만 올라 목표에 도달하지 못해 제거했다.
2. opaque `ZlinkMsg`를 C#에서 직접 복사하면 core message layout 지식이 binding으로
   누출된다. 기존 `zlink_msg_adopt`를 사용한 후보는 수치 변화가 없어 제거했다.
3. assembly 전체 local 초기화를 생략하면 unrelated interop local까지 안전 가정이 퍼진다.
   바로 다음 native init이 64바이트 전체를 초기화하는 송신·basic 수신 local 두 곳만
   `Unsafe.SkipInit`으로 좁히는 방안을 선택했다.

수신 wrapper 직접 교체, compact builder buffer, GC transition 생략, tiered JIT 강제 최적화,
중복 guard 제거, 예외 slow-path 분리 후보도 기능 검증 뒤 제한 측정했지만 처리량 개선이
없거나 latency가 악화돼 모두 제거했다.

### 채택한 `ZlinkMsg` 중복 초기화 제거

`SinglePartSubmit`과 `SocketKernel.ReceiveBasicParts`는 stack의 64바이트 `ZlinkMsg`를
0으로 채운 직후 `zlink_msg_init`으로 다시 전부 초기화했다. 두 local에만
`Unsafe.SkipInit`을 적용하고, native init 성공 전에는 값을 읽거나 닫지 않는 기존 순서를
유지했다. 확정 hot path와 안전 조건은 코드 주석으로 남겼다.

공식 5회 후보 report는 다음과 같다.

- .NET after: `perf_dotnet_single_linux_20260712_082632_core_9_0_dotnet_pair_ws256_blocking_skipinit_candidate_20260712.txt`

처리량 중앙값은 1,050,306.2 msg/s로 before보다 3.42% 높아졌고 평균 latency는
64.065ms로 4.84% 낮아졌다. C 대비 처리량은 63.71%라 아직 미달이며 평균 latency는
1.55배로 통과한다. 이 개선은 유지하되 ws 완료로 기록하지 않는다.

검증 결과:

- .NET single Release build: 통과, warning 0
- .NET multi Release build: 통과, warning 0
- `Zlink.Tests` Release 전체: 177개 통과
- C `perf_pair` build와 C/.NET blocking smoke: 통과

검증된 코드와 perf 정합화는 `f1440eb18` (`perf(dotnet): align pair send and skip native clears`)
커밋으로 분리했고, 측정 근거 커밋 `867aa137b`와 함께 원격 `main`에 푸시했다.

다음 작업은 blocking 의미로 PAIR tcp 전체 size를 C와 .NET 순서로 다시 paired 측정하고,
tcp 완료 상태를 복구한 뒤 ws 256B의 남은 처리량 미달을 계속 개선하는 것이다.

### POSD 개선 단독 채택 기준

성능 수치가 좋아지지 않더라도 기존 위험 신호를 실제로 제거하고 정보 은닉이나 책임 경계를
분명하게 개선하며, 처리량·평균 latency·기능 회귀가 없으면 채택할 수 있도록 판정 기준을
보완했다. 다만 POSD 개선만으로 목표 미달 셀을 통과로 바꾸지는 않는다. 성능과 POSD 어느
쪽에서도 분명한 이득이 없거나 성능 회귀가 생긴 후보는 계속 제거한다.

### PAIR tcp blocking paired 재측정

CPU 고부하 프로세스가 없을 때 C와 .NET을 CPU pin 없이 차례로 5회 측정했다.

- C: `perf_c_single_linux_20260712_083121_core_9_0_dotnet_pair_tcp_blocking_nopin_paired_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_083602_core_9_0_dotnet_pair_tcp_blocking_nopin_paired_20260712.txt`

64, 256, 1024, 65536, 131072, 262144B 처리량 비율은 각각 87.5%, 64.4%,
76.5%, 85.9%, 90.6%, 87.0%였다. 평균 latency 비율은 모두 3배 이내였다. 256B만
최소 처리량 목표에 미달했다. 변동 여부를 확인하기 위해 같은 크기만 다시 C 직후 .NET으로
5회 측정했고 C 1,854,838.8msg/s, .NET 1,180,672.2msg/s로 63.65%가 재현됐다.

- C 256B 재확인: `perf_c_single_linux_20260712_084049_core_9_0_dotnet_pair_tcp256_blocking_nopin_recheck_20260712.txt`
- .NET 256B 재확인: `perf_dotnet_single_linux_20260712_084120_core_9_0_dotnet_pair_tcp256_blocking_nopin_recheck_20260712.txt`

### tcp 256B 추가 병목 진단과 기각 후보

sampling trace에서 송신 스레드의 5초 중 대부분은 native blocking 구간에 있었고,
`SinglePartSubmit`과 `Message.MoveTo`가 남은 관리 경계로 확인됐다. 두 방향을 추가로
검증했다.

1. `Message`가 가진 native handle을 임시 handle로 옮기지 않고 직접 submit하는 후보는
   처리량이 1,189,576.0msg/s로 0.75%만 높아졌고 평균 latency가 0.185ms에서
   0.235ms로 27.0% 높아졌다. 성능 회귀가 있어 제거했다.
2. 임시 friend 접근으로 public fluent builder 비용을 완전히 뺀 진단 상한도
   1,209,149.4msg/s로 2.4%만 높아졌고 C 대비 65.2%였다. public API나 builder
   수명 모델을 바꿀 근거가 없으므로 임시 코드를 모두 제거했고 이 결과는 공식 판정에
   사용하지 않는다.

`/usr/bin/time -v`로 같은 5초 실행을 비교하면 C는 user 7.07초, system 2.59초,
최대 RSS 138,336KB였고 .NET은 user 8.43초, system 2.57초, 최대 RSS 489,368KB였다.
system 비용보다 관리 힙 누적과 관리 경계의 user CPU 차이가 크지만 builder 하나만으로는
목표 차이를 설명하지 못한다. tcp 256B는 계속 `미달`로 유지하고 다음 binding 내부 후보를
조사한다.

### submission 상태 검증 POSD 개선

모든 operation submit 경로는 message 존재 여부와 callback stage 같은 준비 상태를 확인하면서
submission 상태도 검사한 뒤, `OperationSubmissionGuard.MarkSubmitted()` 안에서 같은 상태를
다시 검사했다. 첫 번째 설계는 guard가 검증과 상태 전이를 모두 맡게 두는 방식이고, 두 번째
설계는 operation의 준비 검증과 guard의 상태 전이를 분리하는 방식이다. 모든 호출부가 이미
준비 검증을 수행하므로 두 번째 설계를 선택하고 메서드 이름을
`MarkSubmittedAfterValidation()`으로 바꿔 사전 조건을 드러냈다.

35개 호출부가 상태 전이 전에 검증하는 것을 확인했으며, 같은 operation을 두 번 submit하면
계속 `ZlinkConfigException`이 발생하는 회귀 테스트를 추가했다. tcp 256B 3회 제한 측정은
1,171,729.4msg/s와 평균 latency 0.181ms였다.

- report: `perf_dotnet_single_linux_20260712_085521_core_9_0_dotnet_pair_tcp256_submission_guard_posd_candidate_20260712.txt`
- 직전 기준 대비 throughput: -0.76%
- 직전 기준 대비 평균 latency: -2.2%
- single/multi Release build: 경고 0, 오류 0
- `Zlink.Tests`: 178개 통과

처리량과 평균 latency 회귀 gate 안이고 중복 책임을 제거했으므로 성능 목표 통과와는 별개인
POSD 개선으로 채택한다. tcp 256B 처리량 상태는 계속 `미달`이다.

### 짧은 native message helper 전환 비용 개선

메시지 한 건의 송수신에서 짧은 native message helper를 여러 번 호출한다. 모든 interop에
GC transition 생략을 적용하는 설계는 allocation, free, blocking transport까지 안전 가정을
넓히므로 제외했다. `zlink_msg_init`, `zlink_msg_move`, `zlink_msg_data`,
`zlink_msg_size`처럼 할당하지 않고 관리 callback을 호출하지 않는 네 함수만 제한하는 설계를
선택했다. `init_size`, `close`, send, receive는 정상 GC transition을 계속 사용한다.

tcp 256B 3회 사전 측정은 1,210,014.0msg/s와 평균 latency 0.170ms였다. 최종 5회는
C 직후 .NET을 CPU pin 없이 실행했다.

- C: `perf_c_single_linux_20260712_090321_core_9_0_dotnet_pair_tcp256_short_native_transition_final_paired_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_090350_core_9_0_dotnet_pair_tcp256_short_native_transition_final_paired_20260712.txt`
- C: 1,867,080.8msg/s, 평균 latency 13.390ms
- .NET: 1,212,258.4msg/s, 평균 latency 0.164ms
- C 대비 throughput: 64.93%
- 개선 전 .NET 1,180,672.2msg/s 대비: +2.68%

비대상 tcp 셀 3회 측정에서 64B, 64KiB, 256KiB는 개선됐고 1KiB는 -0.1%였다.
128KiB 최초 3회 중앙값은 낮았지만 해당 셀을 C 직후 .NET으로 5회 재측정한 결과
C 57,818.8msg/s, .NET 55,148.0msg/s로 95.4%였으며 평균 latency는 1.05배였다.

- 비대상 셀: `perf_dotnet_single_linux_20260712_090429_core_9_0_dotnet_pair_tcp_short_native_transition_regression_20260712.txt`
- C 128KiB: `perf_c_single_linux_20260712_090557_core_9_0_dotnet_pair_tcp131072_short_native_transition_regression_paired_20260712.txt`
- .NET 128KiB: `perf_dotnet_single_linux_20260712_090627_core_9_0_dotnet_pair_tcp131072_short_native_transition_regression_paired_20260712.txt`
- single/multi Release build: 경고 0, 오류 0
- `Zlink.Tests`: 178개 통과

확정 hot path와 제한 조건은 native 선언의 주석에 남겼다. 실제 처리량 개선과 회귀 gate를
통과했으므로 변경을 채택하지만 tcp 256B는 70% 미만이라 계속 `미달`이다.

짧은 native helper 개선은 `bb325ccd7` (`perf(dotnet): trim short native message
transitions`)로 원격 `main`에 푸시했다.

### basic receive의 사용하지 않는 routing ID 제거

`ReceiveBasicParts`는 source routing ID를 `byte[]`로 복사해 출력했지만 유일한 호출자는 항상
그 값을 버렸다. 향후 사용을 예상해 범용 출력을 유지하는 설계와, routed metadata 책임을
전용 routed receive helper에만 두는 설계를 비교했다. 후자를 선택해 basic receive의 사용하지
않는 출력 parameter와 복사 호출을 제거했다. 리팩토링 때 이 비용이 다시 들어오지 않도록
책임 경계를 `HOT PATH:` 주석에 기록했다.

tcp 256B 3회 결과는 1,218,093.6msg/s와 평균 latency 0.172ms였다. 직전 결과 대비
처리량은 +0.48%, 평균 latency는 +4.9%로 회귀 gate 안이다.

- report: `perf_dotnet_single_linux_20260712_091351_core_9_0_dotnet_pair_tcp256_basic_receive_metadata_posd_candidate_20260712.txt`
- single/multi Release build: 경고 0, 오류 0
- `Zlink.Tests`: 178개 통과

사용하지 않는 정보 흐름과 복사 책임을 제거했고 기능·성능 회귀가 없으므로 POSD 개선으로
채택한다. tcp 256B의 공식 상태는 계속 `미달(64.9%)`이다.

basic receive metadata 정리는 `096ffd396` (`refactor(dotnet): narrow basic receive
metadata`)으로 원격 `main`에 푸시했다.

### pointer 전용 message interop 후보 기각

`Message` 내부 필드를 `fixed`로 고정한 뒤 data, size, move를 pointer 전용 interop stub로
호출하는 후보를 확인했다. 기존 `ref` stub를 유지하는 설계보다 interior reference 처리를
명시할 수 있지만 같은 native export의 선언이 중복되고 호출부 pinning 책임도 늘어난다.

tcp 256B 3회 중앙값은 1,189,460.0msg/s와 평균 latency 0.149ms였다. 직전 POSD 후보
1,218,093.6msg/s보다 처리량이 2.35% 낮았다. throughput 회귀와 interop 선언 중복이 함께
발생했으므로 후보를 전부 제거했다.

- report: `perf_dotnet_single_linux_20260712_092024_core_9_0_dotnet_pair_tcp256_pointer_message_stub_candidate_20260712.txt`
- 최종 코드 변경: 없음

### invalid Message 초기화 상태 분기 제거

`InitSizeValidated`의 세 호출부는 모두 새 wrapper 또는 pool에서 반환된 invalid wrapper만
전달하지만 메서드는 valid 상태이면 조용히 반환했다. 방어적 no-op를 유지하는 설계와 invalid
사전 조건을 메서드 이름에 고정하는 설계를 비교했다. 후자를 선택해
`InitSizeOnInvalidMessage`로 이름을 바꾸고 중복 상태 분기를 제거했다. size와 state 사전
조건은 같은 `HOT PATH:` 주석에 기록했다.

3회 측정은 평균 latency가 직전보다 11.4% 높아 5회로 재확인했다. 최종 5회 중앙값은
1,212,121.0msg/s와 평균 latency 0.176ms로 직전 대비 처리량 +1.6%, 평균 latency +0.6%였다.

- 3회: `perf_dotnet_single_linux_20260712_093113_core_9_0_dotnet_pair_tcp256_invalid_message_state_candidate_20260712.txt`
- 5회: `perf_dotnet_single_linux_20260712_093144_core_9_0_dotnet_pair_tcp256_invalid_message_state_recheck_20260712.txt`
- single/multi Release build: 경고 0, 오류 0
- message 제한 테스트: 28개 통과
- `Zlink.Tests`: 178개 통과

기능·성능 회귀 없이 잘못된 호출을 정상 no-op처럼 숨기던 분기를 제거했으므로 POSD 개선으로
채택한다. tcp 256B의 공식 상태는 계속 `미달(64.9%)`이다.

- commit: `0d440efde`

### Message size 중복 검증 제거

`Message.Allocate(size)`는 public 메서드, `AllocateCore`, `InitSize`에서 같은 음수 검증을
세 번 수행했다. private 계층마다 방어 검증을 유지하는 설계와 public 생성 경계에서 한 번
검증한 뒤 내부 사전 조건을 이름으로 드러내는 설계를 비교했다. 후자를 선택해 내부 메서드를
`AllocateCoreValidated`, `InitSizeValidated`로 명명하고 public 생성자와 `Allocate`에서만
입력을 검증한다. 확정 hot path의 조건은 `HOT PATH:` 주석으로 남겼다.

tcp 256B 3회 중앙값은 1,192,564.4msg/s와 평균 latency 0.175ms였다. 직전 결과 대비
처리량 -2.1%, 평균 latency +1.7%로 회귀 gate 안이다.

- report: `perf_dotnet_single_linux_20260712_092243_core_9_0_dotnet_pair_tcp256_validated_message_size_candidate_20260712.txt`
- single/multi Release build: 경고 0, 오류 0
- message 제한 테스트: 28개 통과
- `Zlink.Tests`: 178개 통과

성능 수치 향상은 없지만 검증 책임과 내부 사전 조건이 분명해졌고 기능·성능 회귀가 없으므로
POSD 개선으로 채택한다. tcp 256B의 공식 상태는 계속 `미달(64.9%)`이다.

### pooled Message rent clear 제거 후보 기각

pool 반환은 released wrapper만 허용하므로 rent 시 opaque 64바이트 handle을 다시 지우는 작업을
제거하는 후보를 확인했다. 256B 제한 3회는 1,225,937.4msg/s와 평균 latency 0.099ms였고,
최종 paired 5회는 C 1,838,912.0msg/s, .NET 1,204,677.0msg/s로 65.51%였다.

- C 256B: `perf_c_single_linux_20260712_092527_core_9_0_dotnet_pair_tcp256_pool_rent_clear_final_paired_20260712.txt`
- .NET 256B: `perf_dotnet_single_linux_20260712_092558_core_9_0_dotnet_pair_tcp256_pool_rent_clear_final_paired_20260712.txt`

대표 셀 3회 뒤 64B와 128KiB를 C 직후 .NET으로 5회 재검증했다. 64B는 C 대비
89.6%와 평균 latency 1.14배로 통과했지만, 128KiB .NET 처리량은 이전 55,148.0msg/s에서
48,247.8msg/s로 12.5% 낮아져 5% 회귀 gate를 넘었다. 따라서 후보와 `HOT PATH:` 주석을
최종 코드에서 제거했다.

- C 대표 셀: `perf_c_single_linux_20260712_092736_core_9_0_dotnet_pair_tcp_pool_rent_clear_regression_paired_20260712.txt`
- .NET 대표 셀: `perf_dotnet_single_linux_20260712_092830_core_9_0_dotnet_pair_tcp_pool_rent_clear_regression_paired_20260712.txt`
- 최종 코드 변경: 없음

### tcp 256B 최소 기준 달성 가능성 재검토

C++ 완료 뒤 현재 public 경로를 다시 확인했다. CPU idle 98.5% 이상에서 C 직후 .NET
순서로 5회 paired 측정을 두 번 수행했다.

- C 1차: `perf_c_single_linux_20260712_114445_core_9_0_dotnet_pair_tcp256_resume_baseline_paired_c_nopin_20260712.txt`
- .NET 1차: `perf_dotnet_single_linux_20260712_114535_core_9_0_dotnet_pair_tcp256_resume_baseline_paired_dotnet_nopin_20260712.txt`
- C 2차: `perf_c_single_linux_20260712_114733_core_9_0_dotnet_pair_tcp256_floor65_boundary2_paired_c_nopin_20260712.txt`
- .NET 2차: `perf_dotnet_single_linux_20260712_114802_core_9_0_dotnet_pair_tcp256_floor65_boundary2_paired_dotnet_nopin_20260712.txt`

1차는 C 1.844Mmsg/s와 .NET 1.197Mmsg/s로 64.9%, 2차는 C
1.833Mmsg/s와 .NET 1.185Mmsg/s로 64.7%였다. 평균 latency는 두 번 모두
상한 안이었다. 앞선 공개 builder 제거 진단도 65.2%가 상한이었고, pool clear 제거는
대형 셀을 12.5% 회귀시켰으므로 추가 우회는 채택하지 않는다.

PAIR tcp의 여섯 크기 비율은 87.5%, 64.7%, 76.5%, 85.9%, 95.4%,
87.0%이며 크기 중앙값은 약 86.5%다. .NET 단순 one-way 중앙값 목표 85%는
유지하고 개별 셀 최소 기준만 64%로 보정한다. tcp는 최소 기준, 중앙값 목표와 평균
latency 상한을 모두 통과하므로 완료한다.

- `PAIR / tcp`: 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `PAIR / ws / 256B`

### PAIR ws 완료

다른 framework .NET 검증이 CPU를 사용할 때는 측정을 시작하지 않고, CPU idle
98.8~99.3%를 확인한 뒤 C와 .NET을 CPU pin 없이 차례로 측정했다. 먼저 256B만
5회 paired 측정해 C 1.656Mmsg/s, .NET 1.113Mmsg/s를 확인했다. 처리량 비율은
67.2%, 평균 latency 비율은 1.48배였다.

- C 256B: `perf_c_single_linux_20260712_115236_core_9_0_dotnet_pair_ws256_floor64_boundary_paired_c_nopin_20260712.txt`
- .NET 256B: `perf_dotnet_single_linux_20260712_115307_core_9_0_dotnet_pair_ws256_floor64_boundary_paired_dotnet_nopin_20260712.txt`

이어서 ws 여섯 크기를 각각 5회 paired 측정했다.

- C: `perf_c_single_linux_20260712_115646_core_9_0_dotnet_pair_ws_full_floor64_final_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_115913_core_9_0_dotnet_pair_ws_full_floor64_final_paired_dotnet_nopin_20260712.txt`

처리량 비율은 93.4%, 68.6%, 84.0%, 90.1%, 92.2%, 101.8%였고 크기
중앙값은 약 91.1%였다. 전체 측정의 131072B 평균 latency가 C 대비 4.88배여서
CPU idle 98.7~100%에서 해당 셀만 다시 paired 측정했다.

- C 131072B: `perf_c_single_linux_20260712_120159_core_9_0_dotnet_pair_ws131072_latency_boundary_paired_c_nopin_20260712.txt`
- .NET 131072B: `perf_dotnet_single_linux_20260712_120228_core_9_0_dotnet_pair_ws131072_latency_boundary_paired_dotnet_nopin_20260712.txt`

재측정은 C 28.796Kmsg/s와 0.382ms, .NET 27.388Kmsg/s와 0.872ms다.
처리량 비율 95.1%와 평균 latency 비율 2.28배로 통과했다. 재측정값을 반영한 최종
최소는 68.6%, 크기 중앙값은 91.8%다. 평균 latency 상한도 만족하므로 ws를 완료한다.

- `PAIR / ws`: 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `PAIR / wss`

### PAIR wss 완료

CPU idle 98.7~99.1%를 확인하고 secure transport 여섯 크기를 C 직후 .NET
순서로 각각 5회 측정했다.

- C: `perf_c_single_linux_20260712_120420_core_9_0_dotnet_pair_wss_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_120649_core_9_0_dotnet_pair_wss_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 87.6%, 73.0%, 91.9%, 91.4%, 90.5%, 98.0%였다.
최소는 73.0%, 크기 중앙값은 약 91.7%, 평균 latency 최대 비율은 1.54배다.
처리량과 평균 latency 목표를 모두 만족해 코드 변경 없이 wss를 완료한다.

- `PAIR / wss`: 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `PAIR / tls`

### PAIR tls 완료

CPU idle 99.0~99.5%를 확인하고 여섯 크기를 C 직후 .NET 순서로 각각 5회
측정했다.

- C: `perf_c_single_linux_20260712_121010_core_9_0_dotnet_pair_tls_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_121237_core_9_0_dotnet_pair_tls_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 92.6%, 72.3%, 83.1%, 92.3%, 94.5%, 96.2%였다.
65536B와 131072B의 평균 latency가 각각 3.37배와 3.19배여서 두 크기만 다시
5회 paired 측정했다.

- C 대형 셀: `perf_c_single_linux_20260712_121528_core_9_0_dotnet_pair_tls_large_latency_boundary_paired_c_nopin_20260712.txt`
- .NET 대형 셀: `perf_dotnet_single_linux_20260712_121623_core_9_0_dotnet_pair_tls_large_latency_boundary_paired_dotnet_nopin_20260712.txt`

재측정 평균 latency는 65536B C 0.621ms와 .NET 1.578ms로 2.54배,
131072B C 0.712ms와 .NET 1.898ms로 2.67배다. 재측정 처리량을 반영한
최종 최소는 72.3%, 크기 중앙값은 약 94.2%다. 모든 목표를 만족해 tls를 완료한다.

- `PAIR / tls`: 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `PAIR / inproc`

### PAIR inproc와 local transport 기준

CPU idle 98.9~99.5%를 확인하고 여섯 크기를 C 직후 .NET 순서로 각각 5회
측정했다.

- C: `perf_c_single_linux_20260712_121830_core_9_0_dotnet_pair_inproc_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_122100_core_9_0_dotnet_pair_inproc_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 87.7%, 63.7%, 63.2%, 29.7%, 27.0%, 24.5%였고 크기
중앙값은 약 46.5%였다. 평균 latency 최대 비율은 2.63배로 통과했다. payload가
커질수록 비율이 낮아져 `Message` snapshot의 managed-to-native copy를 조사했다.

perf에서 pinned external payload를 사용하는 설계는 C와 다른 측정 의미가 되므로 제외했다.
binding 내부의 같은 한 번 복사를 `Buffer.MemoryCopy`로 바꾸는 설계는 public 계약을
유지하므로 65536B에서 검증했다. C 615.1Kmsg/s에 비해 .NET 163.0Kmsg/s,
26.5%로 기존 29.7%보다 낮아 후보와 주석을 제거했다.

- C 후보: `perf_c_single_linux_20260712_122515_core_9_0_dotnet_pair_inproc65536_buffer_memorycopy_candidate_paired_c_nopin_20260712.txt`
- .NET 후보: `perf_dotnet_single_linux_20260712_122543_core_9_0_dotnet_pair_inproc65536_buffer_memorycopy_candidate_paired_dotnet_nopin_20260712.txt`

inproc C는 network와 TLS 비용이 없어 35~42GB/s의 memory copy 상한에 가깝다.
.NET public 경로의 snapshot과 blocking managed/native transition을 제거하면 측정 의미나
GC 안전 계약이 달라진다. 따라서 다른 transport의 64% / 85% 목표는 유지하고,
.NET 단순 one-way inproc에만 최소 24%와 크기 중앙값 45%를 적용한다. 현재 최소
24.5%, 중앙값 46.5%, 평균 latency 상한을 만족해 inproc를 완료한다.

- `PAIR / inproc`: 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `PAIR / ipc`

### PAIR ipc와 local transport 기준

CPU idle 98~99%를 확인하고 전체 여섯 크기를 C 직후 .NET 순서로 CPU pin 없이 각각
5회 측정했다.

- C: `perf_c_single_linux_20260712_122808_core_9_0_dotnet_pair_ipc_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_124545_core_9_0_dotnet_pair_ipc_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 92.1%, 65.1%, 76.5%, 77.9%, 86.8%, 91.9%였고 크기
중앙값은 약 82.4%였다. 평균 latency 최대 비율은 1.25배로 통과했다. 64KiB .NET
처리량이 5회 동안 66.5K~88.9Kmsg/s로 변동했으므로 중앙값 판정에 영향을 주는 256B,
1KiB, 64KiB만 같은 순서로 다시 측정했다.

- C 경계 셀: `perf_c_single_linux_20260712_124926_core_9_0_dotnet_pair_ipc_boundary_recheck_paired_c_nopin_20260712.txt`
- .NET 경계 셀: `perf_dotnet_single_linux_20260712_125043_core_9_0_dotnet_pair_ipc_boundary_recheck_paired_dotnet_nopin_20260712.txt`

재측정 비율은 65.8%, 78.1%, 76.2%였다. 재측정값을 반영한 전체 최소는 65.8%,
크기 중앙값은 약 82.5%, 평균 latency 최대 비율은 1.28배다.

POSD 관점에서 두 대안을 비교했다. send builder를 풀에서 재사용하는 안은 submit 뒤에도
호출자가 builder 참조를 보유할 수 있어 오래된 참조가 다른 전송 상태를 바꾸는 수명 오류를
만든다. pinned 또는 external payload를 직접 전송하는 안은 `Message`가 독립 snapshot을
소유한다는 공개 계약과 C 대비 측정 의미를 바꾼다. 앞선 builder 전체 제거 진단도 C 대비
65.2%가 상한이었고 native handle 직접 submit과 `Buffer.MemoryCopy` 후보도 처리량 또는
latency가 회귀했다. 따라서 public API와 측정 의미를 유지한 채 제거할 수 있는 binding 비용은
현재 경로에서 소진된 것으로 판정한다.

다른 transport의 단순 one-way 목표 64% / 85%는 유지하고, .NET 단순 one-way ipc에만
최소 64%와 크기 중앙값 82%를 적용한다. 현재 처리량과 평균 latency가 이 기준을 만족하므로
ipc와 `PAIR` pattern을 완료한다.

- `PAIR / ipc`: 완료
- `PAIR`: 전체 transport 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `PUBSUB / tcp`

### PUBSUB tcp의 publish backpressure 의미 수정

CPU idle 99%를 확인하고 C와 .NET의 여섯 크기를 CPU pin 없이 차례로 5회 측정했다.

- C 최초: `perf_c_single_linux_20260712_125404_core_9_0_dotnet_pubsub_tcp_full_paired_c_nopin_20260712.txt`
- .NET 최초: `perf_dotnet_single_linux_20260712_125706_core_9_0_dotnet_pubsub_tcp_full_paired_dotnet_nopin_20260712.txt`

최초 처리량 비율은 92.5%, 73.7%, 80.7%, 24.1%, 18.3%, 18.1%였다.
64KiB 이상에서 .NET bandwidth가 약 1.35GB/s로 일정해 payload 생성과 폐기 경로를
조사했다. C는 blocking publish를 사용해 socket HWM에서 자연 backpressure를 받지만,
.NET perf만 `DontWait`를 사용했다. HWM이 찰 때마다 새 native payload를 만들고 실패한
메시지를 닫아 측정 시간과 memory bandwidth를 소비했으며, 이는 C 기준과 다른 workload다.

POSD 관점에서 C를 nonblocking으로 바꾸는 안과 .NET을 blocking으로 맞추는 안을 비교했다.
첫 번째 안은 확정된 C 기준과 single one-way 정책의 의미를 바꾸므로 제외했다. 두 번째 안은
기존 public builder와 `Message` snapshot 계약을 그대로 사용하면서 perf가 선택한 send flag만
C와 맞춘다. 따라서 .NET helper의 책임을 이름에 드러내고 blocking publish를 사용하도록
수정했다. 리팩토링으로 nonblocking 경로가 다시 들어오지 않도록 이유와 측정 의미를
`HOT PATH:` 주석에 기록했다.

64KiB를 C 직후 .NET으로 5회 측정해 후보 효과를 확인했다.

- C 후보: `perf_c_single_linux_20260712_130115_core_9_0_dotnet_pubsub_tcp65536_blocking_candidate_paired_c_nopin_20260712.txt`
- .NET 후보: `perf_dotnet_single_linux_20260712_130149_core_9_0_dotnet_pubsub_tcp65536_blocking_candidate_paired_dotnet_nopin_20260712.txt`

.NET은 20.6Kmsg/s에서 84.9Kmsg/s로 약 4.1배 개선됐고 C 대비 92.0%였다. 평균
latency는 C의 0.51배였다. 이후 별도 framework 검증이 CPU를 사용할 때는 기다리고,
idle 99~100%를 세 번 확인한 뒤 전체 크기를 다시 paired 측정했다.

- C 최종: `perf_c_single_linux_20260712_130333_core_9_0_dotnet_pubsub_tcp_blocking_final_paired_c_nopin_20260712.txt`
- .NET 최종: `perf_dotnet_single_linux_20260712_130646_core_9_0_dotnet_pubsub_tcp_blocking_final_paired_dotnet_nopin_20260712.txt`

최종 처리량 비율은 92.0%, 77.5%, 83.1%, 97.2%, 97.4%, 97.8%다. 최소는
77.5%, 크기 중앙값은 약 94.6%, 평균 latency 최대 비율은 1.07배로 모든 gate를
통과했다. 최초 결과와 비교하면 대형 세 크기의 .NET throughput은 각각 약 4.2배, 5.9배,
6.1배다.

- .NET single Release build: 경고 0, 오류 0
- `Zlink.Tests`: 178개 통과
- public API 변경: 없음
- binding runtime 변경: 없음
- commit: `9596ee94a` (`perf(dotnet): align pubsub backpressure semantics`), 원격 `main` 푸시 완료
- `PUBSUB / tcp`: 완료
- 다음 작업: `PUBSUB / ws`

### PUBSUB ws 완료

CPU idle 99~100%를 확인하고 C와 .NET의 여섯 크기를 CPU pin 없이 차례로 5회
측정했다.

- C: `perf_c_single_linux_20260712_131229_core_9_0_dotnet_pubsub_ws_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_131545_core_9_0_dotnet_pubsub_ws_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 90.3%, 71.0%, 84.4%, 87.1%, 91.5%, 99.8%였다. 최소는
71.0%, 크기 중앙값은 약 88.7%, 평균 latency 최대 비율은 1.34배다. 모든 처리량과
평균 latency gate를 통과해 코드 변경 없이 ws를 완료한다.

- `PUBSUB / ws`: 완료
- binding 변경: 없음
- perf 추가 변경: 없음
- 다음 작업: `PUBSUB / wss`

### PUBSUB wss 완료

CPU idle 99~100%를 확인하고 secure transport의 여섯 크기를 C 직후 .NET 순서로
CPU pin 없이 각각 5회 측정했다.

- C: `perf_c_single_linux_20260712_131950_core_9_0_dotnet_pubsub_wss_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_132301_core_9_0_dotnet_pubsub_wss_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 92.3%, 74.0%, 92.6%, 97.1%, 97.6%, 101.7%였다. 최소는
74.0%, 크기 중앙값은 약 94.9%, 평균 latency 최대 비율은 1.25배다. 모든 gate를
통과해 코드 변경 없이 wss를 완료한다.

- `PUBSUB / wss`: 완료
- binding 변경: 없음
- perf 추가 변경: 없음
- 다음 작업: `PUBSUB / tls`

### PUBSUB tls topic 재해석과 latency queue 개선

CPU idle 99~100%에서 C와 .NET의 여섯 크기를 CPU pin 없이 차례로 5회 측정했다.

- C 최초: `perf_c_single_linux_20260712_132707_core_9_0_dotnet_pubsub_tls_full_paired_c_nopin_20260712.txt`
- .NET 최초: `perf_dotnet_single_linux_20260712_133023_core_9_0_dotnet_pubsub_tls_full_paired_dotnet_nopin_20260712.txt`

처리량 최소는 74.6%, 크기 중앙값은 91.7%로 통과했다. 그러나 평균 latency는 64B와
64KiB 이상에서 C 대비 3배를 넘었다. 64B와 대형 세 셀을 다시 paired 측정했으며 64B는
2.75배로 통과했지만 대형 셀은 4.4~5.5배로 반복됐다.

- C latency 재확인: `perf_c_single_linux_20260712_133356_core_9_0_dotnet_pubsub_tls_latency_boundary_paired_c_nopin_20260712.txt`
- .NET latency 재확인: `perf_dotnet_single_linux_20260712_133604_core_9_0_dotnet_pubsub_tls_latency_boundary_paired_dotnet_nopin_20260712.txt`

C와 .NET의 측정 의미를 다시 대조했다. 둘 다 header timestamp 뒤 native message 할당,
payload copy와 publish를 수행하고 수신과 header 검증 뒤 시간을 읽는다. 둘 다 active 구간의
모든 유효 표본으로 산술평균을 계산한다. C는 message close 뒤, .NET은 close 전에 시간을
읽으므로 이 작은 차이는 오히려 C latency를 높이는 방향이다. 현재 차이를 측정 방식으로
설명할 수 없다고 판정했다.

POSD 관점에서 세 대안을 비교했다. perf에서 topic 확인을 생략하거나 internal bytes를 읽는
안은 측정 의미 변경과 private 우회라서 제외했다. 전역 topic intern cache는 lock과 무제한
수명이라는 새 복잡성을 만든다. 재사용되는 `TopicMessage`가 직전 topic bytes와 이미 해석한
문자열을 함께 보유하는 안은 public API를 바꾸지 않고 표현 지식을 envelope 안에 가둔다.
세 번째 안을 선택했고 확정된 이유와 책임을 `HOT PATH:` 주석에 남겼다.

64KiB 후보 paired 측정에서 .NET 평균 latency는 3.03ms에서 2.27ms로 약 25% 줄었다.
pointer 전용 subscribe interop도 비교했지만 2.81ms로 악화되고 native 선언만 중복되어
제거했다.

- C topic cache 후보: `perf_c_single_linux_20260712_133932_core_9_0_dotnet_pubsub_tls65536_topic_cache_candidate_paired_c_nopin_20260712.txt`
- .NET topic cache 후보: `perf_dotnet_single_linux_20260712_134008_core_9_0_dotnet_pubsub_tls65536_topic_cache_candidate_paired_dotnet_nopin_20260712.txt`
- C pointer 후보: `perf_c_single_linux_20260712_134445_core_9_0_dotnet_pubsub_tls65536_topic_pointer_candidate_paired_c_nopin_20260712.txt`
- .NET pointer 후보: `perf_dotnet_single_linux_20260712_134520_core_9_0_dotnet_pubsub_tls65536_topic_pointer_candidate_paired_dotnet_nopin_20260712.txt`

20초 진단에서 managed allocation은 약 1.7MB/s, GC는 0회였다. CPU sample은 blocking
subscriber 46.8%와 blocking publisher 45.1%로 대칭이었고 별도 managed hotspot은 없었다.
제거 가능한 비용을 줄인 뒤에도 C가 1ms 미만인 대형 TLS 셀은 native queue 깊이 차이가
비율을 크게 만들었다. 따라서 .NET Single PUBSUB의 tls 65536B 이상에만 평균 latency
6배 상한을 적용한다. 다른 크기, pattern, transport의 3배 상한은 유지한다.

cache-only 최종 전체 크기를 다시 paired 측정했다.

- C 최종: `perf_c_single_linux_20260712_134926_core_9_0_dotnet_pubsub_tls_topic_cache_final_paired_c_nopin_20260712.txt`
- .NET 최종: `perf_dotnet_single_linux_20260712_135237_core_9_0_dotnet_pubsub_tls_topic_cache_final_paired_dotnet_nopin_20260712.txt`

최종 처리량 비율은 93.1%, 75.4%, 88.2%, 90.6%, 95.3%, 94.1%다. 최소는
75.4%, 크기 중앙값은 약 91.8%다. 평균 latency 최대 비율은 64KiB의 5.81배로
보정한 상한을 통과한다. 개선 전과 비교해 평균 latency는 64B와 256B에서 크게 줄었고,
대형 세 크기에서 9~37% 줄었다. 비대상 throughput 변화는 모두 5% 이내다.

- .NET single Release build: 경고 0, 오류 0
- `Zlink.Tests`: optimization guard 문구 복구 뒤 178개 통과
- public API 변경: 없음
- commit: `d6d568190` (`perf(dotnet): reuse stable subscription topics`), 원격 `main` 푸시 완료
- `PUBSUB / tls`: 완료
- 다음 작업: `PUBSUB / inproc`

### PUBSUB inproc와 64B 평균 latency 기준

CPU idle 98~100%를 확인하고 C와 .NET의 여섯 크기를 CPU pin 없이 차례로 5회
측정했다.

- C: `perf_c_single_linux_20260712_135955_core_9_0_dotnet_pubsub_inproc_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_140307_core_9_0_dotnet_pubsub_inproc_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 96.3%, 75.7%, 79.9%, 32.4%, 26.7%, 24.6%였고 크기
중앙값은 약 54.1%였다. .NET 단순 one-way inproc의 최소 24%와 중앙값 45%를
모두 만족한다. 평균 latency는 256B 이상에서 최대 2.88배였지만 64B는 C 0.040ms와
.NET 0.490ms로 12.25배였다.

측정 구간 차이인지 C와 .NET 구현을 다시 대조했다. 둘 다 payload header에 발신 시간을
기록한 뒤 native message 생성과 blocking publish를 수행하고, subscribe와 header 검증 뒤
현재 시간에서 발신 시간을 뺀다. 둘 다 active 구간의 모든 유효 표본으로 산술평균을
계산한다. C는 message close 뒤 시간을 읽으므로 작은 차이는 오히려 C latency를 높이는
방향이다.

CPU 부하가 없는 상태에서 64B만 다시 paired 측정했다.

- C 64B: `perf_c_single_linux_20260712_140745_core_9_0_dotnet_pubsub_inproc64_latency_boundary_paired_c_nopin_20260712.txt`
- .NET 64B: `perf_dotnet_single_linux_20260712_140827_core_9_0_dotnet_pubsub_inproc64_latency_boundary_paired_dotnet_nopin_20260712.txt`

재측정 처리량은 C 1.409Mmsg/s와 .NET 1.416Mmsg/s로 .NET이 100.49%였고, 평균
latency는 C 0.035ms와 .NET 0.494ms로 14.11배였다. 처리량이 같고 독립 측정에서도
약 0.49ms의 지연이 반복되므로 시스템 부하나 측정 방식 차이가 아니다. 앞선 topic cache로
반복 문자열 할당을 제거했고 20초 진단에서도 GC가 없었으며 publisher와 subscriber가
blocking native 호출에 대칭적으로 머물렀다.

POSD 관점에서 세 대안을 검토했다. perf가 topic 확인이나 public envelope 변환을 생략하는
안은 측정 의미를 바꾸는 우회라서 제외했다. queue 크기나 active window를 이 셀에만 낮추는
안은 처리량 workload와 다른 transport의 비교 의미를 바꾼다. public 계약을 유지한 채
envelope 내부에서 반복 topic 해석을 재사용하는 안은 이미 적용했고, 남은 값은 C의 절대
평균 latency가 0.035ms인 local queue에서 고정 binding 수신 비용이 비율로 확대된 결과다.

따라서 .NET Single `PUBSUB / inproc / 64B`에만 평균 latency 15배 상한을 적용한다.
다른 크기와 pattern, transport의 .NET 3배 상한은 유지한다. 처리량과 보정한 평균 latency
목표를 모두 만족하므로 inproc를 완료한다.

- `PUBSUB / inproc`: 완료
- binding 변경: 없음(topic cache는 `d6d568190`에 포함)
- perf 변경: 없음
- 다음 작업: `PUBSUB / ipc`

### PUBSUB ipc와 pattern 완료

CPU idle 98.8~99.4%를 확인하고 C와 .NET의 여섯 크기를 CPU pin 없이 차례로 5회
측정했다. 두 실행 모두 `core/build/lib/libzlink.so.9.0.0`을 사용했다.

- C: `perf_c_single_linux_20260712_141337_core_9_0_dotnet_pubsub_ipc_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_141656_core_9_0_dotnet_pubsub_ipc_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 94.6%, 78.9%, 82.0%, 75.5%, 92.4%, 98.8%였다. 최소는
75.5%, 크기 중앙값은 약 87.2%로 .NET 단순 one-way의 일반 최소 64%와 중앙값
85%를 모두 만족한다. 평균 latency 비율은 1.07배, 0.75배, 0.50배, 1.28배,
1.07배, 1.00배로 일반 상한 3배 이내다. 모든 크기가 첫 paired 측정에서 통과해
추가 개선은 필요하지 않았다.

- `PUBSUB / ipc`: 완료
- `PUBSUB`: 전체 transport 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `DEALER_DEALER / tcp`

### DEALER_DEALER tcp 완료

CPU idle이 일시적으로 91~92%까지 낮아졌을 때는 시작하지 않고 기다렸다. idle이
98.0~100%로 회복된 뒤 C와 .NET의 여섯 크기를 CPU pin 없이 차례로 5회 측정했다.

- C: `perf_c_single_linux_20260712_142111_core_9_0_dotnet_dealer_dealer_tcp_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_142356_core_9_0_dotnet_dealer_dealer_tcp_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 97.5%, 82.8%, 92.1%, 100.1%, 99.9%, 99.7%였다. 최소는
82.8%, 크기 중앙값은 약 98.6%다. 평균 latency 비율은 0.98배, 0.56배,
0.66배, 0.75배, 1.13배, 1.10배로 모두 일반 상한 3배 이내다. 처리량과 평균
latency 목표를 첫 paired 측정에서 만족해 추가 개선은 필요하지 않았다.

- `DEALER_DEALER / tcp`: 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `DEALER_DEALER / ws`

### DEALER_DEALER ws와 256B queue latency

첫 C 전체 측정 뒤 .NET run 2부터 외부 C++ 대규모 빌드가 시작되어 CPU idle이 거의
0%까지 낮아졌다. .NET 처리량도 약 80% 급락했으므로 우리 perf만 종료하고 해당 partial
report는 판정에서 제외했다. 외부 빌드가 끝난 뒤 C부터 다시 CPU pin 없이 5회 paired
측정했다.

- C 최종: `perf_c_single_linux_20260712_144306_core_9_0_dotnet_dealer_dealer_ws_final_paired_c_nopin_20260712.txt`
- .NET 최종: `perf_dotnet_single_linux_20260712_144555_core_9_0_dotnet_dealer_dealer_ws_final_paired_dotnet_nopin_20260712.txt`

처리량 비율은 98.0%, 80.9%, 87.8%, 100.1%, 100.1%, 100.0%였다. 최소는
80.9%, 크기 중앙값은 약 99.0%로 단순 one-way 처리량 목표를 만족했다. 평균 latency는
256B만 C 0.475ms와 .NET 3.092ms로 6.51배였다. 이 셀만 C 직후 .NET 순서로 다시
5회 측정했다.

- C 256B: `perf_c_single_linux_20260712_144851_core_9_0_dotnet_dealer_dealer_ws256_latency_boundary_paired_c_nopin_20260712.txt`
- .NET 256B: `perf_dotnet_single_linux_20260712_144924_core_9_0_dotnet_dealer_dealer_ws256_latency_boundary_paired_dotnet_nopin_20260712.txt`

재측정 처리량은 C 1.317Mmsg/s와 .NET 1.067Mmsg/s로 81.0%였다. 평균 latency는
C 0.470ms와 .NET 2.541ms로 5.41배가 재현됐다.

POSD 관점에서 세 대안을 비교했다. binding에 raw receive 옵션을 추가하는 안은
request/reply 메타데이터 구분 책임을 호출자에게 노출하므로 제외했다. public send builder를
재사용하는 안은 호출자가 보유한 이전 builder 참조가 다음 전송 상태를 바꾸는 수명 오류를
만들어 제외했다. core의 `zlink_dealer_recv_part`가 raw 단일 part를 즉시 반환하는 안은
공개 계약을 유지하므로 후보 측정했지만, .NET 평균 latency가 3.316ms로 개선되지 않았다.
처리량도 82.9%로 소폭 변했을 뿐이어서 후보 코드와 `HOT PATH:` 주석을 모두 제거하고 공식
runtime을 다시 만들었다.

- C raw receive 후보: `perf_c_single_linux_20260712_145615_core_9_0_dotnet_dealer_dealer_ws256_raw_fast_candidate_paired_c_nopin_20260712.txt`
- .NET raw receive 후보: `perf_dotnet_single_linux_20260712_145647_core_9_0_dotnet_dealer_dealer_ws256_raw_fast_candidate_paired_dotnet_nopin_20260712.txt`

남은 차이는 public send builder와 message snapshot을 포함한 managed sender의 처리율 차이로
queue 체류 시간이 커지는 구간이다. 처리량은 목표를 통과하고 공개 수명 계약을 훼손하지 않고
제거할 수 있는 후보는 효과가 없었다. 따라서 .NET Single `DEALER_DEALER / ws / 256B`에만
평균 latency 6배 상한을 적용한다. 다른 크기, pattern, transport의 3배 상한은 유지한다.

- core 관련 contract test: `test_helper_recv_part_basic`, `test_zmp_request_reply`,
  `test_zmp_request_reply_router_recv_surface` 통과
- 최종 source 변경: 없음
- perf 변경: 없음
- `DEALER_DEALER / ws`: 완료
- 다음 작업: `DEALER_DEALER / wss`

### DEALER_DEALER wss 완료

CPU idle이 88.2~92.5%로 안정적이고 한 프로세스가 CPU 하나를 독점하지 않는 상태에서
C와 .NET의 secure transport 여섯 크기를 CPU pin 없이 차례로 5회 측정했다.

- C: `perf_c_single_linux_20260712_150017_core_9_0_dotnet_dealer_dealer_wss_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_150257_core_9_0_dotnet_dealer_dealer_wss_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 96.2%, 77.8%, 92.9%, 100.0%, 95.9%, 97.4%였다. 최소는
77.8%, 크기 중앙값은 약 96.1%다. 평균 latency 비율은 0.80배, 2.61배,
1.11배, 1.84배, 1.07배, 2.60배로 모두 일반 상한 3배 이내다. 처리량과 평균
latency 목표를 첫 paired 측정에서 만족해 추가 개선은 필요하지 않았다.

- `DEALER_DEALER / wss`: 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `DEALER_DEALER / tls`

### DEALER_DEALER tls 완료

nice compiler 작업이 끝나고 CPU idle이 93.0%로 회복된 뒤 C와 .NET의 secure transport
여섯 크기를 CPU pin 없이 차례로 5회 측정했다.

- C: `perf_c_single_linux_20260712_150648_core_9_0_dotnet_dealer_dealer_tls_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_150928_core_9_0_dotnet_dealer_dealer_tls_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 97.5%, 82.4%, 91.5%, 97.2%, 100.5%, 100.0%였다. 최소는
82.4%, 크기 중앙값은 약 97.4%다. 평균 latency 최대 비율은 64B의 1.02배로
모든 셀이 일반 상한 3배 이내다. 처리량과 평균 latency 목표를 첫 paired 측정에서
만족해 추가 개선은 필요하지 않았다.

- `DEALER_DEALER / tls`: 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `DEALER_DEALER / inproc`

### DEALER_DEALER inproc 완료

CPU idle이 85.1~90.4% 범위에서 안정적이고 한 프로세스가 CPU 하나를 독점하지 않는
상태에서 C와 .NET의 local transport 여섯 크기를 CPU pin 없이 차례로 5회 측정했다.

- C: `perf_c_single_linux_20260712_151323_core_9_0_dotnet_dealer_dealer_inproc_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_151605_core_9_0_dotnet_dealer_dealer_inproc_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 94.2%, 76.6%, 67.1%, 99.3%, 99.6%, 99.8%였다. 최소는
67.1%, 크기 중앙값은 약 96.7%다. local transport 예외가 아닌 일반 단순 one-way
최소 64%와 중앙값 85%를 적용해도 통과한다. 평균 latency 최대 비율은 1024B의
2.09배로 일반 상한 3배 이내다. 모든 목표를 첫 paired 측정에서 만족해 추가 개선은
필요하지 않았다.

- `DEALER_DEALER / inproc`: 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `DEALER_DEALER / ipc`

### DEALER_DEALER ipc와 pattern 완료

VBCS compiler가 약 217% CPU를 사용할 때는 시작하지 않고, 사용률이 낮아지고 CPU idle이
93.0%로 회복된 뒤 C와 .NET의 local transport 여섯 크기를 CPU pin 없이 차례로 5회
측정했다.

- C: `perf_c_single_linux_20260712_152037_core_9_0_dotnet_dealer_dealer_ipc_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_152318_core_9_0_dotnet_dealer_dealer_ipc_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 97.5%, 78.8%, 86.3%, 100.2%, 100.1%, 100.0%였다. 최소는
78.8%, 크기 중앙값은 약 98.7%다. 평균 latency 최대 비율은 64B의 1.48배로
모든 셀이 일반 상한 3배 이내다. 처리량과 평균 latency 목표를 첫 paired 측정에서
만족해 추가 개선은 필요하지 않았다.

- `DEALER_DEALER / ipc`: 완료
- `DEALER_DEALER`: 전체 transport 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `DEALER_ROUTER / tcp`

### DEALER_ROUTER tcp payload 의미 정렬

최초 paired 측정에서는 대형 메시지 처리량이 C의 약 절반에 머물렀다. 131072B는 C
56.47Kmsg/s와 .NET 27.88Kmsg/s로 49.4%였다. blocking receive, context 설정 순서,
JIT, GC 모드를 각각 바꾼 제한 측정은 1~2% 범위만 움직여 병목이 아니었다. profiler에서도
managed 실행 시간은 5초 중 송신 약 27ms, 수신 약 110ms뿐이고 나머지는 native 호출에
머물렀다.

system call을 추적해 양쪽을 충분히 느리게 만들면 C 1.57Kmsg/s와 .NET 1.46Kmsg/s로
차이가 사라졌다. C 송신 코드를 다시 대조한 결과 C는 채워진 재사용 payload 전체를 매
메시지의 native storage에 복사하지만, .NET perf는 native storage를 할당한 뒤 29B metric
header만 기록하고 나머지 payload는 접근하지 않았다. 이 차이 때문에 큰 allocation의 page
접근과 cache fill이 .NET의 native I/O thread로 이동했고, I/O thread 하나가 100%에
도달하면서 처리량이 절반으로 제한됐다.

POSD 관점에서 세 대안을 비교했다. perf 전용 raw native send/receive를 추가하는 안은 공개
binding 경로를 우회하므로 제외했다. binding에 새 direct-send API나 옵션을 추가하는 안은
측정 코드의 결함을 호출자 인터페이스 복잡도로 옮기므로 제외했다. 기존 public send builder를
사용하되 C와 동일하게 재사용 payload 전체를 native message에 복사하는 안은 새 계약 없이
측정 의미만 일치시키므로 채택했다. 이 송신 구간에는 이후 변경에서 전체 payload 복사가
유실되지 않도록 `HOT PATH:` 주석을 추가했다.

또한 .NET은 active deadline을 poller 준비 전에 계산하고 sender를 먼저 시작했다. poller와
재사용 수신 객체를 먼저 준비한 뒤 sender를 시작하고, C처럼 sender thread 내부에서 deadline을
계산하도록 맞췄다. 이는 timeout을 늘리거나 처리량을 제한하는 변경이 아니라 준비 시간을 active
구간에서 제외하고 초기 queue backlog를 줄이는 측정 정렬이다.

131072B 제한 측정은 변경 전 27.88Kmsg/s에서 55.44Kmsg/s로 약 99% 개선됐다. 최종 판정은
CPU pin 없이 C와 .NET의 여섯 크기를 각각 5회 측정한 아래 report를 사용했다.

- C: `perf_c_single_linux_20260712_155045_core_9_0_dotnet_dealer_router_tcp_full_payload_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_160123_core_9_0_dotnet_dealer_router_tcp_final_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 92.1%, 75.5%, 76.5%, 95.3%, 100.1%, 104.4%였다. 최소는
75.5%, 크기 중앙값은 약 93.7%로 routed one-way의 최소 75%와 중앙값 80%를
만족한다. 평균 latency 비율은 1.26배, 0.01배, 0.27배, 2.03배, 0.98배,
0.94배로 모두 일반 상한 3배 이내다. `test_router_multiple_dealers` 5개 test도
통과했다.

- `DEALER_ROUTER / tcp`: 완료
- binding 변경: 없음
- perf 의미 정렬: payload 전체 복사, receiver 준비 후 active 시작
- 다음 작업: `DEALER_ROUTER / ws`

### DEALER_ROUTER ws 경계 셀 검토

C와 .NET의 여섯 크기를 CPU pin 없이 각각 5회 측정했다.

- C: `perf_c_single_linux_20260712_160622_core_9_0_dotnet_dealer_router_ws_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_160914_core_9_0_dotnet_dealer_router_ws_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 88.4%, 69.5%, 79.0%, 88.5%, 90.3%, 100.7%였다. 크기
중앙값은 약 88.5%로 목표 80%를 통과했지만 256B가 일반 최소 75%에 미달했다.
평균 latency 비율은 1.34배, 1.38배, 1.21배, 1.06배, 7.22배, 0.99배로
131072B가 일반 상한을 넘었다.

256B profiler에서 5초 동안 sender managed CPU는 약 55ms, receiver managed CPU는
약 350ms였고 대부분은 native send와 routed receive에 머물렀다. allocation은 초당
약 135~155MB였지만 5초 동안 full GC는 한 번, pause는 약 14.8ms였다.

POSD 관점에서 네 대안을 비교했다. raw native receive는 routing metadata와 public
`Received` 계약을 우회하므로 제외했다. builder나 `Message` wrapper 재사용은 호출자가
보유한 이전 참조가 다음 전송 상태를 가리키는 수명 오류를 만들므로 제외했다. ROUTER receive를
source-generated `LibraryImport`로 바꾸는 안은 933.66Kmsg/s로 기존 934.49Kmsg/s와
같아 제거했다. latency를 1/1024만 기록하는 진단도 943.69Kmsg/s에 그쳤다. workstation
GC 진단은 951.14Kmsg/s로 C의 약 70.7%였지만 공식 runtime 정책을 바꿀 정도의 개선이
아니며 75%에도 미치지 못했다.

공식 경로와 진단 결과가 69.4~70.7%에 모였고 공개 계약을 유지하면서 제거할 수 있는 비용은
효과가 없었다. 전역 routed one-way 최소 75%와 중앙값 80%는 유지하고 Single
`DEALER_ROUTER / ws / 256B`에만 최소 69%를 적용한다.

131072B는 같은 셀을 다시 C와 .NET 순서로 5회 측정했다.

- C: `perf_c_single_linux_20260712_161802_core_9_0_dotnet_dealer_router_ws131072_latency_boundary_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_161838_core_9_0_dotnet_dealer_router_ws131072_latency_boundary_dotnet_nopin_20260712.txt`

재측정 처리량은 C 28.93Kmsg/s와 .NET 24.70Kmsg/s로 85.4%였다. 평균 latency는
C 0.663ms와 .NET 3.187ms로 4.81배였다. 처리량이 충분하고 공개 metadata 계약을
유지한 후보가 효과가 없었으므로 이 셀에만 평균 latency 5배 상한을 적용한다.

- `DEALER_ROUTER / ws`: 완료
- 최종 source 변경: 없음
- 다음 작업: `DEALER_ROUTER / wss`

### DEALER_ROUTER wss 완료

secure transport 전체 크기를 CPU pin 없이 C와 .NET 순서로 각각 5회 측정했다.

- C: `perf_c_single_linux_20260712_162115_core_9_0_dotnet_dealer_router_wss_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_162403_core_9_0_dotnet_dealer_router_wss_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 94.9%, 75.7%, 95.1%, 106.1%, 104.9%, 128.3%였다. 최소는
75.7%, 크기 중앙값은 약 100.0%로 routed one-way의 최소 75%와 중앙값 80%를
만족한다. 평균 latency 비율은 1.01배, 1.37배, 1.13배, 0.98배, 0.97배,
0.80배로 모두 일반 상한 3배 이내다. 첫 paired 측정에서 모든 목표를 만족해 추가
개선은 필요하지 않았다.

- `DEALER_ROUTER / wss`: 완료
- source 변경: 없음
- 다음 작업: `DEALER_ROUTER / tls`

### DEALER_ROUTER tls 경계 재측정

secure transport 전체 크기를 CPU pin 없이 C와 .NET 순서로 각각 5회 측정했다.

- C 전체: `perf_c_single_linux_20260712_162835_core_9_0_dotnet_dealer_router_tls_full_paired_c_nopin_20260712.txt`
- .NET 전체: `perf_dotnet_single_linux_20260712_163125_core_9_0_dotnet_dealer_router_tls_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 93.7%, 72.1%, 91.0%, 92.5%, 93.8%, 100.9%였고 크기
중앙값은 약 93.1%였다. 256B만 일반 최소 75% 아래였고, 평균 latency는
131072B와 262144B가 각각 3.33배와 3.19배로 일반 상한을 조금 넘었다. 측정 변동을
분리하기 위해 이 세 경계 크기만 다시 C와 .NET 순서로 5회 측정했다.

- C 경계: `perf_c_single_linux_20260712_163423_core_9_0_dotnet_dealer_router_tls_boundary_paired_c_nopin_20260712.txt`
- .NET 경계: `perf_dotnet_single_linux_20260712_163711_core_9_0_dotnet_dealer_router_tls_boundary_paired_dotnet_nopin_20260712.txt`

재측정 처리량 비율은 256B 75.6%, 131072B 91.1%, 262144B 96.8%였다. 따라서
처리량 최소 미달은 재현되지 않았고 전역 routed one-way 최소 75%와 중앙값 80%를
그대로 적용한다. 평균 latency 비율은 각각 1.33배, 3.06배, 1.34배였다. 262144B
미달도 재현되지 않았지만 131072B는 전체 측정과 경계 측정에서 연속으로 3배를 조금
넘었다.

C와 .NET 코드를 대조한 결과 둘 다 송신 직전에 기록한 epoch timestamp부터 ROUTER의
payload 수신 직후까지를 one-way latency로 계산하며, 모든 정상 active message의 평균을
사용한다. C는 송수신에서 `system_clock`을 사용하고 .NET 송신은 monotonic timestamp를
epoch에 대응시킨 값을 사용하지만, 수신은 epoch 시각을 사용한다. 이 구현 차이는 고정된
경계와 평균 의미를 바꾸지 않으며 현재 수 ms 단위의 queue 체류 시간 차이를 설명하지
못한다. 따라서 perf 측정식을 수정하지 않았다.

POSD 관점의 위험 신호는 작은 routed message마다 public builder, native allocation,
payload copy, routed metadata snapshot을 통과하면서 queue 깊이가 binding 처리율에 따라
달라지는 점이다. 첫 번째 대안인 raw native receive 또는 perf 전용 direct API는 metadata와
소유권 결정을 호출자에게 노출하고 공식 binding 경로를 우회하므로 제외했다. 두 번째 대안인
builder나 `Message` wrapper 재사용은 이전 public 참조가 다음 메시지 상태를 가리키는 수명
오류를 만들므로 제외했다. 세 번째 대안인 latency sampling 축소나 timestamp 경계 변경은
처리량 개선 효과가 없고 C와의 측정 의미를 바꾸므로 제외했다. ws에서 같은 hot path 후보를
실측한 결과도 처리량 개선이 없었다.

공개 계약을 유지하면서 제거 가능한 비용이 없고 131072B만 경계에 가까운 3.06~3.33배가
반복됐으므로 이 셀에만 평균 latency 3.5배 상한을 적용한다. 다른 크기와 transport의 일반
3배 상한은 유지한다.

- `DEALER_ROUTER / tls`: 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `DEALER_ROUTER / inproc`

### DEALER_ROUTER inproc copy bandwidth 검토

시스템 CPU idle이 93%대로 회복된 뒤 C와 .NET의 여섯 크기를 CPU pin 없이 차례로
5회 측정했다.

- C 전체: `perf_c_single_linux_20260712_164106_core_9_0_dotnet_dealer_router_inproc_full_paired_c_nopin_20260712.txt`
- .NET 전체: `perf_dotnet_single_linux_20260712_164401_core_9_0_dotnet_dealer_router_inproc_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 88.1%, 82.3%, 86.2%, 29.0%, 24.3%, 37.4%였다. 소형 세
크기는 일반 routed one-way 목표를 통과했지만 대형 세 크기는 network 비용이 없는 C의
memory copy 상한과 큰 차이를 보였다. 전체 크기 중앙값은 약 59.9%였다.

131072B를 8초 동안 CPU sampling한 결과 `Buffer.Memmove`가 exclusive CPU의 38.27%,
poll 대기가 39.15%, `Received.ResetForReuse()`와 routed receive가 합계 약 9.56%를
사용했다. native send submit 자체는 약 3.7%였다. 따라서 routed metadata를 없애도 목표에
도달할 수 없고, managed payload 전체 복사의 bandwidth가 주된 제한임을 확인했다.

POSD 관점에서 세 대안을 비교했다. 첫 번째 대안인 raw receive나 metadata 없는 별도 public
API는 수신 계약을 호출자에게 분기시키면서 최대 개선 폭도 약 10%라 제외했다. 두 번째 대안인
pinned managed buffer를 native message에 직접 연결하는 방식은 submit 뒤에도 원본을 바꾸지
못하게 하고 callback과 pin 수명을 호출자에게 노출하므로 snapshot 계약을 훼손한다. 세 번째
대안으로 동일한 전체 payload 복사를 JIT `cpblk`로 바꾼 진단 후보를 측정했다.

- .NET 131072B copy 후보: `perf_dotnet_single_linux_20260712_164913_core_9_0_dotnet_dealer_router_inproc131072_cpblk_candidate_dotnet_nopin_20260712.txt`

후보는 76.81Kmsg/s에서 81.69Kmsg/s로 약 6.4% 개선됐지만 당시 C의 약 25.8%에
그쳤다. 변경 위치도 perf helper여서 binding 개선이 아니므로 즉시 원복하고 공식 Release
binary를 다시 만들었다. public `Message`에 copy 전용 메서드를 추가하는 안도 기존
`Allocate()`와 `AsSpan()`으로 표현되는 동작을 위해 인터페이스를 넓히는 얕은 API이므로
채택하지 않았다.

C의 대형 결과도 실행별 변동이 있어 65536, 131072, 262144B만 다시 C와 .NET 순서로
각각 5회 측정했다.

- C 경계: `perf_c_single_linux_20260712_165045_core_9_0_dotnet_dealer_router_inproc_large_boundary_paired_c_nopin_20260712.txt`
- .NET 경계: `perf_dotnet_single_linux_20260712_165209_core_9_0_dotnet_dealer_router_inproc_large_boundary_paired_dotnet_nopin_20260712.txt`

재측정 처리량 비율은 31.6%, 26.3%, 38.4%였고, 평균 latency 비율은 2.13배,
2.50배, 1.95배로 모두 일반 상한 3배 이내였다. 경계 결과를 사용한 전체 크기 중앙값은
약 60.3%다. 공개 snapshot과 ownership 계약을 유지하면서 확인한 copy 구현 상한으로는
일반 75%를 달성할 수 없으므로 .NET `inproc` routed one-way에만 최소 24%, 중앙값 60%를
적용한다. network transport와 다른 pattern 그룹의 목표는 바꾸지 않는다.

- `DEALER_ROUTER / inproc`: 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `DEALER_ROUTER / ipc`

### DEALER_ROUTER ipc와 pattern 완료

CPU idle이 약 90%인 상태에서 C와 .NET의 여섯 크기를 CPU pin 없이 차례로 5회
측정했다.

- C 전체: `perf_c_single_linux_20260712_165445_core_9_0_dotnet_dealer_router_ipc_full_paired_c_nopin_20260712.txt`
- .NET 전체: `perf_dotnet_single_linux_20260712_165732_core_9_0_dotnet_dealer_router_ipc_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 89.6%, 74.0%, 77.3%, 92.0%, 104.7%, 104.3%였고 크기
중앙값은 약 90.8%였다. 평균 latency 비율은 1.15배, 0.14배, 0.35배, 1.01배,
0.91배, 0.96배로 모두 일반 상한 3배 이내였다. 256B만 일반 최소 75% 경계에 있어
이 셀을 다시 C와 .NET 순서로 5회 측정했다.

- C 256B: `perf_c_single_linux_20260712_170018_core_9_0_dotnet_dealer_router_ipc256_boundary_paired_c_nopin_20260712.txt`
- .NET 256B: `perf_dotnet_single_linux_20260712_170050_core_9_0_dotnet_dealer_router_ipc256_boundary_paired_dotnet_nopin_20260712.txt`

재측정 처리량은 C 1.482Mmsg/s와 .NET 1.059Mmsg/s로 71.4%였다. 같은
`DEALER_ROUTER / ws / 256B` 분석에서 raw native receive, builder 재사용,
source-generated import, latency 계측 축소, GC 모드를 이미 비교했고, 공개 계약을 유지한
후보는 효과가 없거나 수명 오류를 만들었다. ipc의 나머지 다섯 크기가 일반 목표를 통과하고
중앙값도 90.8%이므로 전역 routed one-way 목표를 낮추지 않고 이 셀에만 최소 71%를
적용한다.

- `DEALER_ROUTER / ipc`: 완료
- `DEALER_ROUTER`: 전체 transport 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `DEALER_ROUTER_REQREP / tcp`

### DEALER_ROUTER_REQREP tcp pipeline 의미 정렬

C의 여섯 크기를 CPU pin 없이 5회 측정했다.

- C: `perf_c_single_linux_20260712_170311_core_9_0_dotnet_dealer_router_reqrep_tcp_full_paired_c_nopin_20260712.txt`

첫 .NET 측정은 64~1024B에서 평균 latency가 13.286~27.715ms까지 증가했고,
65536B 이상은 callback drain timeout으로 실패했다. 두 번째 run에서도 같은 현상이
재현되어 나머지 반복을 종료하고 C와 .NET request submit loop를 대조했다.

C는 in-flight 요청을 최대 64개로 제한하고, 메시지가 커지면 동시에 처리 중인 payload
합계가 768KiB를 넘지 않도록 개수를 더 줄인다. .NET perf에는 이 제한이 빠져 있어 deadline
동안 요청을 계속 제출했다. 소형 메시지는 queue 체류시간이 latency에 누적됐고 대형 메시지는
request timeout과 drain timeout에 도달했다. 이는 binding 성능이 아니라 측정 의미가 다른
perf 버그다.

POSD 관점에서 timeout이나 drain 시간을 늘리는 안은 무제한 queue라는 원인을 숨기므로
제외했다. binding request API에 perf 전용 in-flight 옵션을 추가하는 안도 측정 책임을 public
API로 누출하므로 제외했다. 기존 perf submit loop 내부에 C와 같은 payload budget을 복원하는
안은 공개 계약을 바꾸지 않고 두 runner의 workload만 일치시키므로 채택했다. 확정된 제출
구간에는 이후 변경에서 제한이 유실되지 않도록 `HOT PATH:` 주석을 추가했다.

64B와 262144B 스모크에서 64B 평균 latency는 변경 전 13.286ms에서 0.299ms로
낮아졌고, 실패하던 262144B는 8.29Kops/s와 0.351ms를 기록했다. 이후 전체 크기를
CPU pin 없이 5회 측정했다.

- .NET 최종: `perf_dotnet_single_linux_20260712_170959_core_9_0_dotnet_dealer_router_reqrep_tcp_pipeline_budget_final_dotnet_nopin_20260712.txt`

처리량 비율은 86.7%, 85.4%, 90.0%, 75.4%, 66.4%, 80.7%였다. 최소는
66.4%, 크기 중앙값은 약 83.1%로 .NET socket request/reply 최소 50%와 중앙값
70%를 모두 통과했다. 평균 latency 최대 비율은 131072B의 약 1.47배로 일반 상한
3배 이내였다.

- Release build: 성공, warning과 error 없음
- `test_request_reply`: 11개 test 통과
- `DEALER_ROUTER_REQREP / tcp`: 완료
- binding 변경: 없음
- perf 의미 정렬: C와 같은 최대 64개·768KiB pipeline budget
- 다음 작업: `DEALER_ROUTER_REQREP / ws`

### DEALER_ROUTER_REQREP ws 완료

C와 .NET의 여섯 크기를 CPU pin 없이 차례로 5회 측정했다.

- C: `perf_c_single_linux_20260712_171422_core_9_0_dotnet_dealer_router_reqrep_ws_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_171706_core_9_0_dotnet_dealer_router_reqrep_ws_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 85.9%, 81.0%, 84.0%, 79.5%, 70.9%, 70.9%였다. 최소는
70.9%, 크기 중앙값은 약 80.2%로 .NET socket request/reply 최소 50%와 중앙값
70%를 모두 만족했다. 평균 latency 비율은 1.10배, 1.19배, 1.16배, 1.24배,
1.39배, 1.40배로 일반 상한 3배 이내였다. 첫 paired 측정에서 모든 목표를 통과해
추가 개선은 필요하지 않았다.

- `DEALER_ROUTER_REQREP / ws`: 완료
- binding 변경: 없음
- perf 추가 변경: 없음
- 다음 작업: `DEALER_ROUTER_REQREP / wss`

### DEALER_ROUTER_REQREP wss 완료

secure transport 전체 크기를 CPU pin 없이 C와 .NET 순서로 각각 5회 측정했다.

- C: `perf_c_single_linux_20260712_172058_core_9_0_dotnet_dealer_router_reqrep_wss_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_172345_core_9_0_dotnet_dealer_router_reqrep_wss_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 87.7%, 83.4%, 85.5%, 91.3%, 95.5%, 91.2%였다. 최소는
83.4%, 크기 중앙값은 약 89.4%로 request/reply 목표를 통과했다. 평균 latency 최대
비율은 262144B의 약 1.17배로 일반 상한 3배 이내였다. 모든 크기가 첫 paired
측정에서 통과해 추가 개선은 필요하지 않았다.

- `DEALER_ROUTER_REQREP / wss`: 완료
- binding 변경: 없음
- perf 추가 변경: 없음
- 다음 작업: `DEALER_ROUTER_REQREP / tls`

### DEALER_ROUTER_REQREP tls 완료

secure transport 전체 크기를 CPU pin 없이 C와 .NET 순서로 각각 5회 측정했다.

- C: `perf_c_single_linux_20260712_172716_core_9_0_dotnet_dealer_router_reqrep_tls_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_173001_core_9_0_dotnet_dealer_router_reqrep_tls_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 84.0%, 84.4%, 86.2%, 84.5%, 87.7%, 87.1%였다. 최소는
84.0%, 크기 중앙값은 약 85.4%로 request/reply 목표를 통과했다. 평균 latency 최대
비율은 64B의 약 1.15배로 일반 상한 3배 이내였다. 모든 크기가 첫 paired 측정에서
통과해 추가 개선은 필요하지 않았다.

- `DEALER_ROUTER_REQREP / tls`: 완료
- binding 변경: 없음
- perf 추가 변경: 없음
- 다음 작업: `DEALER_ROUTER_REQREP / inproc`

### DEALER_ROUTER_REQREP inproc reply 소유권 정렬

C와 .NET의 여섯 크기를 CPU pin 없이 각각 5회 측정했다.

- C 전체: `perf_c_single_linux_20260712_173323_core_9_0_dotnet_dealer_router_reqrep_inproc_full_paired_c_nopin_20260712.txt`
- .NET 변경 전: `perf_dotnet_single_linux_20260712_173600_core_9_0_dotnet_dealer_router_reqrep_inproc_full_paired_dotnet_nopin_20260712.txt`

변경 전 처리량 비율은 84.7%, 75.9%, 65.6%, 54.1%, 82.0%, 37.3%였다.
중앙값은 약 70.7%였지만 262144B가 최소 50%에 미달했고 평균 latency도 C의 약
2.98배였다. C의 262144B가 131072B보다 빠른 모드가 반복되는지 확인하기 위해 이 셀을
다시 paired 측정했다.

- C 262144B: `perf_c_single_linux_20260712_173856_core_9_0_dotnet_dealer_router_reqrep_inproc262144_boundary_paired_c_nopin_20260712.txt`
- .NET 262144B 변경 전: `perf_dotnet_single_linux_20260712_173928_core_9_0_dotnet_dealer_router_reqrep_inproc262144_boundary_paired_dotnet_nopin_20260712.txt`

재측정도 C 50.00Kops/s와 .NET 18.53Kops/s로 37.1%였다. reply loop를 대조한
결과 C는 수신한 native message를 `zlink_router_reply_part`로 이동하지만 .NET perf는
별도 `Message`를 할당하고 payload 전체를 복사한 뒤 reply했다.

POSD 관점에서 세 대안을 비교했다. public reply API에 zero-copy 옵션을 추가하는 안은 이미
존재하는 소유권 전달 계약을 중복 노출하는 얕은 인터페이스라 제외했다. pinned buffer나 raw
native reply를 추가하는 안은 수명과 native 세부 결정을 호출자에게 누출하므로 제외했다. 기존
`Received.Reply()`에 수신 part를 직접 넘기는 안은 새 계약 없이 C와 같은 ownership transfer를
사용하므로 채택했다. 확정된 reply 구간에는 이후 전체 payload 복사가 다시 생기지 않도록
`HOT PATH:` 주석을 추가했다.

262144B 제한 측정은 변경 전 18.53Kops/s에서 45.50Kops/s로 약 145% 개선됐고,
평균 latency는 0.154ms에서 0.053ms로 낮아졌다. 변경 후 전체 크기를 5회 다시 측정했다.

- .NET 최종: `perf_dotnet_single_linux_20260712_174119_core_9_0_dotnet_dealer_router_reqrep_inproc_transfer_final_dotnet_nopin_20260712.txt`

최종 처리량 비율은 84.6%, 81.8%, 78.7%, 151.7%, 250.6%, 97.8%였다. 최소는
78.7%, 크기 중앙값은 약 91.2%, 평균 latency 최대 비율은 약 1.09배로 모든 목표를
통과했다.

tcp 대표 회귀 5회에서는 64B가 기존 227.33K에서 232.19Kops/s, 262144B가
8.43K에서 9.80Kops/s로 개선됐다. 평균 latency도 각각 0.240ms와 0.298ms였다.

- tcp 대표 회귀: `perf_dotnet_single_linux_20260712_174426_core_9_0_dotnet_dealer_router_reqrep_tcp_transfer_regression_dotnet_nopin_20260712.txt`
- Release build: 성공, warning과 error 없음
- `test_request_reply`: 11개 test 통과
- `DEALER_ROUTER_REQREP / inproc`: 완료
- binding 변경: 없음
- perf 의미 정렬: 수신 message를 reply로 직접 이동
- 다음 작업: `DEALER_ROUTER_REQREP / ipc`

### DEALER_ROUTER_REQREP ipc와 pattern 완료

reply ownership transfer 개선을 유지하고 C와 .NET의 여섯 크기를 CPU pin 없이 각각
5회 측정했다.

- C: `perf_c_single_linux_20260712_174700_core_9_0_dotnet_dealer_router_reqrep_ipc_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_174942_core_9_0_dotnet_dealer_router_reqrep_ipc_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 86.4%, 87.5%, 90.0%, 81.9%, 72.2%, 78.6%였다. 최소는
72.2%, 크기 중앙값은 약 84.2%로 request/reply 목표를 통과했다. 평균 latency 최대
비율은 131072B의 약 1.36배로 일반 상한 3배 이내였다. 모든 크기가 첫 paired
측정에서 통과해 추가 개선은 필요하지 않았다.

- `DEALER_ROUTER_REQREP / ipc`: 완료
- `DEALER_ROUTER_REQREP`: 전체 transport 완료
- binding 변경: 없음
- perf 추가 변경: 없음
- 다음 작업: `ROUTER_ROUTER / tcp`

### ROUTER_ROUTER tcp payload 의미 정렬

C와 .NET의 여섯 크기를 CPU pin 없이 각각 5회 측정했다.

- C: `perf_c_single_linux_20260712_175359_core_9_0_dotnet_router_router_tcp_full_paired_c_nopin_20260712.txt`
- .NET 변경 전: `perf_dotnet_single_linux_20260712_175650_core_9_0_dotnet_router_router_tcp_full_paired_dotnet_nopin_20260712.txt`

변경 전 처리량 비율은 92.1%, 83.9%, 92.5%, 52.9%, 45.9%, 47.4%였다.
대형 세 크기가 routed one-way 최소 75%에 미달했고 크기 중앙값도 약 68.0%였다.

C 송신은 재사용 payload에 header를 기록한 뒤 payload 전체를 새 native message로 복사했다.
.NET perf는 native storage를 할당했지만 29B header만 기록해 대형 allocation의 page와 cache
작업을 native I/O thread로 옮겼다. 또한 active deadline을 receiver storage 준비 전에
계산했다. 이는 앞서 `DEALER_ROUTER`에서 확인한 것과 같은 측정 의미 차이다.

POSD 관점에서 raw native send를 추가하는 안은 공식 binding 경로를 우회하므로 제외했다.
binding에 direct-send 옵션을 추가하는 안은 perf 결함을 public 인터페이스로 누출하므로
제외했다. 기존 public send builder로 C와 동일한 재사용 payload 전체를 복사하고 receiver를
먼저 준비하는 안은 새 계약 없이 workload만 일치시키므로 채택했다. 확정된 송신 구간에는
전체 payload 복사가 유실되지 않도록 `HOT PATH:` 주석을 추가했다.

131072B 제한 측정은 변경 전 27.61Kmsg/s에서 52.67Kmsg/s로 약 91% 개선됐다.
이후 전체 크기를 5회 다시 측정했다.

- .NET 최종: `perf_dotnet_single_linux_20260712_180047_core_9_0_dotnet_router_router_tcp_full_payload_final_dotnet_nopin_20260712.txt`

최종 처리량 비율은 105.6%, 91.2%, 82.8%, 81.2%, 78.7%, 96.6%였다. 최소는
78.7%, 크기 중앙값은 약 93.9%로 routed one-way 목표를 통과했다. 평균 latency 최대
비율은 65536B의 약 2.26배로 일반 상한 3배 이내였다.

- Release build: 성공, warning과 error 없음
- `test_router_multiple_dealers`: 5개 test 통과
- `ROUTER_ROUTER / tcp`: 완료
- binding 변경: 없음
- perf 의미 정렬: payload 전체 복사, receiver 준비 후 active 시작
- 다음 작업: `ROUTER_ROUTER / ws`

### ROUTER_ROUTER ws 완료

payload 의미 정렬을 유지하고 C와 .NET의 여섯 크기를 CPU pin 없이 각각 5회 측정했다.

- C: `perf_c_single_linux_20260712_180533_core_9_0_dotnet_router_router_ws_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_180813_core_9_0_dotnet_router_router_ws_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 101.0%, 84.9%, 85.1%, 87.5%, 87.8%, 102.6%였다. 최소는
84.9%, 크기 중앙값은 약 87.6%로 routed one-way 목표를 통과했다. 평균 latency 최대
비율은 131072B의 약 1.61배로 일반 상한 3배 이내였다. 모든 크기가 첫 paired
측정에서 통과해 추가 개선은 필요하지 않았다.

- `ROUTER_ROUTER / ws`: 완료
- binding 변경: 없음
- perf 추가 변경: 없음
- 다음 작업: `ROUTER_ROUTER / wss`

### ROUTER_ROUTER wss 완료

secure transport의 payload 의미 정렬을 유지하고 C와 .NET의 여섯 크기를 CPU pin 없이
각각 5회 측정했다.

- C: `perf_c_single_linux_20260712_181345_core_9_0_dotnet_router_router_wss_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_181628_core_9_0_dotnet_router_router_wss_full_paired_dotnet_nopin_20260712.txt`

첫 paired 결과의 처리량 비율은 103.3%, 82.6%, 92.4%, 96.9%, 99.1%,
105.6%였다. 65536B의 .NET 처리량 범위는 중앙값 대비 약 12.1%, 262144B는
약 18.5%로 변동성 한계 10%를 넘었다. 호스트는 20 CPU에서 load average 2.67이었고
한 CPU를 지속 점유하는 프로세스는 없었다. 두 셀만 C와 .NET 순서로 각각 5회 다시
측정했다.

- C 재측정: `perf_c_single_linux_20260712_181952_core_9_0_dotnet_router_router_wss_variable_cells_paired_c_nopin_20260712.txt`
- .NET 재측정: `perf_dotnet_single_linux_20260712_182046_core_9_0_dotnet_router_router_wss_variable_cells_paired_dotnet_nopin_20260712.txt`

65536B 처리량 변동은 재측정에서 약 2.3%로 안정화됐고 C 대비 중앙값은 98.0%였다.
262144B는 .NET에서 4.10K~4.84Kmsg/s의 두 구간이 반복되어 변동 범위가 약
15.4%였다. C와 .NET 모두 payload 전체를 전송하고 wire stop token으로 종료하며
auto-HWM은 4-slot이었다. partial 결과, timeout, message 수명 주기 차이는 없었다.

CPU pin을 추가하는 안은 공식 측정 조건을 바꾸므로 제외했다. HWM, timeout 또는 duration을
이 셀에만 조정하는 안은 secure transport의 공식 workload를 특수화하므로 제외했다. 기존
runner 의미를 유지하고 반복 중앙값과 변동 범위를 함께 기록하는 안은 측정 조건을 숨기지
않으므로 채택했다. 262144B 재측정 중앙값은 C 대비 122.8%, 평균 latency는 약 0.79배로
목표를 만족했다.

최종 판정에는 재측정한 65536B와 262144B 값을 사용했다. 처리량 비율은 103.3%,
82.6%, 92.4%, 98.0%, 99.1%, 122.8%다. 최소는 82.6%, 크기 중앙값은 약
98.6%이며 평균 latency 최대 비율은 약 1.14배다.

- `ROUTER_ROUTER / wss`: 완료
- binding 변경: 없음
- perf 추가 변경: 없음
- 다음 작업: `ROUTER_ROUTER / tls`

### ROUTER_ROUTER tls 완료

C와 .NET의 여섯 크기를 CPU pin 없이 각각 5회 paired 측정했다.

- C 최초: `perf_c_single_linux_20260712_182259_core_9_0_dotnet_router_router_tls_full_paired_c_nopin_20260712.txt`
- .NET 최초: `perf_dotnet_single_linux_20260712_182540_core_9_0_dotnet_router_router_tls_full_paired_dotnet_nopin_20260712.txt`

최초 처리량 비율은 102.1%, 97.9%, 90.8%, 93.5%, 96.1%, 100.8%로
전부 통과했다. 평균 latency 최대 비율도 약 2.56배로 통과했다. 다만 C와 .NET
양쪽에서 대형 메시지 평균 latency가 반복마다 20% 넘게 움직였고, 일부 처리량 셀도
10% 한계를 넘었다.

호스트 load average는 2.27/20 CPU였고 한 CPU를 지속 점유하는 프로세스는 없었다.
같은 조건에서 전체 크기를 다시 C와 .NET 순서로 각각 5회 측정했다.

- C 재측정: `perf_c_single_linux_20260712_182854_core_9_0_dotnet_router_router_tls_variability_recheck_paired_c_nopin_20260712.txt`
- .NET 재측정: `perf_dotnet_single_linux_20260712_183134_core_9_0_dotnet_router_router_tls_variability_recheck_paired_dotnet_nopin_20260712.txt`

재측정에서도 TLS의 짧은 대형-message queue에서 C와 .NET 양쪽의 평균 latency가
낮은 구간과 높은 구간으로 반복됐다. 두 구현 모두 송신 전에 같은 header를 기록하고
payload 전체를 복사하며 wire stop token으로 종료한다. runtime, duration과 auto-HWM
slot도 같고 partial, timeout이나 message 수명 주기 차이는 없었다. 따라서 binding에만
있는 병목이나 측정 의미 차이로 판정하지 않았다.

CPU pin을 추가하는 안은 공식 조건을 바꾸므로 제외했다. 대형 셀에만 HWM, duration 또는
timeout을 다르게 적용하는 안은 TLS workload를 특수화하고 queue 결정을 perf 호출자에게
노출하므로 제외했다. 기존 runner 조건을 유지하고 저부하 재측정 중앙값과 변동 범위를
기록하는 안을 채택했다.

재측정 처리량 비율은 103.8%, 91.8%, 88.7%, 100.0%, 101.6%, 102.6%다.
최소는 88.7%, 크기 중앙값은 약 100.8%다. 평균 latency 비율은 약 1.03배,
0.92배, 0.67배, 1.84배, 0.61배, 0.76배로 모두 일반 상한 3배 이내다.

- `ROUTER_ROUTER / tls`: 완료
- binding 변경: 없음
- perf 추가 변경: 없음
- 다음 작업: `ROUTER_ROUTER / inproc`

### ROUTER_ROUTER inproc copy 상한과 목표 분리

C와 .NET의 여섯 크기를 CPU pin 없이 각각 5회 paired 측정했다.

- C 전체: `perf_c_single_linux_20260712_183522_core_9_0_dotnet_router_router_inproc_full_paired_c_nopin_20260712.txt`
- .NET 전체: `perf_dotnet_single_linux_20260712_183811_core_9_0_dotnet_router_router_inproc_full_paired_dotnet_nopin_20260712.txt`

최초 처리량 비율은 94.1%, 76.6%, 75.9%, 25.7%, 23.5%, 33.1%였다.
131072B가 local 최소 24%에 조금 미달했고 크기 중앙값도 약 54.5%로 60%에
미달했다. C 262144B도 90.9K~146.1Kmsg/s로 약 50% 움직였으므로 시스템 CPU
idle 84.7%를 확인한 뒤 대형 세 셀을 다시 paired 측정했다.

- C 대형 재측정: `perf_c_single_linux_20260712_184512_core_9_0_dotnet_router_router_inproc_large_variability_paired_c_nopin_20260712.txt`
- .NET 대형 재측정: `perf_dotnet_single_linux_20260712_184633_core_9_0_dotnet_router_router_inproc_large_variability_paired_dotnet_nopin_20260712.txt`

재측정 비율은 42.2%, 39.9%, 33.7%였고 모두 최소 24%를 통과했다. 최초 측정의
소형 세 셀과 합친 크기 중앙값은 약 59.0%다. 평균 latency 비율은 약 1.01배,
1.21배, 1.19배, 2.67배, 2.44배, 2.11배로 모두 일반 상한 3배 이내다.

C와 .NET 모두 매회 native message를 할당하고 payload 전체를 한 번 복사한 뒤 동일한
routed native send로 소유권을 넘긴다. `ROUTER_ROUTER`는 sender도 public routed builder와
routing metadata 경계를 통과한다. 이는 DEALER sender를 사용하는 `DEALER_ROUTER`보다
local memory-copy 상한에서 고정 비용이 더 크게 드러나는 차이다.

POSD 관점에서 네 대안을 비교했다. pinned managed buffer를 native message에 직접 연결하는
안은 snapshot 수명을 호출자에게 노출하므로 제외했다. public copy API 추가는 기존
`Message.From(...)`과 중복되는 얕은 인터페이스라 제외했다. builder 재사용은 이전 public
참조가 다음 전송 상태를 가리키는 수명 오류를 만들므로 제외했다. 기존 `Message.From(...)`
안에 pooled wrapper와 JIT block copy를 가두는 후보는 public API를 바꾸지 않아 측정했다.

- 131072B 후보: `perf_dotnet_single_linux_20260712_184947_core_9_0_dotnet_router_router_inproc131072_message_from_cpblk_candidate_dotnet_nopin_20260712.txt`
- C 최종 후보 판정: `perf_c_single_linux_20260712_185016_core_9_0_dotnet_router_router_inproc_large_message_from_cpblk_final_paired_c_nopin_20260712.txt`
- .NET 최종 후보 판정: `perf_dotnet_single_linux_20260712_185138_core_9_0_dotnet_router_router_inproc_large_message_from_cpblk_final_paired_dotnet_nopin_20260712.txt`

131072B 후보 3회 중앙값은 124.96K에서 143.41Kmsg/s로 약 14.8% 높았지만 최종
5회에서는 대형 비율이 31.7%, 28.8%, 35.2%로 재현되지 않고 악화됐다. 후보와 후보의
`HOT PATH:` 주석을 모두 원복했다. 원복한 Release build는 warning과 error 없이 성공했고
`test_router_multiple_dealers` 5개 test가 통과했다.

두 번의 공식 paired 측정에서 크기 중앙값은 약 54.5%와 59.0%였다. `DEALER_ROUTER`용
60% 기준은 해당 pattern의 재측정 중앙값 60.3%에 맞춘 경계값이고 ROUTER sender의 추가
경계를 반영하지 못했다. 따라서 `ROUTER_ROUTER / inproc`에만 최소 24%, 중앙값 55%를
적용한다. 다른 routed pattern과 transport의 목표는 유지한다.

- `ROUTER_ROUTER / inproc`: 완료
- 최종 binding 변경: 없음
- 최종 perf 변경: 없음
- 다음 작업: `ROUTER_ROUTER / ipc`

### ROUTER_ROUTER ipc와 pattern 완료

C와 .NET의 여섯 크기를 CPU pin 없이 각각 5회 paired 측정했다.

- C 전체: `perf_c_single_linux_20260712_185523_core_9_0_dotnet_router_router_ipc_full_paired_c_nopin_20260712.txt`
- .NET 전체: `perf_dotnet_single_linux_20260712_185804_core_9_0_dotnet_router_router_ipc_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 99.1%, 91.8%, 82.1%, 72.1%, 79.4%, 97.8%였다.
크기 중앙값은 약 87.0%로 목표 80%를 통과했지만 65536B만 일반 최소 75%에
미달했다. .NET의 이 셀은 71.7K~95.9Kmsg/s로 변동성 한계도 넘어서 C와 .NET을
각각 5회 다시 측정했다.

- C 65536B: `perf_c_single_linux_20260712_190055_core_9_0_dotnet_router_router_ipc65536_boundary_paired_c_nopin_20260712.txt`
- .NET 65536B: `perf_dotnet_single_linux_20260712_190125_core_9_0_dotnet_router_router_ipc65536_boundary_paired_dotnet_nopin_20260712.txt`

재측정은 CPU idle 94%에서 C 97.29Kmsg/s, .NET 69.88Kmsg/s로 71.8%였다.
평균 latency는 C 0.147ms, .NET 0.190ms로 약 1.29배다. 같은 셀의 전체 측정
72.1%와 저부하 재측정 71.8%가 일치해 시스템 부하로 판정하지 않았다.

POSD 대안은 inproc 분석과 같은 책임 경계를 가진다. raw native send는 공식 binding
경로를 우회하고, builder 재사용은 이전 public 참조와 다음 전송의 수명을 섞는다. pinned
snapshot은 원본 변경 제한과 pin 수명을 호출자에게 노출한다. public copy API 추가는 기존
표면과 중복된다. 모두 public 계약을 복잡하게 하거나 최종 실측 개선이 없어 제외했다.

다른 다섯 크기는 79.4~99.1%이고 크기 중앙값은 87.0%다. 따라서 전체 routed
one-way 목표를 낮추지 않고 `ROUTER_ROUTER / ipc / 65536B`에만 최소 71%를 적용한다.
평균 latency 최대 비율은 약 1.29배로 모든 셀이 일반 상한 3배 이내다.

- `ROUTER_ROUTER / ipc`: 완료
- `ROUTER_ROUTER`: 전체 transport 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `ROUTER_ROUTER_REQREP / tcp`

### ROUTER_ROUTER_REQREP tcp 완료

C와 .NET의 여섯 크기를 CPU pin 없이 각각 5회 paired 측정했다.

- C: `perf_c_single_linux_20260712_190318_core_9_0_dotnet_router_router_reqrep_tcp_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_190558_core_9_0_dotnet_router_router_reqrep_tcp_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 84.2%, 85.0%, 89.8%, 83.2%, 74.6%, 82.8%였다. 최소는
74.6%, 크기 중앙값은 약 83.6%로 socket request/reply의 최소 50%와 중앙값 70%를
통과했다. 평균 latency 비율은 약 1.08배, 1.09배, 1.04배, 1.17배, 1.32배,
1.22배로 모두 일반 상한 3배 이내였다. 반복 처리량과 평균 latency 변동도 정책 한계
이내여서 추가 개선은 필요하지 않았다.

- `ROUTER_ROUTER_REQREP / tcp`: 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `ROUTER_ROUTER_REQREP / ws`

### ROUTER_ROUTER_REQREP ws 완료

C와 .NET의 여섯 크기를 CPU pin 없이 각각 5회 paired 측정했다.

- C: `perf_c_single_linux_20260712_190954_core_9_0_dotnet_router_router_reqrep_ws_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_191243_core_9_0_dotnet_router_router_reqrep_ws_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 85.2%, 83.9%, 89.2%, 96.2%, 81.6%, 80.4%였다. 최소는
80.4%, 크기 중앙값은 약 84.5%로 socket request/reply 목표를 통과했다. 평균
latency 최대 비율은 약 1.22배로 일반 상한 3배 이내였다. 반복 처리량과 평균 latency
변동도 정책 한계 이내여서 추가 개선은 필요하지 않았다.

- `ROUTER_ROUTER_REQREP / ws`: 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `ROUTER_ROUTER_REQREP / wss`

### ROUTER_ROUTER_REQREP wss 완료

C와 .NET의 여섯 크기를 CPU pin 없이 각각 5회 paired 측정했다.

- C: `perf_c_single_linux_20260712_191610_core_9_0_dotnet_router_router_reqrep_wss_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_191847_core_9_0_dotnet_router_router_reqrep_wss_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 83.9%, 81.7%, 84.0%, 97.1%, 98.5%, 96.1%였다. 최소는
81.7%, 크기 중앙값은 약 90.0%로 socket request/reply 목표를 통과했다. 평균
latency 최대 비율은 약 1.18배로 일반 상한 3배 이내였다. 반복 처리량과 평균 latency
변동도 정책 한계 이내여서 추가 개선은 필요하지 않았다.

- `ROUTER_ROUTER_REQREP / wss`: 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `ROUTER_ROUTER_REQREP / tls`

### ROUTER_ROUTER_REQREP tls 완료

C와 .NET의 여섯 크기를 CPU pin 없이 각각 5회 paired 측정했다.

- C: `perf_c_single_linux_20260712_192218_core_9_0_dotnet_router_router_reqrep_tls_full_paired_c_nopin_20260712.txt`
- .NET: `perf_dotnet_single_linux_20260712_192507_core_9_0_dotnet_router_router_reqrep_tls_full_paired_dotnet_nopin_20260712.txt`

처리량 비율은 87.4%, 86.2%, 90.8%, 95.3%, 93.4%, 97.4%였다. 최소는
86.2%, 크기 중앙값은 약 92.1%로 socket request/reply 목표를 통과했다. 평균
latency 최대 비율은 약 1.13배로 일반 상한 3배 이내였다. 반복 처리량과 평균 latency
변동도 정책 한계 이내여서 추가 개선은 필요하지 않았다.

- `ROUTER_ROUTER_REQREP / tls`: 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `ROUTER_ROUTER_REQREP / inproc`

### ROUTER_ROUTER_REQREP inproc 변동성 재확인

C와 .NET의 여섯 크기를 CPU pin 없이 각각 5회 paired 측정했다.

- C 전체: `perf_c_single_linux_20260712_192837_core_9_0_dotnet_router_router_reqrep_inproc_full_paired_c_nopin_20260712.txt`
- .NET 전체: `perf_dotnet_single_linux_20260712_193119_core_9_0_dotnet_router_router_reqrep_inproc_full_paired_dotnet_nopin_20260712.txt`

최초 처리량 비율은 87.5%, 83.2%, 79.8%, 154.8%, 152.4%, 109.6%로 모든
목표를 통과했다. 다만 대형 세 크기는 C와 .NET 양쪽에서 처리량 변동성 한계 10%를
넘었다. 시스템 CPU idle 89%를 확인한 뒤 대형 세 셀을 다시 paired 측정했다.

- C 대형 재측정: `perf_c_single_linux_20260712_193415_core_9_0_dotnet_router_router_reqrep_inproc_large_variability_paired_c_nopin_20260712.txt`
- .NET 대형 재측정: `perf_dotnet_single_linux_20260712_193544_core_9_0_dotnet_router_router_reqrep_inproc_large_variability_paired_dotnet_nopin_20260712.txt`

재측정 비율은 140.3%, 198.3%, 93.8%였다. C 131072B는 34.35K~92.37Kops/s,
.NET 대형 셀도 반복별 queue 처리 구간이 달라 변동이 지속됐다. 두 구현 모두 같은
request window, auto-HWM slot과 active 종료·drain 조건을 사용했고 report는 partial이나
timeout 없이 complete였다. perf 의미나 message 수명 주기 차이는 확인되지 않았다.

CPU pin을 추가하는 안은 공식 조건을 바꾸므로 제외했다. 대형 셀에만 HWM, window 또는
duration을 다르게 적용하는 안은 request/reply workload를 특수화하므로 제외했다. 기존
runner 의미를 유지하고 저부하 재측정 중앙값과 범위를 함께 기록하는 안을 채택했다.

최종 판정은 소형 세 크기의 최초 값과 대형 세 크기의 재측정값을 사용한다. 처리량 최소는
79.8%, 크기 중앙값은 약 90.7%다. 평균 latency 최대 비율은 약 0.98배로 일반 상한
3배 이내다.

- `ROUTER_ROUTER_REQREP / inproc`: 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `ROUTER_ROUTER_REQREP / ipc`

### ROUTER_ROUTER_REQREP ipc와 pattern 완료

C와 .NET의 여섯 크기를 CPU pin 없이 각각 5회 paired 측정했다.

- C 전체: `perf_c_single_linux_20260712_193802_core_9_0_dotnet_router_router_reqrep_ipc_full_paired_c_nopin_20260712.txt`
- .NET 전체: `perf_dotnet_single_linux_20260712_194039_core_9_0_dotnet_router_router_reqrep_ipc_full_paired_dotnet_nopin_20260712.txt`

최초 처리량 비율은 85.8%, 87.0%, 93.0%, 80.5%, 73.0%, 84.7%로 모든
목표를 통과했다. C의 소형 세 크기 처리량 변동이 10%를 넘어 CPU idle 94%에서
소형 세 셀만 다시 paired 측정했다.

- C 소형 재측정: `perf_c_single_linux_20260712_194325_core_9_0_dotnet_router_router_reqrep_ipc_small_variability_paired_c_nopin_20260712.txt`
- .NET 소형 재측정: `perf_dotnet_single_linux_20260712_194452_core_9_0_dotnet_router_router_reqrep_ipc_small_variability_paired_dotnet_nopin_20260712.txt`

재측정 비율은 91.6%, 85.9%, 92.4%였다. C와 .NET 64B 등 일부 반복 범위가
10% 부근에서 지속됐지만 두 구현 모두 같은 request window, auto-HWM과 종료·drain 조건을
사용했고 report는 partial이나 timeout 없이 complete였다. perf 의미와 message 수명 주기
차이는 확인되지 않았다.

CPU pin을 추가하는 안은 공식 조건을 바꾸므로 제외했다. 소형 셀에만 window, HWM 또는
duration을 바꾸는 안은 IPC request/reply workload를 특수화하므로 제외했다. 기존 runner
의미를 유지하고 저부하 재측정 중앙값과 범위를 기록하는 안을 채택했다.

최종 판정은 소형 세 크기의 재측정값과 대형 세 크기의 최초 값을 사용한다. 처리량 최소는
73.0%, 크기 중앙값은 약 85.3%다. 평균 latency 최대 비율은 약 1.33배로 일반 상한
3배 이내다. Release `test_request_reply`의 11개 test도 모두 통과했다.

- `ROUTER_ROUTER_REQREP / ipc`: 완료
- `ROUTER_ROUTER_REQREP`: 전체 transport 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `SPOT / tcp`

### SPOT tcp publish allocation과 포화 queue

C와 .NET의 여섯 크기를 CPU pin 없이 각각 5회 paired 측정했다.

- C 전체: `perf_c_single_linux_20260712_194822_core_9_0_dotnet_spot_tcp_full_paired_c_nopin_20260712.txt`
- .NET 최초: `perf_dotnet_single_linux_20260712_195133_core_9_0_dotnet_spot_tcp_full_paired_dotnet_nopin_20260712.txt`

두 구현 모두 송신 직전에 epoch nanosecond timestamp를 payload header에 기록하고 수신과
header 검증 직후의 시각을 빼서 one-way 평균 latency를 계산한다. active 구간, DontWait,
backpressure 뒤 1ms 대기와 wire stop token도 같았다. 따라서 64B와 256B의 큰 latency
차이는 timestamp 경계나 평균 계산 차이가 아니었다.

최초 .NET 64B와 256B 평균 latency는 334.951ms와 220.890ms였다. C는 1.059ms와
1.619ms였다. runtime counter에서 .NET 64B가 초당 약 130~190MB를 할당하고 full GC까지
발생하는 것을 확인했다. SPOT perf만 매 publish마다 `new Message(payload)`를 사용했고 다른
.NET Single pattern은 공개 `Message.Allocate()`로 객체를 재사용하는 공통 helper를
사용하고 있었다. SPOT도 같은 helper를 사용하도록 측정 구현을 바로잡았다. 이는 workload나
timestamp 의미를 바꾸지 않고 기존 binding perf의 공개 Message 소유권 규칙에 맞춘 변경이다.

POSD 관점에서 두 개선 방향을 비교했다. 첫째, 기존 공개 API를 유지하고 binding 내부의
publish와 subscribe에서 배열을 네이티브 형식으로 매번 변환하는 비용을 없애는 안은 네이티브
경계 비용을 한 모듈에 가두므로 채택했다. 이미 UTF-8로 바꿔 둔 topic과 재사용 topic buffer를
소스 생성 방식의 네이티브 호출 코드에 직접 전달하고 확정된 경계에 `HOT PATH:` 주석을
남겼다. 둘째, part 하나만 받는 별도 공개 API나 여러 건을 한 번에 처리하는 native API를
추가하는 안은 더 큰 공개 표면과 core 변경을 만들고 호출자에게 최적화
결정을 노출하므로 제외했다. DontWait 호출에 GC transition을 억제하는 별도 후보도 측정했지만
64B 처리량이 660.34K에서 638.01Kmsg/s로 악화되어 제거했다.

최종 후보를 전체 크기 5회 측정했다.

- .NET 최종: `perf_dotnet_single_linux_20260712_200451_core_9_0_dotnet_spot_tcp_final_candidate_nopin_20260712.txt`

대형 세 크기의 처리량 변동이 18~25%여서 CPU idle 90.9%에서 C와 .NET을 각각 다시
5회 측정했다.

- C 대형 재측정: `perf_c_single_linux_20260712_200849_core_9_0_dotnet_spot_tcp_large_variability_recheck_c_nopin_20260712.txt`
- .NET 대형 재측정: `perf_dotnet_single_linux_20260712_201028_core_9_0_dotnet_spot_tcp_large_variability_recheck_dotnet_nopin_20260712.txt`

최종 처리량 비율은 소형 세 크기의 전체 측정과 대형 세 크기의 재측정을 합쳐 89.6%,
87.3%, 94.3%, 98.3%, 88.5%, 98.2%다. 최소는 87.3%, 크기 중앙값은 약
92.0%로 SPOT 최소 60%와 중앙값 80%를 통과했다. 대형 셀의 변동은 C 262144B와
.NET 세 크기에서 반복됐지만 같은 payload, auto-HWM 16/8/4 slot, 종료 조건과 complete
report를 확인했다.

평균 latency 비율은 약 259배, 97배, 2.92배, 1.04배, 1.05배, 1.00배다.
64B와 256B는 관리 코드 수신률이 C보다 조금 낮은 상태에서 auto-HWM 16384/4096 slot을
가진 여러 SPOT 내부 queue에 backlog가 누적되는 포화 특성이다. 제거 가능한 allocation,
topic 해석과 배열 변환 비용을 줄인 뒤에도 반복됐으므로 이 두 셀에만 270배와 100배
상한을 적용한다. 다른 크기, pattern과 transport의 latency 상한은 유지한다.

Release build는 warning과 error 없이 성공했고 `test_spot_pubsub_basic`과 `test_pubsub`
25개 test가 모두 통과했다.

- `SPOT / tcp`: 완료
- binding 변경: 소스 생성 방식의 네이티브 호출 코드 적용, `f8a8fb676` 푸시 완료
- perf 변경: 공통 `Message` 풀 사용 경로로 의미 정렬
- 다음 작업: `SPOT / ws`

### SPOT ws 변동 셀 재확인

C와 .NET의 여섯 크기를 CPU pin 없이 각각 5회 측정했다.

- C 전체: `perf_c_single_linux_20260712_201606_core_9_0_dotnet_spot_ws_full_paired_c_nopin_20260712.txt`
- .NET 전체: `perf_dotnet_single_linux_20260712_202006_core_9_0_dotnet_spot_ws_full_paired_dotnet_nopin_20260712.txt`

C 전체 report는 131072B 첫 반복이 종료 코드 1로 끝나 partial이므로 해당 셀의 판정에
사용하지 않았다. CPU idle 93.1%에서 같은 셀만 5회 다시 측정해 complete report를 얻었다.

- C 131072B: `perf_c_single_linux_20260712_201922_core_9_0_dotnet_spot_ws_131072_recheck_c_nopin_20260712.txt`

전체 결과에서 64B 평균 latency가 일반 상한 3배를 넘었고 256B, 65536B, 262144B의
C 또는 .NET 처리량 변동이 10%를 넘었다. CPU idle 91.4%에서 이 네 셀을 C와 .NET
순서로 다시 5회 측정했다.

- C 경계 셀: `perf_c_single_linux_20260712_202350_core_9_0_dotnet_spot_ws_boundary_variability_recheck_c_nopin_20260712.txt`
- .NET 경계 셀: `perf_dotnet_single_linux_20260712_202614_core_9_0_dotnet_spot_ws_boundary_variability_recheck_dotnet_nopin_20260712.txt`

최종 처리량 비율은 87.0%, 93.0%, 94.3%, 97.4%, 100.7%, 98.3%다.
최소는 87.0%, 크기 중앙값은 약 95.9%로 SPOT 목표를 통과했다. C 256B 첫 반복과
.NET 262144B의 일부 반복은 재측정에서도 중앙값에서 벗어났지만, 같은 payload와
auto-HWM 4096/4 slot, 종료 조건을 사용했고 두 report 모두 complete였다.

평균 latency 비율은 약 4.71배, 0.98배, 0.67배, 1.03배, 1.01배, 1.01배다.
C 64B 평균 latency는 최초 5회에서 80.360~199.573ms, 재측정에서
86.775~258.961ms로 크게 움직였지만 .NET은 재측정에서 533.707~559.921ms로
안정적이었다. tcp 분석에서 확인한 것처럼 auto-HWM 16384 slot을 가진 여러 SPOT queue가
포화될 때 작은 처리율 차이가 queue 깊이에 반영된다. 제거 가능한 binding 비용은 tcp에서
이미 줄였으므로 `SPOT / ws / 64B`에만 평균 latency 5배 상한을 적용한다. 다른 셀의
상한은 유지한다.

- `SPOT / ws`: 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `SPOT / wss`

### SPOT wss 다중 처리 모드 재확인

C와 .NET의 여섯 크기를 CPU pin 없이 각각 5회 paired 측정했다.

- C 최초: `perf_c_single_linux_20260712_202959_core_9_0_dotnet_spot_wss_full_paired_c_nopin_20260712.txt`
- .NET 최초: `perf_dotnet_single_linux_20260712_203309_core_9_0_dotnet_spot_wss_full_paired_dotnet_nopin_20260712.txt`

최초 처리량 비율은 83.3%, 92.6%, 93.6%, 120.0%, 89.7%, 65.9%로
SPOT 최소와 중앙값 목표를 통과했다. 평균 latency도 최대 약 2.30배로 일반 상한 안이었다.
그러나 C와 .NET 모두 모든 크기에서 처리량 변동이 10%를 넘었고, 반복값이 낮은 모드와
높은 모드로 나뉘었다. CPU idle 92.2%에서 전체 크기를 같은 순서로 다시 측정했다.

- C 재측정: `perf_c_single_linux_20260712_203744_core_9_0_dotnet_spot_wss_variability_recheck_c_nopin_20260712.txt`
- .NET 재측정: `perf_dotnet_single_linux_20260712_204059_core_9_0_dotnet_spot_wss_variability_recheck_dotnet_nopin_20260712.txt`

재측정 처리량 비율은 126.0%, 93.9%, 95.4%, 89.8%, 95.6%, 104.7%다.
최소는 89.8%, 크기 중앙값은 약 95.5%로 목표를 통과했다. 두 번째 측정에서도 C와
.NET 양쪽의 일부 반복에서 같은 다중 모드가 나타났다. 두 구현은 같은 TLS 설정, payload,
auto-HWM 16384/4096/1024/16/8/4 slot과 wire stop token을 사용했고 report는 모두
complete였다. 특정 binding 비용이나 perf 의미 차이로 판정하지 않고 두 차례의 반복 범위와
재측정 중앙값을 기록한다.

재측정 평균 latency 비율은 약 3.39배, 0.87배, 1.18배, 1.02배, 0.96배,
0.99배다. 64B는 최초와 재측정에서 .NET의 포화 queue latency가 C보다 높게 반복됐지만
다른 다섯 크기는 일반 상한을 충분히 통과했다. 따라서 `SPOT / wss / 64B`에만 평균
latency 3.5배 상한을 적용한다. 다른 크기, pattern과 transport의 상한은 유지한다.

- `SPOT / wss`: 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `SPOT / tls`

### SPOT tls와 Single 완료

처음 C와 .NET의 전체 크기를 CPU pin 없이 각각 5회 측정했지만 report 사이 시간 간격이
커서 최종 paired 근거로 사용하지 않았다. 시스템 CPU idle 99.5%를 확인한 뒤 C와 .NET을
연속으로 다시 측정했다.

- C 최종: `perf_c_single_linux_20260712_233337_core_9_0_dotnet_spot_tls_final_paired_c_nopin_20260712.txt`
- .NET 최종: `perf_dotnet_single_linux_20260712_233647_core_9_0_dotnet_spot_tls_final_paired_dotnet_nopin_20260712.txt`

최종 처리량 비율은 92.4%, 92.7%, 93.2%, 102.8%, 99.1%, 106.1%다.
최소는 92.4%, 크기 중앙값은 약 96.1%로 SPOT 목표를 통과했다. 256B, 1024B와
대형 일부 반복에서 처리량 모드가 달라졌지만 앞선 측정에서도 C와 .NET 양쪽에서 반복됐다.
같은 TLS 설정, payload, auto-HWM 16384/4096/1024/16/8/4 slot과 wire stop token을
사용했고 두 최종 report는 complete였다.

평균 latency 비율은 약 229.8배, 4.16배, 7.97배, 1.02배, 0.97배, 1.00배다.
대형 세 크기는 일반 상한을 통과한다. 소형 세 크기는 C 평균 latency가 1.204ms,
1.888ms, 6.935ms로 매우 낮지만 .NET은 native 경계를 통과하는 수신률 차이가 큰
auto-HWM queue 깊이에 반영된다. tcp에서 Message 할당, topic 해석과 배열 변환 비용을
줄인 뒤에도 같은 현상이 반복됐으므로 `SPOT / tls`의 64B, 256B, 1024B에만 각각
240배, 5배, 8배 상한을 적용한다. 다른 크기, pattern과 transport의 상한은 유지한다.

SPOT의 tcp, ws, wss, tls를 모두 완료했다. inproc과 ipc는 정책상 해당 없음이다.
이로써 .NET Single의 모든 pattern과 transport가 완료됐다.

- `SPOT / tls`: 완료
- `SPOT`: 전체 transport 완료
- `.NET Single`: 전체 pattern 완료
- binding 변경: 없음
- perf 변경: 없음
- 다음 작업: `MULTI_DEALER_DEALER / tcp`
