# Socket — PAIR 스펙-구현 gap 감사

> 감사 도구: codex (gpt-5.6-terra, reasoning high, read-only) · 2026-08-24
> 범위: `core/doc/spec/core/socket/01-pair.ko.md`와 `core/include/`, `core/src/`, `core/tests/`의 정적 대조. 실행 테스트는 하지 않았다.

판정: **구현/문서 gap 4건, 요확인 1건**. PAIR의 공개 ABI와 핵심 런타임 동작은 스펙과 일치한다. 발견 건은 모두 §5가 명시한 PAIR별 contract test 부재이며, 코드 동작 자체가 스펙과 모순된다는 정적 증거는 없었다.

## 대조 완료 계약군

- PAIR의 단일 peer 제한, 양방향 part 송수신, PAIR 수신의 `source_rid_out_ == NULL`: 일치
- `zlink_send_part`/`zlink_recv_part` signature, `ZLINK_PART_MORE`/`ZLINK_PART_FINAL` 값, `ZLINK_DONTWAIT`의 `EAGAIN` 결과 mapping: 일치
- multipart의 동일 thread·동일 send spec 유지, 실패 시 send scope rollback, part 소비와 다음 record reset: 일치
- PAIR `zlink_send_async`/handler/cancel 지원, `options_->target` 무시, 하나의 zero target queue에 의한 FIFO admission: 일치
- PAIR receive-flow 미지원(`ENOTSUP` → `ZLINK_CONFIG_NOT_SUPPORTED`), flow monitor detail/event 미생성: 일치
- pending 상한·timeout·취소·close fail-fast·completion callback 세부는 Socket 공통 문서가 소유하므로 중복 계상하지 않았다.

## Gap 목록

| 분류 | 스펙 근거 | 코드 근거 | 판단 |
|---|---|---|---|
| B. 구현 gap | `socket/01-pair.ko.md:176-179` — PAIR 양방향 `zlink_send_part`/`zlink_recv_part`, `source_rid_out_ == NULL`, 수신 part 소유권을 각각 test로 확인해야 함 | `core/src/runtime/sockets/pair/pair.cpp:49-56`은 단일 peer만 유지하고, `core/src/api/socket/socket_message_api.cpp:119-124`는 PAIR source RID를 노출하지 않는다. `core/tests/integration/test_pair_inproc.cpp:30-32`는 두 PAIR의 aggregate `bounce`만, `core/tests/integration/test_helper_ownership.cpp:20-90`는 각 방향의 일부 표면만 검증한다. | 양쪽 모두 `zlink_send_part`로 보내고 `zlink_recv_part`의 non-NULL output에서 `NULL` source RID와 close 소유권을 확인하는 PAIR contract test가 없다. 두 번째 peer가 기존 연결을 대체하지 않는 독점성 관찰도 없다. |
| B. 구현 gap | `socket/01-pair.ko.md:186-189` — 열린 PAIR multipart sequence의 중간/마지막 submit 실패 시 전체 record가 보이지 않아야 하고, 다음 submit은 새 record여야 함 | `core/src/api/socket/part_helper_api.cpp:668-677`은 실패 시 scoped send를 rollback하고 sequence를 reset한다. 그러나 `core/tests/integration/test_public_inproc_multipart_send.cpp:618-635`는 peer가 없는 첫 aggregate send의 part 소비만 확인하고, `core/tests/integration/test_send_async_multipart.cpp:541-582`의 partial-record 검증은 ROUTER async 경로다. | PAIR의 순차 `zlink_send_part(MORE …)` 경로에서 이미 staging한 part 뒤의 실패, peer의 무수신, 그리고 다음 record 재시작을 함께 검증하는 test가 없다. |
| B. 구현 gap | `socket/01-pair.ko.md:191-195` — PAIR async 표면 전체, `options_->target` 무시 및 여러 pending operation의 제출 순서 admission을 각각 확인해야 함 | `core/src/runtime/sockets/common/socket_send_complete.cpp:44-49,595-626,661-668`은 PAIR를 지원하고 target 해석을 건너뛰어 공통 zero target queue에 넣는다. `core/tests/integration/test_send_async_multipart.cpp:424-469`는 PAIR의 callback 재진입만 단일 operation으로 확인한다. | PAIR에 non-NULL target을 넣어도 수신/완료가 되는지와, backpressure 상태의 복수 async operation이 제출 순서로 완료되는지를 확인하는 contract test가 없다. |
| B. 구현 gap | `socket/01-pair.ko.md:197-200` — PAIR receive-flow 호출의 `ENOTSUP`, flow monitor detail/event 부재, byte HWM·low-water mark·backpressure 보존을 확인해야 함 | `core/src/runtime/sockets/common/socket_base_flow_state.cpp:15-32`, `core/src/runtime/sockets/common/socket_base_monitor.cpp:135-141`은 PAIR를 flow-state 대상에서 제외한다. `core/tests/integration/test_flow_state_c_api.cpp:262-276`는 PAIR의 `ZLINK_CONFIG_NOT_SUPPORTED` 결과만 확인한다. | test가 `errno == ENOTSUP`, PAIR monitor의 `ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE` 미설정 및 세 flow event 무발생, receive-flow 호출 전후 HWM/backpressure 불변을 검증하지 않는다. §5의 PAIR-specific observability 요구가 남아 있다. |

## 요확인

- `socket/01-pair.ko.md:19-20`의 “타입 전용 옵션이 없다”는 공개 option enum과 setter routing을 함께 대조했을 때 명시적 PAIR 전용 option은 찾지 못했다. 다만 이 부재를 자동으로 증명하는 source-level allowlist 또는 dedicated negative contract test는 확인하지 못했다. 공개 option 전체를 대상으로 PAIR 허용/거부 행렬을 생성해 확인할 필요가 있다.
