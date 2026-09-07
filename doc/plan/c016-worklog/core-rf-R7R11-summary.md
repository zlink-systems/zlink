# core-rf-R7R11 apply job 결과

## 결과(수치)
- `ctest --test-dir core/build-dev -R 'option|close|xsub|sub|pub|send|part|flags|errno'` 5회, 매번 30/30 통과(19~22초).
- dev 빌드(`JOBS=4 scripts/build-core.sh dev`) 성공. 1차 빌드 시 R7 #6(capacity() 제거)이 `unittest_zmp_contract_edges.cpp:554`에서 실사용 중임을 빌드가 잡아 즉시 되돌림(2차 빌드로 통과) — 인벤토리의 `grep -rn ".capacity ()" core/src`가 `core/tests`를 빠뜨린 오탐.

## 변경 파일
- `core/src/api/core/zlink_option_internal.hpp`, `zlink_option_mapping.cpp`, `zlink_option.cpp` — R7 #1: `option_descriptor_t::unsupported_on_socket`(항상 false) 필드와 dead 분기 2곳 제거.
- `core/src/api/core/zlink.cpp` — R7 #3: `zlink_close`의 STREAM-mutex 유무 분기에서 복제된 drain/cleanup/complete tail을 `finish_close_after_drain()` 로 통합.
- `core/src/api/core/zlink_option_specialized_api.cpp` — R7 #5: pub/sub set/get 4개 함수의 `resolve_option_target`→`set/get_socket_option_checked` 반복을 `set/get_socket_option_by_target()` 2개 helper로 축약.
- `core/src/api/socket/socket_message_send_api.cpp` — R7 #2: 로컬 `validate_send_flags(int)`(ENOTSUP) 제거, `part_helper_internal::validate_send_flags`(EINVAL) 하나로 통합.
- `core/CMakeLists.txt`, `core/builds/cmake/platform.hpp.in`, `core/src/runtime/sockets/pubsub/xsub.{hpp,cpp}`, `core/tests/unittest/CMakeLists.txt` — R11 A: `ZLINK_USE_RADIX_TREE`(도달 불가) 분기·`radix_tree.{hpp,cpp}`·전용 unittest·CMake 소스/옵션 라인 제거.
- `core/src/api/socket/inline_msg_buffer_internal.hpp` — R7 #6은 되돌림(실사용 확인, 위 참고).

## 설계 비교와 선택 이유
- #3: 매크로 대신 파일-local 함수(`finish_close_after_drain`)로 추출 — 두 분기 모두 `socket_handle_t`와 `bool`만 필요해 클래스 멤버화 불필요, 반경 최소.
- #5: 함수 포인터 템플릿 대신 `expected_a/expected_b` 파라미터를 그대로 넘기는 얇은 helper 2개 — pub/sub는 이미 `set_socket_only_option`과 같은 패턴이지만 `resolve_option_target`(소켓 외 kind 처리)을 쓰므로 완전 통합하지 않고 별도 helper로 분리해 기존 as_socket 경로와 동작을 섞지 않음.
- R7 #2: 스펙이 명확했으므로(아래) 병합 방향은 EINVAL 쪽(기존 다수 호출부가 이미 사용 중인 `part_helper_internal::validate_send_flags`)으로 통일 — 새 함수를 만들지 않고 기존 걸 재사용(POSDDD: 중복 제거, 기존 것 우선).

## 스펙 판정 (R7 #2)
- `core/doc/spec/core/03-errors.ko.md:345-346`, `.en.md:362-363` (Submit result 매핑, 2. Submit result 표):
  - `ZLINK_SUBMIT_INVALID_ARGUMENT` ↔ `EINVAL, EMSGSIZE` — "잘못된 pointer, count, metadata 또는 **flags**" / "Invalid pointer, count, metadata, or **flags**"
  - `ZLINK_SUBMIT_NOT_SUPPORTED` ↔ `ENOTSUP` — "handle에서 지원하지 않는 operation" / "The handle does not support the operation"
- 스펙은 명확함(ko/en 동일): 잘못된 flags 값 → EINVAL. ENOTSUP은 handle이 operation 자체를 지원하지 않을 때(예: PUB/SUB에 unrouted send)만 해당.
- `socket_message_send_api.cpp:63`(옛 로컬 `validate_send_flags`, ENOTSUP)이 스펙에서 벗어난 쪽. `part_helper_api.cpp:245`(EINVAL)가 스펙과 일치. → **class B(기존 결함) 수정**: 로컬 함수 제거, 두 경로 모두 `part_helper_internal::validate_send_flags`(EINVAL) 사용.
- 두 경로 모두 `zlink::submit_result_internal::from_errno`로 `zlink_submit_result_t`를 만들므로(라인 133/588/658/720 확인), errno 변경은 `zlink_send`/`zlink_send_part`류 호출자가 관측하는 `ZLINK_SUBMIT_NOT_SUPPORTED`→`ZLINK_SUBMIT_INVALID_ARGUMENT` result 변경도 동반 — 스펙에 맞추는 방향이라 계약 위반 아님(오히려 기존 계약 불일치를 해소).

## 실행한 테스트와 남은 실패
- 위 ctest 5회 전부 통과, 남은 실패 없음.
- STREAM 공개 계약 테스트는 위 패턴에 포함(`unittest_public_contract_headers` 등). TSan은 mutex/pipe/mailbox/engine을 만지지 않아 미실행.

## 성능 표
- 해당 없음(성능 영향 없는 정적 클린업/errno 정정).

## 재확인한 스펙 절
- `core/doc/spec/core/03-errors.ko.md` §2 Submit result, `.en.md` 동일 절 — "어느 문장도 다른 동작이 되지 않았다": 위 errno 통일은 스펙이 이미 규정한 값과 다르게 동작하던 결함(ENOTSUP)을 스펙이 규정한 값(EINVAL)으로 고친 것이며, 스펙 문장 자체는 변경 없음.

## 변경 분류
- R7 #1(dead 필드/분기 제거): C(우회 아님, 순수 dead code 제거) — 분류상 A/B/C/D 중 실질은 리팩터이나 굳이 나누면 **C**(계약 영향 없음).
- R7 #3(close tail 통합): **C**.
- R7 #5(pub/sub helper): **C**.
- R7 #2(errno 통일): **B**(기존 결함 수정, 스펙 대조로 확정).
- R11 A(radix_tree 제거): **C**.
- R7 #6(capacity() 제거): 착수 후 실사용 확인되어 **되돌림** — 변경 없음.

## 멈춘 지점
- 없음. 계획한 R7 bundle A/C, #2 스펙 판정+수정, R11 bundle A 모두 완료. 커밋은 하지 않음(감독관 리뷰 대기).

## worktree
- `~/project/zlink-work/r7r11` (detached from main 80871f34f3), 커밋 안 함, diff 14개 파일(+140/-1141행, 대부분 radix_tree 삭제).
