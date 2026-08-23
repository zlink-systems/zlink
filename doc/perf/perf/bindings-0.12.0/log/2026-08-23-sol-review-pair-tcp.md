# Sol review — C++ PAIR/tcp 2차 개선 패스

읽기 전용 검토를 완료했습니다. 저장소에는 아무것도 수정하지 않았습니다.

## 최종 판정

- C2는 계속 **no-go**입니다. `ETERM` 관측 시점과 오류 결과의 동등성을 코드만으로 증명할 수 없습니다.
- 65536B 저하는 현재 C++ binding pool 경계에서 발생한 cliff가 아닙니다. 현재 `use_large_message_pool=false`라 모든 크기가 `zlink_msg_init_size()` 경로를 사용합니다.
- borrow/large-message pool 하한을 128KiB 아래로 내리는 것은 ownership 관점에서는 구현 가능하지만, 성능 후보로는 **no-go**입니다.
- C1은 일반적인 `socket_t::close()` 경합에서는 안전하지만, 다른 스레드의 `socket_t` 소멸과 동시 실행되는 lifetime race가 남아 있습니다.
- C4에는 실제 계약 결함이 있습니다. native message를 `message_t`로 복구·materialize할 때 `_has_payload`를 갱신하지 않는 경로가 있습니다.
- C3의 `first_rid_native_cache` invariant는 현재 사용 경로에서는 유지되지만, 이제 presence flag에 전적으로 의존합니다.
- 90% aggregate를 회복할 근거가 있는 안전한 2차 성능 후보는 현재 확인되지 않았습니다. 먼저 64KiB release profile로 binding-only 비용인지 분리해야 합니다.

공식 결과는 [공식 paired log](/home/hep7hep7/project/zlink/doc/perf/perf/bindings-0.12.0/log/2026-08-23-cpp-pair-tcp-official.md:168)에 기록된 값과 일치합니다.

## 현재 hot path

### Send

PAIR builder 경로는 다음과 같습니다.

1. [`pair_socket_t::send()`](/home/hep7hep7/project/zlink/bindings/cpp/src/Runtime/Sockets/pair.cpp:17)
2. TLS `operation_state_t` pool에서 state 획득
3. `.message(msg)`에서 raw single-part는 caller message 포인터를 보관
4. [`submit_raw_send_state()`](/home/hep7hep7/project/zlink/bindings/cpp/src/Runtime/Messaging/operation_submit.hpp:28)
5. callback lifetime 확인, `socket_closed` 확인, `outbound_record_attempt_mutex` 획득
6. `zlink_send_part()` 호출
7. 성공 시 `mark_sent()`로 message를 consumed/invalid 상태로 변경
8. builder 소멸 시 state를 TLS pool로 반환

첫 번째 profile이 지적한 대로 64B에서는 C++ builder/state와 callback synchronization 비용이 중요합니다. C++ sender의 native send 호출 자체는 C 경로와 동일한 Core API입니다.

### Receive

PAIR receive는 [`recv_single_part_message()`](/home/hep7hep7/project/zlink/bindings/cpp/src/Runtime/Sockets/detail.hpp:85)로 들어갑니다.

- 새 기본 `message_t`는 valid한 empty native frame이며 `_has_payload=false`입니다.
- C4 fast path는 이 경우 guard 생성, `init()` 재확인, save/restore를 건너뛰고 직접 `zlink_recv_part()`를 호출합니다.
- 성공 후 empty frame 구분을 위해 `zlink_msg_size()`를 한 번 호출합니다.
- payload가 있던 caller-owned message는 기존 guard 경로에서 복구됩니다.

C4에서 `zlink_msg_size()`를 완전히 제거하는 것은 안전하지 않습니다. Core receive API가 수신 크기를 반환하지 않으므로, 이를 제거하고 `_has_payload=true`로 고정하면 empty frame에서도 매번 rollback 경로를 타게 됩니다.

## 65536B cliff 해석

현재 [`message.cpp`](/home/hep7hep7/project/zlink/bindings/cpp/src/Runtime/Messaging/message.cpp:100)는 다음 정책을 갖고 있습니다.

- 정책 범위: 128KiB–1MiB
- 실제 `use_large_message_pool`: `false`
- 따라서 공식 측정에서는 64KiB, 128KiB, 256KiB 모두 native `zlink_msg_init_size()`를 사용

즉 65536B는 현재 실행 바이너리에서 pool 진입 직전의 allocator cliff가 아닙니다. 64KiB에서 C++/C가 70.90%이고 131072B에서 83.22%, 262144B에서 88.64%로 회복되지만, 이 사실만으로 pool 원인을 입증할 수 없습니다.

64KiB에서 우선 확인해야 할 것은 다음입니다.

- C++ `message_t::from()`의 allocation/copy
- C++ receive wrapper와 `zlink_msg_size()`
- TCP/Core send·receive 및 poller
- page fault, cache miss, scheduler variation
- C++와 C의 timestamp/metric 처리 차이

기존 profile은 64B만 분석했으며, 남은 gap의 상당 부분은 binding call layer 밖에 위치했습니다. 따라서 64KiB에 같은 profile을 하지 않고 pool을 먼저 바꾸는 것은 근거가 약합니다.

## 2차 후보 순위

### 0. C6 — `_has_payload` native move invariant 복구

성능 후보라기보다 **선행 수정이 필요한 correctness 후보**입니다.

- 메커니즘: `zlink_msg_move()`로 native payload를 `message_t`에 넣는 모든 내부 helper가 `_has_payload`를 갱신하도록 중앙화합니다.
- 문제 위치: [`native_message_parts.hpp`](/home/hep7hep7/project/zlink/bindings/cpp/src/Runtime/Native/native_message_parts.hpp:79)
  - `restore_part_from_native()`
  - `restore_parts_from_native()`
  - `assign_parts_from_native()`
  - `take_parts_from_native()`
- 현재는 `message_t::init()`이 `_has_payload=false`로 만든 뒤 native payload를 move하지만 flag를 다시 설정하지 않습니다.
- 영향 범위: 모든 크기지만 정상 PAIR single-part 성공 경로보다는 multipart, send 실패 복구, request result materialization 경로입니다.
- 계약 영향: payload ownership은 유지하고, 추가 `zlink_msg_size()`는 비-hot 복구/materialization 경로에만 발생합니다.
- A/B:
  - partial multipart send 실패 후 모든 part의 `valid()`, `size()`, `is_empty()` 확인
  - 실패로 복구된 message를 다시 `recv()`에 넣고 기존 payload가 보존되는지 확인
  - request result와 `received_t` materialization 확인
  - contract suite 및 ASan/UBSan 실행
  - 이후 PAIR/tcp 6개 size paired 측정으로 정상 경로 회귀 확인

판정: **GO prerequisite**. 이 invariant를 고치기 전에는 C4 성능 결과를 안정적으로 신뢰하기 어렵습니다.

### 1. C7 — TLS operation-state pool container/reset micro-A/B

- 메커니즘: public builder와 `unique_ptr<operation_state_t>` 구조는 유지하고, TLS `vector<unique_ptr<...>>`를 고정 용량 8개 배열+count로 바꾸거나, raw single-send 전용 reset을 별도 측정합니다.
- 대상: 64B–1024B. 65536B 이상에는 효과가 거의 없을 것으로 예상됩니다.
- 계약/lifetime: public type/layout, builder lifetime, async strong callback ownership은 변경하지 않아야 합니다.
- 비용: 낮은 single-digit ns 수준으로 예상되며, TLS 배열 초기화 비용 때문에 오히려 악화될 수도 있습니다.
- A/B:
  - 현재 generic reset/vector와 한 가지 변형만 비교
  - release Core 0.12.0, C를 먼저 실행
  - 64, 256, 1024, 65536, 131072, 262144B, duration 5, runs 3
  - builder outliving socket, nested builder, address reuse 테스트 재실행
  - 64B에서 반복 개선이 없거나 65536B가 변하지 않으면 폐기

판정: **조건부 A/B 가능**. 65536B 문제를 해결할 후보는 아니며, aggregate 5.08%p를 회복할 가능성도 낮습니다.

### 2. C8 — 64KiB size-boundary allocation/storage differential

실제 소스 변경 전에 수행할 **진단 후보**입니다.

- 메커니즘: 현재 native allocation/copy, receive wrapper, Core send/receive를 분리해 64KiB에서 C++ 전용 비용을 확인합니다.
- 대상: 32KiB, 65535B, 65536B, 65537B, 128KiB, 256KiB.
- 계약/lifetime: 진단 단계에서는 변경 없음. binding-only stack이 확인될 때만 내부 후보를 설계합니다.
- 비용: 실행 자체는 official measurement에 포함하지 않는 별도 profile입니다.
- A/B:
  - C와 C++을 각각 `perf record -g`로 단독 측정
  - `cycles`, `instructions`, `cache-misses`, `page-faults`, context switch를 `perf stat`으로 비교
  - `message_t::from()`/`zlink_msg_init_size()`/`zlink_msg_size()`/close stack을 분리
  - binding-only 차이가 없으면 Core/transport 영역으로 분류하고 후보를 만들지 않음

판정: **이 진단 없이 65536B source optimization을 시작하면 안 됩니다.**

### 3. C9 — pool 하한을 64KiB로 확장

현재 [`message.cpp`](/home/hep7hep7/project/zlink/bindings/cpp/src/Runtime/Messaging/message.cpp:102)의 기존 pool을 64KiB부터 활성화하는 후보입니다.

- 메커니즘: native `zlink_msg_init_size()` 대신 pooled external buffer와 release callback 사용
- 대상: 정확히 65536B 및 128/256KiB 경계
- 계약: callback이 마지막 native reference 이후 buffer를 반환한다는 전제가 지켜지면 public ownership/ABI는 보존 가능
- 위험:
  - global mutex
  - exact-size linear search
  - release callback이 다른 thread에서 실행될 가능성
  - 8MiB 혼합 allocation cap
  - `zlink_msg_init_data()`와 callback lifecycle 비용
  - static pool destruction과 늦은 message destruction의 lifetime 위험
  - payload copy 자체는 제거하지 못함
- A/B 방법:
  - 현재 pool-off와 임시 pool-on/64KiB floor를 별도 binary로 비교
  - 동일 release Core, 동일 C baseline, C→C++ 순서
  - 64/256/1024/65536/131072/262144B throughput·latency뿐 아니라 page fault, allocation count, mutex contention 기록
  - 65536B만 개선되고 128/256KiB 또는 latency가 악화되면 즉시 폐기

판정: **계약상 가능할 수 있으나 성능상 no-go**입니다. 계획서도 C++ large-message pool 재도입과 pool A/B를 후보에서 제외하고 있습니다([plan](/home/hep7hep7/project/zlink/doc/perf/perf/bindings-0.12.0/bindings-library-performance-improvement-plan-core-0.12.0.ko.md:562)). 과거 64KiB floor A/B도 65536/131072B latency 개선을 보이지 않아 폐기되었습니다.

### 4. C10 — receive 성공 후 `zlink_msg_size()` 제거

- 메커니즘: C4 성공 경로의 마지막 native size query 제거
- 대상: 모든 receive size
- 계약 영향: empty frame과 payload frame을 구분할 수 없어 `_has_payload=true` 고정은 계약 위반입니다.
- 유일한 정확한 대안은 Core receive API 변경인데, 이번 범위 밖입니다.
- A/B는 실제 후보로 수행하지 말고, size query의 CPU 비용만 microbenchmark로 측정해야 합니다.

판정: **no-go**.

## C2 판정

C2는 코드상 동등성을 주장할 수 없습니다.

현재 builder submit은 다음 순서를 가집니다.

1. `socket_closed` 확인
2. `outbound_record_attempt_mutex` 획득
3. 두 번째 `socket_closed` 확인
4. native send

[`socket_t::close()`](/home/hep7hep7/project/zlink/bindings/cpp/src/Runtime/Sockets/socket.cpp:139)는 같은 mutex 안에서 closed flag를 세우고 native close를 수행합니다. 따라서 현재 gate는 단순 mutex가 아니라 “send attempt와 close의 lifecycle 선형화 지점”입니다.

gate를 제거하면 다음 순서가 가능해집니다.

```text
send: socket_closed == false 확인
close: close accepted, native socket 종료
send: zlink_send_part() 진입
```

이때 native 결과는 `ETERM`, `ESHUTDOWN`, 기타 invalid-handle 오류 또는 운 좋게 accepted send가 될 수 있습니다. 현재 builder 경로는 binding-side closed check에서 `invalid_state/EINVAL`을 반환하고, admission 경로는 별도로 `terminated/ETERM`을 반환합니다. 따라서 오류 종류와 관측 시점 모두 보존되지 않습니다.

Core도 close와 admitted API에 stricter lifecycle gate를 요구합니다([Core socket API](/home/hep7hep7/project/zlink/core/include/zlink/socket/api.h:186)). 결론은 **C2 no-go 유지**입니다.

## C1~C5 defect review

### C1

정상적인 동시 `close()`에는 문제가 없습니다. `share_callback_state()`가 필요한 async/admission 경로에서 strong reference를 유지하는 것도 올바릅니다.

다만 다음 race는 남습니다.

```text
T1: live_callback_state()가 weak expired=false를 확인하고 raw pointer 반환
T2: socket_t destructor가 closed 설정, gate 획득, _callbacks 해제
T1: raw callback pointer의 mutex/atomic 역참조
```

기존 `weak_ptr::lock()`은 callback state 자체를 submit 종료까지 살렸지만, 현재 raw pointer 경로는 그렇지 않습니다. `socket_t` 객체 자체를 동시 파괴하는 것은 native handle lifetime도 이미 깨지므로 일반적인 지원 계약 밖으로 볼 수 있지만, 코드 수준에서 lifetime-equivalent하지는 않습니다.

판정: **조건부 유지 가능**, 단 object destruction과 concurrent member use가 금지된다는 lifetime 전제가 필요합니다.

### C3

현재 reset semantics는 다음 면에서 안전합니다.

- retained `weak_ptr`는 ownership을 보유하지 않음
- socket 주소 재사용 시 `expired()`가 true이면 재bind
- `first_rid_native_cache`는 모든 읽기가 `has_first_rid_native_cache`를 통과
- `cache_first_rid_native()`는 cache write → presence flag set → optional reset 순서

따라서 현재 코드에서 stale routing ID가 관측되는 경로는 찾지 못했습니다.

그러나 cache bytes를 zero하지 않으므로:

- reset 후 256B routing ID가 TLS state에 남음
- invariant가 깨지는 향후 코드가 stale 값을 읽을 위험이 커짐
- cache 접근이 helper를 통하지 않으면 즉시 결함으로 이어짐

또한 generic `reset_for_reuse()`가 PAIR send마다 topic/target/reply/received까지 정리하므로, 기존 raw-send 특수 reset보다 실제 hot path가 항상 개선된다고 단정할 수 없습니다. C7 A/B로 분리 측정할 가치가 있습니다.

### C4

가장 중요한 결함입니다.

[`message_access_t::move_to_native()`](/home/hep7hep7/project/zlink/bindings/cpp/src/Runtime/Native/message_access.hpp:67)는 source `_has_payload=false`를 설정합니다. 이후 [`restore_part_from_native()`](/home/hep7hep7/project/zlink/bindings/cpp/src/Runtime/Native/native_message_parts.hpp:79)가 `init()`으로 empty message를 만든 뒤 native payload를 move하지만 `_has_payload=true`로 되돌리지 않습니다.

결과적으로 native payload는 존재하지만 binding metadata는 empty가 됩니다. 이후 C4 fast path가 이를 empty message로 오인해 receive 실패 시 기존 payload를 close할 수 있습니다.

root cause는 C4 이전 helper에도 있었지만, C4가 `_has_payload`를 branch selector로 사용하면서 위험이 더 커졌습니다. C6 수정이 필요합니다.

### C5

C harness에 없는 `tcp_no_delay`와 auto-HWM 재계산을 제거한 것은 측정 의미 정렬로 타당합니다. binding public API, ownership, lifetime defect는 확인되지 않았습니다. 단독 성능 후보가 아니라 harness parity 수정으로 분리하는 현재 분류가 맞습니다.

## 권고

1. `_has_payload` native move invariant를 먼저 고칩니다.
2. 64KiB release profile로 binding-only 원인을 확인합니다.
3. 그 결과가 없으면 C2와 pool을 사용해 수치를 억지로 끌어올리지 말고, 이번 PAIR/tcp 개선은 추가 후보 없음으로 보류하는 것이 타당합니다.
4. C7 state-pool/reset A/B는 64B 최적화용 저위험 실험으로만 진행할 수 있습니다.

기존 first-pass log의 테스트 결과는 변경 전과 동일한 12 pass/2 pre-existing fail, samples 6 pass/1 pre-existing fail이었으며, 이번 리뷰에서는 파일을 수정하거나 테스트를 재실행하지 않았습니다.
