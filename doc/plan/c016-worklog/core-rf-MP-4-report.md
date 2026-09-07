# Core MP-4 동시 multipart single-after-multipart 비용 제거 보고서

## 결과

MP-3의 caller별 multipart 조립 계약은 유지하면서, helper state가 한 번 생성된 socket의 single
`FINAL`에서 helper mutex와 TLS identity 조회를 생략했다. `send_sequences`의 크기를 C3 atomic
값으로 발행하고, 0을 관측한 single `FINAL`은 기존 단일 record 경로로 바로 간다. Caller identity는
API마다 `shared_ptr`을 복사하지 않고 TLS가 보유한 `shared_ptr`을 참조한다.

동일 runner의 single-after-multipart MP-4 두 표본 평균은 **23.573 ms**로 main 22.491 ms 대비
**+4.81%**여서 목표인 ±5% 안에 들어왔다. 첫 표본은 +6.25%였고 확인 표본은 +3.37%였으므로 두 값과
평균을 모두 아래 표에 남긴다. 4-thread 2-part는 MP-3보다 wall time이 7.15% 줄었다.

구현은 `/home/hep7hep7/project/zlink-work/mp2`의 MP-3 patch 위에 미커밋으로 남겼다. 작업 시작 전
MP-3 diff는 지정된 `scratchpad/mp3-before-mp4.patch`에 보존했다. 스펙, 공개 헤더,
`core/src/libzlink.vers`는 수정하지 않았다.

- 소유 계층: caller slot membership의 유일 소유자는 Core part helper의 mutex 아래
  `send_sequences` map이다. Atomic counter는 map 크기의 C3 발행값일 뿐 두 번째 registry가 아니다.
- 스펙 조항: `core/doc/spec/core/systems/11-synchronization-model.ko.md` §2 C3와 §8,
  `core/doc/spec/core/socket/README.ko.md`의 Part send와 pending admission 및 Request/reply를 적용했다.
- 교차언어 대조: C++, .NET, Node, Java, Rust, Go는 모두 multipart part를 Core의
  `zlink_send_part`/`zlink_send_part_rid`/`zlink_request_part`에 위임한다. Binding별 조립 상태는 없고
  Core 한 곳의 내부 비용만 바뀌므로 언어별 runtime 변경은 필요하지 않다.
- 변경 분류: **B(기존 결함)**. MP-3이 남긴 불필요한 hot-path 동기화 비용을 상태 소유 모듈에서
  제거했다.
- 규칙 수: 공개 동작 규칙은 MP-3의 3개에서 **3개로 동일**하다. Slot membership 소유자도 map
  하나에서 하나로 동일하며 counter는 §2가 허용한 “비어 있는가”의 발행 projection이다.

## 설계 비교와 선택

| 안 | 내용 | 판정 |
|---|---|---|
| A | mutex 아래 map 생성·삭제·회수와 함께 map 크기를 release-store하고, single `FINAL`은 relaxed zero만 negative filter로 사용 | 선택. 사실의 소유자는 map 하나이고 lock-free 읽기는 payload나 slot을 역참조하지 않는다. |
| B | TLS raw cache 또는 caller별 active map을 추가해 현재 caller slot을 직접 판정 | 기각. 같은 membership 사실을 두 곳이 소유해 close·만료·OOM마다 일치 규칙이 늘어난다. |
| C | MP-3처럼 API local strong identity를 계속 복사하고 helper mutex에서 매번 lookup | 기각. W06 수명은 TLS 소유권만으로 충족되며 호출마다 strong refcount atomic RMW 두 번과 mutex가 남는다. |

Counter write는 map 변경을 소유한 helper mutex 아래에만 있다. 생성, 현재 caller 삭제, 만료 caller
`extract`, close의 map `swap`이 각각 새 크기를 release-store한다. Single `FINAL`의 relaxed load는
다른 thread의 slot을 판정하는 데 쓰지 않는다. 현재 thread가 slot을 만들었다면 같은 thread가 mutex
아래 counter 증가를 먼저 실행했으며, 그 TLS identity는 정상 API 실행 중 만료되지 않는다. 따라서
0은 현재 thread에 열린 slot이 없다는 negative filter로 충분하고, 0이 아니면 기존 TLS 조회와 mutex
아래 map lookup으로 다시 판정한다. 이 근거는 fast path 옆 source comment에도 한 문장으로 남겼다.

TLS `shared_ptr` 자체가 thread 종료 전 identity의 strong owner다. API local은 그 객체를 가리키는
참조만 사용하므로 refcount를 증감하지 않아도 `weak_ptr::expired()` scan이 정상 함수 실행 중인 caller를
회수할 수 없다. Application TLS destructor가 Core identity의 TLS destructor 뒤에 Core API를 새로
호출하는 순서는 MP-3 W06 제안과 같이 지원 범위 밖으로 유지했다.

## Helper mutex 횟수 재확인

| 성공 경로 | helper mutex 획득 | 근거 |
|---|---:|---|
| 첫 `MORE` | 1회 | `prepare_send_step_locked`가 `unique_lock`을 얻고 같은 lock 안에서 slot 생성, counter 발행, part staging과 MORE 완료까지 수행한다. |
| 열린 sequence의 `FINAL` | 1회 | counter가 nonzero이면 같은 함수가 lock을 한 번 얻고 lookup, record 분리, slot 삭제와 counter 발행까지 수행한다. |
| 2-part SEND 합계 | **2회** | 첫 MORE 1회 + FINAL 1회. 성공 경로에는 helper mutex 재획득이 없다. |
| 열린 sequence가 없는 single `FINAL` | **0회** | counter zero에서 TLS identity 조회와 helper mutex를 모두 생략한다. |

여기서 센 것은 part helper의 `handle_state_t::mutex`이며 public API admission/socket turn은 기존 계약대로
별도 유지된다.

## 검증 결과

| 검증 | 범위 | 결과 |
|---|---|---|
| dev build | `JOBS=4 scripts/build-core.sh dev` | 성공 |
| 직접 영향 target | public multipart, helper ownership/interleave, phase3 request/reply, request/reply owners | **5/5 통과** |
| 관련 suite 반복 | `part|multipart|send|request|reply|router|dealer|pair|flow|hwm|close|wake`, `until-fail:3` | **80 target × 3 = 240/240 통과**, 413.12초 |
| ASan+LSan | MP-3의 6 target, `detect_leaks=1:halt_on_error=1:abort_on_error=1` | **6/6 통과**, sanitizer·leak 오류 0 |
| GCC TSan | MP-3의 신규·변경 8 target, 기존 `/tmp/mp2-tsan.supp`, `setarch x86_64 -R` | **8/8 통과**, 23.21초, 신규 race 0 |
| Release+LTO lib | `JOBS=4 scripts/build-core.sh release --lib-only` | 성공 |
| Release+LTO hotpath | 5셀 1회, `PERF_LOCK` 아래 foreground | **5/5 PASS** |
| diff 검사 | `git diff --check` | 통과 |
| 공개 API·ABI 검사 | `git diff --stat -- core/include core/src/libzlink.vers` | 출력 없음 |

ASan 6 target은 `test_public_inproc_multipart_send`, `test_helper_ownership`,
`test_helper_interleave`, `test_phase3_request_reply_contract`,
`unittest_complete_record_admission`, `unittest_phase3_request_reply_owners`다. TSan 8 target은 여기에
`test_single_lane_flow_control_boundary`와 `unittest_zmp_contract_edges`를 더한 집합이다.

## 성능

### Release+LTO hotpath 5셀

측정 시작 load average는 **3.67 / 1.38 / 0.69**였다. Release lib와 같은 source로 static
`hotpath_bench`도 갱신한 뒤 지정된 `PERF_LOCK` 아래 한 번 실행했다.

| cell | reference Ir/msg | MP-4 Ir/msg | 변화율 | 판정 |
|---|---:|---:|---:|---|
| `dealer_dealer_inproc` | 3230.922 | 3374.922 | +4.46% | PASS |
| `dealer_router_reqrep_inproc` | 18663.506 | 18345.395 | -1.70% | PASS |
| `pair_inproc` | 2348.457 | 2451.508 | +4.39% | PASS |
| `router_router_tcp` | 2972.532 | 3111.844 | +4.69% | PASS |
| `stream_tcp` | 14623.471 | 14228.411 | -2.70% | PASS |

### 동일 runner dev 셀

`/tmp/mp3_multipart_perf`의 동일 binary를 사용하고 `LD_LIBRARY_PATH`만 MP-4 dev lib로 지정했다.
Main과 MP-3 값은 같은 binary·방법으로 측정한 MP-3 보고서 값을 인용했다. MP-4 첫 측정 load
average는 **1.93 / 1.22 / 0.67**, single 확인 측정은 **1.38 / 1.14 / 0.65**였다.

| 셀 | main | MP-3 patch | MP-4 | MP-4 판정 |
|---|---:|---:|---:|---|
| 같은 PAIR socket에서 2-part 100회 후 single 100,000회 | 22.491 ms | 27.821 ms (**+23.70%**) | 23.897 / 23.248 ms, 평균 **23.573 ms (+4.81%)** | 목표 ±5% 이내. 첫 표본 +6.25%, 확인 표본 +3.37% |
| 4 thread × 2-part 20,000회(총 80,000 record) | 20초 timeout | 110.396 ms, 724,662 record/s | **102.501 ms, 780,483 record/s** | MP-3 대비 시간 -7.15%, 처리량 +7.70% |

## 변경 파일

MP-4가 MP-3 patch 위에 추가로 바꾼 파일은 네 개다.

- `core/src/api/socket/part_helper_internal.hpp`: map 크기의 C3 atomic counter
- `core/src/api/socket/part_helper_api.cpp`: counter 발행/zero fast path와 TLS-owned identity 참조
- `core/src/api/socket/part_helper_state.cpp`: close map 회수와 counter 0 발행
- `core/src/api/socket/socket_request_reply_submit_api.cpp`: single REQUEST의 counter-zero helper lookup 생략

새 public API, 옵션, 별도 map, TLS raw cache와 테스트 기대값 변경은 없다. Socket part send의 part
소비·sequence 폐기·FINAL admission, REQUEST/REPLY token, completion·READY/DISCONNECTED,
POLLIN/POLLOUT와 WRITABLE wake의 순서와 조건은 그대로다. 재확인한 스펙의 어느 문장도 다른 동작이
되지 않았다.

## 남은 실패와 위험

기능·sanitizer·hotpath gate의 남은 실패는 없다. Wall-time single 셀은 첫 표본이 main 대비
+6.25%였고 확인 표본과 두 표본 평균이 각각 +3.37%, +4.81%였으므로 표본 간 편차는 남는다.
이번 결과는 목표 범위 복귀를 확인하지만 두 번의 wall-time만으로 더 작은 개선 폭을 주장하지 않는다.
