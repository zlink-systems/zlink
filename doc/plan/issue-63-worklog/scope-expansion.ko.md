---
title: "Issue #63 범위 확대 — part 기반 공개 API 전면 제거 + whole-message send/recv 통일"
---

# Issue #63 범위 확대 (2026-09-10, 사용자 결정)

> 설계·조사 근거: `doc/draft/core-whole-message-recv-api.ko.md` §7. 반영 대상: `doc/plan/core-whole-message-recv-api-apply-plan.ko.md` §10.
> 기존 결정(D63-1..D63-5)은 `decisions.ko.md`. 이 문서는 확대분과 실행 방식만 담는다.

## 확대 내용
- **제거 8개** → whole-message 신설: `send_part→send`, `send_part_rid→send_rid`, `request_part→request`,
  `reply_part→reply`, `publish_part→publish`, `recv_part→recv`, `router_recv_part→router_recv`,
  `subscribe_part→subscribe`.
- **이름만 변경**: `xpub_recv_part→xpub_recv`(계약·인자 불변, zlink_msg_t 안 받음).
- **유지**: `zlink_stream_recv_packet`(header/body 2슬롯), `zlink_multipart_close`(배열 해제 헬퍼).
- **공개 표면에서 삭제**: `zlink_part_flag_t`(MORE/FINAL). send는 배열 순서, recv는 part_count가 대체.
- **스펙에서 삭제되는 계약**: "한 record 첫 part~FINAL 같은 thread"(README §2), mid-sequence BUSY,
  실패 FINAL의 staged-prefix 폐기 규칙 → whole-record 재시도로 대체.

## 감독 결정 — §10.2 계약 셋 (D63-6..D63-8)
- **D63-6 count>capacity**: recv/subscribe에 D63-3 그대로(비소비 `ZLINK_RECV_BUFFER_TOO_SMALL`/ENOBUFS,
  필요 count 반환, 재시도 시 동일 record). send는 caller가 정확한 count를 주므로 해당 없음.
- **D63-7 STREAM**: 배열 API로 접지 않고 STREAM 전용 단일-part send/`stream_recv_packet` 유지.
  유효 RID로 보내는 길이 0 part의 "peer 끊기" 의미 보존(08-stream.en.md:151-153).
- **D63-8 재시도 단위**: whole-record 한 번. part 단위 부분 재시도 없음(실패=전부 미소비 또는 전부 소비).

## 실행 방식 (사용자 지정 2026-09-10)
- **모델**: heavy 코드 작업은 **codex astra / xhigh**(계약·사양 충돌·고위험 대규모 제거).
  astra 입력 272K 초과 시 요금 급증 → 브리프 범위·읽을 파일 좁힘.
- **순서**: Core 먼저(직렬) → 완료 후 **bindings는 언어별 병렬**(detached worktree, 동시 최대 3, WSL 빌드 2).
- **바인딩 범위**: 제거가 빌드를 깨므로 **7개 전부 마이그레이션**(c/cpp/dotnet/java/node/go/python/rust)
  + framework/bench(18). framework/languages는 0건. perf 측정은 cpp/node/java/dotnet 4종.
- **PR**: #63 한 브랜치 누적, 마지막 Closes #63. 제거는 마지막 분리 커밋(apply-plan §10.3).

## Core 단계 세부
1. Core 스펙(감독 직접): send 신설 + 제거 + §2 thread 규칙/BUSY/staged-prefix 삭제 + STREAM/SUB·XSUB.
2. **Core-add**(astra): send/send_rid/request/reply/publish/subscribe + xpub_recv(신규 이름, 옛 이름 임시 병존)
   + 신규 테스트. recv/router_recv는 ③에서 이미 구현됨.
3. **Core-remove**(astra, 마지막): 옛 8개 심볼 제거 + part_flag_t 공개 제거 + libzlink.vers 정리 +
   core/tests 전면 whole-message 이관 + 사용 안 되는 part-adapter 정리.
4. Core guide(감독): recv_part 병존 서술 → whole-message 단일로 갱신.
