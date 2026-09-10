---
title: "Issue #63 바인딩 마이그레이션 공유 참조 (7개 언어 공통)"
---

# 바인딩 마이그레이션 — whole-message send/recv only

Core는 이제 **whole-message API만** 노출한다(part API 전면 제거, `50678dd309`). 각 바인딩은
**내부 구현만** 새 API로 바꾼다 — **공개 바인딩 표면(recv(Received)/send builder 등)은 불변**.

## 제거된 Core 심볼 → 대체 (내부 호출을 전부 교체해야 컴파일됨)
| 제거됨(이제 없음) | 대체 |
|---|---|
| `zlink_send_part` | `zlink_send(s, parts[], count, flags, ctx, cid_out)` |
| `zlink_send_part_rid` | `zlink_send_rid(s, rid, parts[], count, flags, ctx, cid_out)` |
| `zlink_request_part` | `zlink_request(s, rid_or_null, parts[], count, flags, timeout_ms, ctx, cid_out)` |
| `zlink_reply_part` | `zlink_reply(router, source_rid, token, parts[], count)` |
| `zlink_publish_part` | `zlink_publish(subject, topic_id, parts[], count, flags)` |
| `zlink_recv_part` | `zlink_recv(s, rid_out, parts_out[], cap, count_out, flags)` (PAIR/DEALER; **STREAM RAW도 여기**, 1 part) |
| `zlink_router_recv_part` | `zlink_router_recv(r, rid_out, token_out, parts_out[], cap, count_out, flags)` |
| `zlink_subscribe_part` | `zlink_subscribe(sub, rid_out, topic_buf, topic_cap, topic_len_out, parts_out[], cap, count_out, flags)` |
| `zlink_xpub_recv_part` | `zlink_xpub_recv(...)` (동일 시그니처, 이름만) |
| `zlink_part_flag_t`/`ZLINK_PART_MORE`/`FINAL` | 없음. send는 배열 순서, recv는 count가 대신함 |

**유지**: `zlink_stream_recv_packet`(header/body 2슬롯), `zlink_multipart_close`(배열 해제), `zlink_completion_*`.

## 공통 계약 (D63-6..D63-8)
- **send**: 한 호출이 record 하나를 원자적으로 제출. 모든 입력 슬롯을 소비(성공·실패 모두). 실패 시
  whole-record 재시도(부분 제출 없음). `count==0`→EINVAL, 필수 NULL→EFAULT. NONE send/send_rid는
  user_context==NULL.
- **recv/subscribe**: caller-제공 배열, capacity < part_count → **비소비** `ZLINK_RECV_BUFFER_TOO_SMALL`
  (ENOBUFS)+필요 count, 재시도 시 동일 record. 슬롯은 미초기화 허용(Core가 adopt로 채움).
  `zlink_multipart_close`로 해제.
- **STREAM**: RAW 수신=`zlink_recv`(1 part), PACKET=`zlink_stream_recv_packet`, 송신=`zlink_send`/`send_rid`
  count==1(빈 part=peer 끊기 의미 보존, >1은 ENOTSUP).

## 각 바인딩이 할 일
1. FFI/네이티브 선언에서 제거된 심볼 → 새 심볼로 교체(part_flag 인자 삭제, 배열+count 도입).
2. 내부 send 루프(part마다 호출) → `zlink_send*` 1회 호출(재사용 배열, 소유권 이전).
3. 내부 recv 루프(recv_part while) → `zlink_recv`/`zlink_router_recv`/`zlink_subscribe` 1회 호출
   (재사용 배열, capacity grow-on-ENOBUFS 재시도). cpp recv는 이미 전환됨(b7363590f3) — send·STREAM·잔여 part 사용만.
4. xpub_recv_part→xpub_recv 이름 교체. part_flag_t/MORE/FINAL 사용 제거.
5. 공개 표면 불변. 계약 테스트는 **로컬 new-only Core**(ZLINK_CORE_SOURCE=local, core/build-dev) 상대로 통과.
6. per-message native-call 수를 검증하던 테스트가 있으면 새 관측값으로 갱신(공개 계약 약화 금지, 사유 기록).

## 참조 구현
- Core whole-message recv 사용례: `bindings/cpp/src/Runtime/Native/native_receive.hpp`(재사용 배열+grow retry).
- Core 신설 send 계약: `core/include/zlink/socket/api.h`의 whole-message send 주석.
