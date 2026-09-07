# core-rf R10-B 적용 보고서

- worktree: `~/project/zlink-work/r10` (detached, main `d98ef46fd7` 기준, 인벤토리 기준 `482d7bca80`와 diff 없음 — 그 사이 커밋 없음 확인)
- 범위: 인벤토리 묶음 B(항목 3, 5) + 묶음 C(항목 1). 묶음 A(항목 2, msg join/leave)는 G-2에서 이미 처리되어 제외, 항목 4(`socket_poller::rebuild`)는 지시대로 제외.

## 결과(수치)

- 빌드: `JOBS=4 scripts/build-core.sh dev` 성공(0 에러), 링크 에러 없음.
- `ctest --test-dir core/build-dev -R 'ctx|registry|socket_count|term|close|option'` 5회: 매회 14/14 통과(0 실패).

## 변경 파일

- `core/src/runtime/core/ctx_socket_registry.cpp` — `wait_for_socket_removal`/`wait_for_socket_count_at_most`의 중복 폴링 루프(데드라인 계산→predicate 확인→timeout 처리→cv.wait)를 사설 템플릿 `wait_until(mutex_t*, timeout_ms, Predicate)`로 추출. 두 함수는 각자의 predicate(람다)만 넘긴다.
- `core/src/runtime/core/ctx_socket_registry.hpp` — `wait_until` private 템플릿 선언 추가(공개 API 아님, 클래스 자체가 `core/include/**`에 노출되지 않음).
- `core/src/runtime/core/ctx.hpp` — `_slot_sync` 선언부에 계약 주석 추가: `mutex_t*`로 넘겨 `condition_variable_t::wait`에 그대로 전달되므로 재진입 깊이가 1인 상태로만 호출되어야 함(현재 콜사이트 2곳 모두 그러함), recursive mutex의 cond_wait 재진입 보존은 POSIX 보장이 아닌 glibc 구현 세부사항이라는 점을 명시.
- `core/src/runtime/core/options.hpp` — `send_pending_max_msgs`/`send_pending_max_bytes` 선언부에 D-B85 이후 ABI 유지·동작상 no-op(저장/왕복만, 강제 없음)이라는 주석 추가, 근거 문장 위치(`doc/plan/archive/core-0.17.0-dontwait-contract-and-perf-plan-b.ko.md:25`) 명기.

## 설계 비교 및 선택 이유

- 항목 3: (a) 매크로/자유함수 vs (b) private 템플릿 멤버 + 람다. 두 predicate 모두 `contains_socket`/`socket_count` 같은 멤버 함수와 `this`가 필요해 자유함수는 인자 전달이 늘어남. private 템플릿 멤버가 헤더 노출 없이 캡슐화를 유지하면서 중복을 제거해 POSDDD상 "규칙 수 줄이기"에 부합 — 이를 선택.
- 항목 5/1: 코드 변경(락 계약을 assert로 강제, PENDING_MAX 저장 제거)은 계약/ABI를 건드리므로 지시대로 주석 문서화만 수행.

## 테스트

- dev 빌드 1회, ctest 5회(위 결과). TSan 대상 아님(mutex/pipe/engine/mailbox 동작 자체는 변경 없음, 락 순서·타이밍 무변경 — 순수 코드 추출).

## 스펙 절 재확인

- doc/principal 계약(completion, READY/DISCONNECTED, POLLIN/POLLOUT, WRITABLE wake 순서·조건) 및 D-B85 계약(getsockopt 왕복값) 어느 문장도 다른 동작이 되지 않았다 — 폴링 루프는 동일한 순서로 동일 조건을 그대로 재실행하는 리팩터이며, 주석 추가 2건은 동작 변경 없음.

## 변경 분류

B (기존 결함/중복 정리, 계약 변경 없음) — 및 문서화 전용 주석(C 표시 대상 없음, 코드 동작 불변).

## 멈춘 지점

없음. 범위 내 전부 적용 완료. 커밋하지 않음(감독관 리뷰 대기).
