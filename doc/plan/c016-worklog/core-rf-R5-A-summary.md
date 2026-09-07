# core-rf R5-A 결과 보고

작업: Phase 3 적용 job R5-A. worktree `~/project/zlink-work/r5` (detached, main 3586f0eb17 기준).
파일: `core/src/api/socket/socket_request_reply_submit_api.cpp` (1개, +238/-158).

## 결과 (수치)

- dev 빌드: 성공(core/build-dev, RelWithDebInfo, LTO OFF).
- `ctest --test-dir core/build-dev -R 'request|reply|reqrep|router|dealer|timeout'` 5회 반복: 매회 41/41 통과, 0 실패.
- hotpath 게이트(`dealer_router_reqrep_inproc`, valgrind callgrind, core/build-gate: Release+LTO+tests):
  reference 19682.196 / measured 19486.762 / **ratio 0.9901 → PASS**(≈1.00).

## 변경 내용

인벤토리 항목 1·2를 함께 처리(같은 파일, 서로 얽힌 파라미터라 분리 적용이 더 번거로움):

1. `request_part_common`(278행, 파라미터 10개)을 인벤토리 제안대로 두 static 함수로 분리
   - `submit_single_part_request_fast_path` — 1-part FINAL(part_helper 연속 아님) 경로. 파라미터
     5개(socket_handle_, peer_rid_, part_, spec 참조, request_ctx_ 참조).
   - `submit_buffered_request_step` — MORE 버퍼링 + FINAL 플러시(멀티파트) 경로. 파라미터 7개
     (socket_handle_, handle_, peer_rid_, part_, part_flag_, spec 참조, request_ctx_ 참조).
   - 남은 `request_part_common`은 파트 검사·spec 조립·part_helper continuation 조회·"MORE인데
     request_seq 미확정" 조기 버퍼링·그룹 검증·request_state/pending_token 해석까지만 담당하고,
     마지막에 두 함수 중 하나로 위임(약 60행으로 축소).
2. `finish_dontwait_request_admission_failure`(파라미터 7개)를 `request_admission_failure_ctx_t`
   구조체(socket/peer_rid/user_context/state/identity/completion_id_out/request_wait)로 묶어
   파라미터 2개(ctx, failure_errno_)로 축소. 호출부 2곳(fast path, buffered path 각각) 갱신.

추가로 두 함수가 공유하는 `request_submit_context_t`(request_state, pending_token,
reserved_completion_id, user_context, completion_id_out)를 도입해 파라미터 그룹을 명확히 함.

## 설계 비교

- **대안 A(채택)**: 인벤토리가 제시한 경계(1-part fast path vs 버퍼링 경로)로 분리 + 두 개의
  작은 컨텍스트 구조체. 각 함수의 책임이 이름과 일치하고, 호출자 쪽 `request_part_common`이
  "검증 → 해석 → 위임"만 남아 읽기 쉬움.
- **대안 B(기각)**: 파라미터를 개별로 유지한 채 함수만 쪼갬. 파라미터 개수가 그대로거나 더
  늘어나(같은 값을 여러 함수에 반복 전달) POSDDD의 "규칙 수 줄이기" 취지에 맞지 않아 기각.
- 항목 3(`send_public_router_reply_with_wait`)과 항목 4(`recv_router_message_direct`)는 묶음 B
  범위이므로 이번 job에서는 손대지 않음. 항목 7(aggregate-timeout)은 지시대로 제외.

## 테스트

- ctest 패턴 5회(위 수치), 실패 없음.
- hotpath_gate 1회(측정 전후 재실행 없이 1회, ratio 0.99로 임계 내).
- TSan: 이번 변경은 pipe/engine/mailbox/mutex 잠금 구조를 건드리지 않고 파라미터 전달 방식만
  바꾼 순수 리팩터라 TSan 재실행은 생략(공통 규칙의 "만졌으면" 조건에 해당하지 않음).

## 재확인한 스펙 절

- README "Part send"(MORE/FINAL 분기, 1-part FINAL fast path 우선순위) — 각주 로직 그대로
  이동, 조건식 변경 없음.
- dealer/router REQUEST 섹션(상관관계 발행 시점: "선택된 candidate가 write를 수락한 뒤, 최종
  flush 전") — 원본 주석과 순서 그대로 `submit_single_part_request_fast_path`로 이동.
- 03-errors(EAGAIN → writable-wait 등록, ENOENT→EHOSTUNREACH 정규화 등) — 로직/순서/errno 값
  변경 없이 `finish_dontwait_request_admission_failure`로 그대로 이관.
- 어느 문장도 다른 동작이 되지 않았음을 확인함(모든 분기의 반환값·errno·호출 순서가 원본과
  1:1 대응).

## 변경 분류

B(순수 리팩터/구조 정리, 계약·동작 불변).

## 멈춘 지점

없음. 상한 1.5h 이내 완료.

## 커밋 안 함

worktree diff만 남김: `~/project/zlink-work/r5` (git status: 1 file modified, uncommitted).
