[English](./core-0.18.0.md) | [한국어](./core-0.18.0.ko.md)

# libzlink Core 0.18.0 릴리스 노트

Core 0.18.0은 공개 C ABI가 바뀌는 릴리스입니다. 0.17 계열과 SONAME 호환을 유지하지 않으며, 모든 binding은 0.18.0부터 다시 시작합니다.

## 주요 변경

- **whole-message send/recv로 통일.** 호출자가 `zlink_msg_t[]` 배열과 count(send) 또는 capacity(recv)를 넘기고 한 호출이 record 하나를 옮깁니다. 신설: `zlink_send`, `zlink_send_rid`, `zlink_request`, `zlink_reply`, `zlink_publish`, `zlink_recv`, `zlink_router_recv`, `zlink_subscribe`, `zlink_xpub_recv`(`zlink_xpub_recv_part`에서 이름 변경). `zlink_stream_recv_packet`·`zlink_multipart_close`는 그대로입니다.
- **제거:** `zlink_send_part`, `zlink_send_part_rid`, `zlink_request_part`, `zlink_reply_part`, `zlink_publish_part`, `zlink_recv_part`, `zlink_router_recv_part`, `zlink_subscribe_part`, `zlink_xpub_recv_part`, 공개 `zlink_part_flag_t`/`ZLINK_PART_MORE`/`ZLINK_PART_FINAL`. part 단위 호출이 남기던 미완성 record 상태와 "첫 part~FINAL 같은 thread" 규칙, `BUSY`, 부분 재시도 규칙이 함께 사라집니다.
- recv capacity가 record의 part 수보다 작으면 `ZLINK_RECV_BUFFER_TOO_SMALL`(`ENOBUFS`)을 돌려주고 record는 소비하지 않으며 필요한 part 수를 씁니다. 충분한 capacity로 재시도하면 같은 record를 정확히 한 번 받습니다.
- CPack NSIS 아이콘 경로 수정.

설계와 결정: `doc/draft/core-whole-message-recv-api.ko.md` §7, `doc/plan/issue-63-worklog/decisions.ko.md`(D63-1~8). Issue #63, PR #86.

## 검증

- release-gate 빌드(LTO) ctest 214/214, 공개 표면 검사 PASS(함수 99개, export 일치), binding 계약 테스트 6종 PASS.
- bindings perf(multi routed, tcp)는 0.17.4 대비 4언어 모두 향상(cpp +8~+187%, java +16~+240%, dotnet +27~+187%, node +18~+284%). `doc/plan/issue-63-worklog/perf-results.ko.md`.
- hotpath_gate 결과는 Issue #102에 기록합니다.

## 마이그레이션

part 단위 호출을 배열+count 호출 하나로 바꿉니다. 각 언어 binding의 공개 시그니처는 바뀌지 않았고 내부 구현만 whole-message로 전환했으므로, binding 사용자는 binding 0.18.0으로 올리기만 하면 됩니다. C API 직접 사용자는 `core/doc/spec/core/socket/README.ko.md`의 send·recv 절을 참조하십시오.

릴리스 태그는 [`core/v0.18.0`](https://github.com/zlink-systems/zlink/releases/tag/core%2Fv0.18.0)입니다.
