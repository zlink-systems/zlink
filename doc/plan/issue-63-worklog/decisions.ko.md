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

## D63-3 (2026-09-10) whole-message recv capacity 정책
- caller가 준 `parts_capacity_`가 record의 part 수보다 작으면:
  **`ZLINK_RECV_BUFFER_TOO_SMALL`(207, `errno == ENOBUFS`) 반환, record는 소비하지 않음(비소비).**
  `*part_count_out_`에 필요한 part 수를 쓰고, 다른 output(rid/token/parts 슬롯)은 변경하지 않는다.
  충분한 capacity로 재시도하면 같은 record를 **정확히 한 번** 받는다.
- 근거: `ZLINK_RECV_BUFFER_TOO_SMALL` enum 주석이 이미 "first record or topic does not fit"라고
  이 경우를 예상함(`core/include/zlink_errno.h:157`). 기존 `zlink_subscribe_part`의 topic buffer
  too-small 관용(README.ko.md "필요한 길이만 쓰고 … record는 그대로 … 재시도하면 정확히 한 번")과
  동일. record 원자성과 정합(부분 record 상태가 남지 않음).

## D63-4 (2026-09-10) parts 배열 소유권
- `parts_out_`는 caller-제공 `zlink_msg_t[]` 배열(용량 `parts_capacity_`). 성공 시 앞의
  `*part_count_out_`개 슬롯이 각각 caller-소유 msg가 되고, caller는 `zlink_multipart_close(parts_out_,
  count)`(또는 슬롯별 `zlink_msg_close`)로 정확히 한 번 닫는다.
- 실패 시 소유권 이동 없음, output·message content 불변(기존 recv 계열 규칙과 동일).
- 02-message §4가 이미 "`zlink_msg_t` 연속 배열을 `zlink_multipart_close`로 일괄 닫는다"를 정의 →
  whole-message recv가 그 배열을 채우는 생성 경로다.

## recv_part 혼용 규칙
- record 원자성으로 whole-message recv와 `*_recv_part`는 호출 간 공유 커서 상태를 남기지 않는다.
  동시/타 스레드·family 진입은 기존과 동일하게 `ZLINK_RECV_BUSY`(EBUSY). 새 규칙 불필요(draft §5).

## 진행 로그
- 2026-09-10: worktree 생성(`work.sh start --issue 63`). 로컬 패키지 준비는 go boundary 테스트
  (`perf/internal/perfcommon/monotonic.go:C`, clean origin/main의 사전 존재 실패, go는 범위 밖)로
  실패 → Core 단계는 `--no-packages`로 진행. 바인딩 테스트·perf 단계 전에 패키지 재점검 필요.
- 2026-09-10: **① 네이밍 정정 완료·커밋**(ef85ac8e24, codex terra/high). 순수 rename 8파일 +13/-11,
  옛 이름 0건, 213/214 ctest 통과. 단일 실패 `158-hotpath_gate`는 dev(LTO OFF) 빌드에서
  LTO 캘리브레이션 reference 대비 Ir 1.12–1.37×로 나오는 **build-mode 아티팩트**(release-gate 전용
  게이트, origin/main도 동일 실패, rename 무관)로 판정 → 커밋 본문에 근거 기록.
- 2026-09-10: ② Core 스펙(감독 직접) 착수. hot-path 스펙의 옛 이름 참조
  (`core/doc/spec/core/systems/10-hot-path.{ko,en}.md`)도 이 단계에서 `recv_dealer_record`로 정정.
