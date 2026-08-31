# Sol read-only review

결론은 다음과 같습니다.

- 공개 builder 체인을 수동으로 합치거나 state를 stack/inline으로 바꾸는 것은 no-go입니다.
- LTO/IPO는 계약을 바꾸지 않고 TU 경계를 줄일 수 있으므로, 한 번의 통제된 A/B 후보로 남길 가치가 있습니다.
- 두 `weak_ptr::expired()` 검사는 서로 다른 lifetime 경계를 지키므로 제거·호이스팅하지 않습니다.
- 65536B 수치는 산술평균에서 제외하지 않고, 환경 지배 원인만 별도로 표시해야 합니다.
- 현재 3-run은 후보 판정 단계이므로, 최종 확정 전 §7.2의 5-run paired 측정을 한 번 실행하는 것이 계획 위반은 아닙니다.

## 1. 남은 최적화 후보

| 후보 | 판정 | 이유 |
|---|---|---|
| Binding library + 최종 consumer의 LTO/IPO | A/B 권고 | `publish()`·`message()`·`submit()`의 TU 경계를 실제로 줄일 수 있는 유일한 현실적 후보입니다. public API, ownership, error semantics는 유지됩니다. |
| Builder state를 stack/inline 또는 SBO로 변경 | No-go | `send_operation_t`와 후속 builder가 `unique_ptr<operation_state_t>`를 public type의 base 멤버로 보유합니다. 이 변경은 public type layout/lifetime 계약을 바꿉니다. [operation_builder_base.hpp](../../../../../bindings/cpp/include/zlink/Contracts/Messaging/operation_builder_base.hpp#L27) |
| 세 public call을 하나의 call로 병합 | No-go | 각 단계의 builder 보관·이동·소멸, message ownership, terminal error 시점이 public contract입니다. 내부 helper 병합은 P2처럼 중복만 줄일 뿐 call boundary를 제거하지 못합니다. |
| 두 `weak_ptr::expired()` 검사 호이스팅 | No-go | 첫 검사는 pooled state의 stale callback/socket 주소 재검증, 두 번째는 builder가 socket보다 오래 산 뒤 terminal을 호출하는 경우를 검증합니다. 한 검사로 합칠 수 없습니다. [operation_state.hpp](../../../../../bindings/cpp/src/Runtime/Messaging/operation_state.hpp#L131) |

LTO A/B는 `zlink_cpp`만 LTO로 빌드해서는 부족합니다. 현재 C++ CMake는 `ENABLE_LTO`를 실제 IPO 속성으로 연결하지 않고, runner의 `-DENABLE_LTO=OFF`도 binding target에는 효과가 없습니다. [bindings/cpp/CMakeLists.txt](../../../../../bindings/cpp/CMakeLists.txt#L222)와 최종 `cpp_perf_pubsub` executable 양쪽에 IPO를 적용해야 합니다.

LTO A/B를 수행한다면 같은 compiler, release Core, runner 조건으로 C reference도 다시 paired 측정해야 합니다. 개선이 없거나 회귀하면 LTO를 no-go로 기록하면 됩니다. private field reorder/hot-cold split은 이론상 public layout을 바꾸지 않지만, 현재 측정된 4.2ns pooled-state 비용과 call boundary를 직접 해결하지 않으므로 이번 공식 A/B 후보로는 권하지 않습니다.

## 2. 65536B 기록 방식

현재 pass2의 `70.18%`는 반드시 측정값으로 남겨야 합니다. `통과`, `해당 없음`, `환경 제외`로 바꾸거나 5-cell 평균으로 대체하면 안 됩니다.

최종 표에서는 다음 의미가 적절합니다.

- 65536B 셀: `보류(70.18%; 환경 지배)`  
- 행 설명: aggregate throughput `86.15%`는 목표 95%, 완화 90% 모두 미달
- aggregate latency `1.120배`는 통과
- 65536B는 C++ minor page fault가 C의 약 540배이고 allocator 조건에 따라 순위가 역전된 환경 지배 셀
- 단, `70.18%`는 여전히 6개 size aggregate에 포함
- 65536B 제외 시 `89.34%`는 진단용 수치일 뿐 판정 입력이 아님
- `MALLOC_*` 조건의 `94.31%`도 공식 결과가 아님

즉 “환경 원인이지 binding 결함의 증거는 아니다”와 “aggregate throughput 미달이다”를 동시에 기록해야 합니다. 계획서 §8의 정의상 두 개선 pass와 Sol 검토가 끝난 뒤의 최종 상태는 `미달`이 아니라 `보류`이며, `보류`는 통과를 의미하지 않습니다. [계획서 §8](../bindings-library-performance-improvement-plan-core-0.12.0.ko.md#8-판정과-기록-방법)

## 3. 5-run 최종·경계 판정

이번 경우에는 한 번 실행하는 것이 맞습니다.

현재 두 공식 라운드는 모두 §7.2의 `후보 판정` 단계인 `runs=3`입니다. 아직 `최종·경계 판정`의 기본 조건인 default duration, `runs=5`, CPU pin 없음이 실행되지 않았습니다. 따라서 이것은 “변동값이 커서 같은 셀을 반복”하는 것이 아니라, 아직 수행하지 않은 최종 판정 단계입니다. [계획서 §7.2](../bindings-library-performance-improvement-plan-core-0.12.0.ko.md#72-반복-횟수와-측정값-기록)

단, 순서는 다음과 같아야 합니다.

1. LTO A/B를 할지 결정하고, 한다면 먼저 후보 A/B를 수행합니다.
2. 최종 선택된 build에 대해 C와 C++를 같은 session으로 전체 6개 size, `runs=5`로 한 번 실행합니다.
3. 이후에는 65536B만 따로 재실행하거나 유리한 라운드를 선택하지 않습니다.

5-run 결과도 aggregate 평균으로 판정하고, 결과가 계속 미달하면서 추가 후보가 없으면 `보류`로 확정합니다. §7.5의 “변동값을 이유로 반복하지 않는다”는 규칙과 충돌하지 않습니다. [계획서 §7.5](../bindings-library-performance-improvement-plan-core-0.12.0.ko.md#75-pattern-완료와-언어-전환-gate)

## 4. P1/P2 defect review

### P1 — `perf_pubsub.cpp`

결함은 확인되지 않았습니다.

C reference에는 `recalculate_single_auto_hwm()` 호출이 없으므로 C++ PUBSUB runner에서 제거한 것은 측정 의미 정렬입니다. 기존 로그상 Auto-HWM detail도 전후 동일하고, 이 변경이 runtime HWM이나 large-size 변동의 원인은 아닙니다. [perf_pubsub.cpp](../../../../../bindings/cpp/perf/single/src/perf_pubsub.cpp#L82)

### P2 — `pubsub.cpp`

구조 개선 자체는 sound합니다.

- PUB/XPUB publish state 조립 중복 제거
- subscription error translation 중복 제거
- public signature/type, ownership, operation kind, 예외 타입과 errno 유지
- protected `send_operation_t` 생성은 각 friend member에서 수행
- 같은 TU의 helper이므로 정상 `-O3` build에서는 helper 자체가 hot-path call boundary로 남을 가능성이 낮음

다만 작은 contract-order caveat가 있습니다. 현재 `publish()`는 `make_publish_state()` 호출의 인자로 `callback_state()`를 먼저 평가할 수 있고, topic validation은 helper 본문에서 수행됩니다. [pubsub.cpp](../../../../../bindings/cpp/src/Runtime/Sockets/pubsub.cpp#L47) 반면 기존 코드는 validation이 먼저였습니다. `callback_state()`는 `_callbacks`가 없으면 lazy allocation을 수행합니다. [socket.cpp](../../../../../bindings/cpp/src/Runtime/Sockets/socket.cpp#L401)

정상 생성된 socket에서는 실질적 영향이 없지만, moved-from socket에서 embedded-null topic을 넘기면 validation 전에 callback state가 생성되거나 `bad_alloc`이 관측될 수 있습니다. 따라서 P2를 최종 채택할 때는 validation 순서를 보존하는 것이 좋습니다. 이는 성능 blocker는 아니지만, “error 동작이 완전히 동일하다”는 주장은 현재 구현 그대로는 약간 과합니다.

`throw_last_config_error()`의 `zlink_errno()` 2회 호출은 기존 코드와 동일하므로 P2가 새로 만든 결함으로 보지는 않습니다.

읽기 전용으로만 확인했으며 파일, branch, commit은 변경하지 않았습니다.
