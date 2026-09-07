# core-rf R10-B 적용 보고서 (redo)

> **재작업 노트**: 이전 실행(R10-B, worktree `~/project/zlink-work/r10`, main `d98ef46fd7` 기준)은 게이트되기 전에
> worktree가 삭제되어 diff가 유실되었다. 이 문서는 job R10-B-redo가 새 worktree
> `~/project/zlink-work/r10b`(detached, `397c8e246a` 기준)에서 동일 범위를 다시 적용한 결과다. 아래 내용은
> 원본 보고서 구조를 유지하되 이번 실행의 실측치로 갱신했다.

- worktree: `~/project/zlink-work/r10b` (detached, `397c8e246a` — 지시된 기준 커밋)
- 범위: (1) `ctx_socket_registry.cpp`의 `wait_for_socket_removal`/`wait_for_socket_count_at_most` 중복 폴링 루프 →
  사설 템플릿 `wait_until(mutex_t*, timeout_ms, Predicate)`로 통합, (2) `ctx.hpp`의 `_slot_sync` 선언부에
  재진입 락 깊이 계약 주석, (3) `options.hpp`의 `send_pending_max_msgs/bytes` 선언부에 D-B85 이후 ABI 유지·동작
  no-op 주석. 공개 헤더(`core/include/**`) 변경 없음, 동작 변경 없음.

## 결과(수치)

- 빌드: `JOBS=4 scripts/build-core.sh dev` 성공(exit 0), 에러·경고 없음.
- `ctest --test-dir core/build-dev -R 'ctx|registry|socket_count|term|close|option'` 5회 연속 실행:
  매회 **14/14 통과(0 실패)**. 실행 시간 10.66~11.36초/회.

## 변경 파일

- `core/src/runtime/core/ctx_socket_registry.cpp` — `wait_for_socket_removal`/`wait_for_socket_count_at_most`의
  중복 폴링 루프(데드라인 계산 → predicate 확인 → timeout 처리 → `_socket_state_cv.wait`)를 사설 템플릿
  `wait_until(mutex_t *sync_, int timeout_ms_, Predicate predicate_)`로 추출. 두 함수는 각자의 predicate만
  람다로 넘긴다(`wait_for_socket_removal`: `!contains_socket(socket_)`, `wait_for_socket_count_at_most`:
  `socket_count() <= max_count_`). 타이밍 계산(데드라인, `wait_ms`, `ETIMEDOUT` 처리, `EAGAIN` 매핑)은 원본과
  1:1 동일.
- `core/src/runtime/core/ctx_socket_registry.hpp` — `wait_until` private 템플릿 선언 추가. 클래스 자체가
  `core/include/**`에 노출되지 않으므로 공개 인터페이스 변경 아님.
- `core/src/runtime/core/ctx.hpp` — `_slot_sync` 선언부에 계약 주석 추가: 이 락의 주소가 `mutex_t*`로
  `ctx_socket_registry_t::wait_for_socket_removal`/`wait_for_socket_count_at_most`에 그대로 전달되어
  `condition_variable_t::wait`가 정확히 1레벨만 unlock/relock하므로, 두 콜사이트는 대기 시점에 `_slot_sync`를
  재진입 깊이 1(중첩 아님)로 보유해야 한다는 점, 그리고 recursive mutex의 재진입 깊이가 condvar wait 전후로
  보존되는 것은 POSIX 보장이 아니라 glibc 구현 세부사항이라는 점을 명시.
- `core/src/runtime/core/options.hpp` — `send_pending_max_msgs`/`send_pending_max_bytes` 선언부에 D-B85(REQUEST
  계약 B 통일) 이후 Core REQUEST pending pool이 제거되어 이 두 옵션이 동작상 no-op이라는 점, get/setsockopt는
  ABI 호환을 위해 값을 저장·왕복만 한다는 점, 근거 문서 위치
  (`doc/plan/archive/core-0.17.0-dontwait-contract-and-perf-plan-b.ko.md:25`)를 명시하는 주석 추가.

## 설계 비교 및 선택 이유

- 항목 (1): (a) 매크로/자유함수 vs (b) private 템플릿 멤버 + 람다. 두 predicate 모두 `contains_socket`/
  `socket_count` 멤버 함수와 `this`가 필요해 자유함수로 뽑으면 인자 전달이 늘어난다. private 템플릿 멤버는
  헤더 노출 없이 캡슐화를 유지하면서 중복 폴링 루프를 제거해 POSDDD의 "규칙 수 줄이기"에 부합한다 — 이를 선택.
  템플릿을 `.cpp`에 정의하고 동일 TU 내 두 곳에서만 인스턴스화하므로 링크 문제 없음(빌드로 확인).
- 항목 (2)/(3): 코드 변경(락 계약을 assert로 강제, PENDING_MAX 저장 제거)은 계약/ABI를 건드리므로 지시대로
  주석 문서화만 수행. 별도 설계 대안 없음(문서화 전용 작업).

## 테스트

- dev 빌드 1회, `ctest -R 'ctx|registry|socket_count|term|close|option'` 5회(위 결과, 매회 14/14 통과).
- TSan 대상 아님: mutex/pipe/engine/mailbox의 락 순서·타이밍은 무변경(순수 코드 추출 + 주석), `_common-rules.md`의
  TSan 트리거 조건(pipe·engine·mailbox·mutex "만졌으면") 해당 없음 — 락 사용 방식 자체는 그대로.

## 성능 표

- 해당 없음(polling-loop/코드 스타일 리팩터, STREAM 성능 경로 무관 — perf 측정 생략).

## 스펙 절 재확인

- doc/principal 계약(completion, READY/DISCONNECTED, POLLIN/POLLOUT, WRITABLE wake 순서·조건) 및 D-B85 계약
  (getsockopt 왕복값)의 어느 문장도 다른 동작이 되지 않았다. 폴링 루프는 동일한 순서로 동일 조건을 그대로
  재실행하는 리팩터이며, 추가한 주석 2건은 코드 동작을 바꾸지 않는다.

## 변경 분류

B (기존 결함/중복 정리, 계약 변경 없음) — 및 문서화 전용 주석(동작 불변).

## 멈춘 지점

없음. 범위 내 전부 적용 완료. 커밋하지 않음(감독관 리뷰 대기). worktree `~/project/zlink-work/r10b`에 diff 보존.
