---
title: "Issue #63 — whole-message recv 신설 + router recv 네이밍 정정 · 결정 기록"
---

# Issue #63 결정 기록 (감독 = 머신 B)

> 설계: `doc/draft/core-whole-message-recv-api.ko.md` · 적용 계획: `doc/plan/core-whole-message-recv-api-apply-plan.ko.md`
> 브랜치 `core/63-core-c-api-whole-message-recv-router-rec`, worktree `~/project/zlink-63-core-c-api-whole-message-recv-router-rec` (origin/main 4079715c03 분기).

## D63-1 (2026-09-10) 공개 이름 확정
- **PAIR/DEALER whole-message recv: `zlink_recv`**
- **ROUTER whole-message recv: `zlink_router_recv`**
- 근거: 기존 관용 `zlink_recv_part`(PAIR/DEALER)·`zlink_router_recv_part`(ROUTER)에서 `_part` = 단일 part.
  접미어 없는 형태를 whole-message로 대응시키는 것이 가장 단순·일관적. `zlink_recv_message`는 recv
  계열에 없는 새 명사("message")를 도입해 공개 표면을 넓힘 → 기각. draft §3.3 권장안과 일치.
- caller-제공 `zlink_msg_t[]` 배열 + capacity + count (zero-alloc, draft §3.2 (A)).

## D63-2 (2026-09-10) 내부 심볼 네이밍 정정
- `reqrep::recv_router_message_direct` → **`recv_router_record`**
- 형제 `recv_dealer_message_direct` → **`recv_dealer_record`** (대칭, 둘 다 contained)
- 근거: 두 함수는 DATA(`send`)·REQUEST를 모두 받는 일반 whole-record pull이라 "reqrep 소속·direct"
  이름이 오해를 줌. "record"는 스펙이 이미 쓰는 whole-multipart 단위 어휘
  (`core/doc/spec/core/socket/README.ko.md` "수신을 시작한 record는 마지막 part까지").
- 범위: **함수 이름만** 정정. 네임스페이스 `zlink::socket_reqrep_internal` 이동은 광범위·고위험이라
  이번 이슈 밖(향후 별도). 이름 옆에 "일반 router/dealer whole-record recv" 취지 주석만 남긴다.
- rename 대상(조사 시점 앵커, 편집 직전 재확인):
  - router: decl `socket_request_reply_internal.hpp:539`, impl `socket_request_reply_runtime_io.cpp:951`,
    호출 `socket_request_reply_router_api.cpp:160`
  - dealer: decl `socket_request_reply_internal.hpp:549`, impl `socket_request_reply_runtime_io.cpp:1186`,
    호출 `socket_message_recv_api.cpp:127`, `socket_message_api.cpp:160`

## 진행 로그
- 2026-09-10: worktree 생성(`work.sh start --issue 63`). 로컬 패키지 준비는 go boundary 테스트
  (`perf/internal/perfcommon/monotonic.go:C`, clean origin/main의 사전 존재 실패, go는 범위 밖)로
  실패 → Core 단계는 `--no-packages`로 진행. 바인딩 테스트·perf 단계 전에 패키지 재점검 필요.
- 단계 ①(네이밍 정정) codex 위임 준비.
