# Round 150: SPOT/wss publish path triage

## 목표

- round149에서 May26 full 대비 `MULTI_SPOT/wss 64B`가 `-10.33%`로 남은 원인을 코드 경로에서 좁힌다.
- tcp/ws 하락을 만들었던 기존 후보를 반복하지 않는다.
- POSD-safe 후보가 아니면 source 변경 없이 배제한다.

## 기준

- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- round149 refresh:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_052834_round149_pubsub_spot_worst_refresh.txt`

## 확인한 call path

- perf server:
  - `bindings/c/perf/multi/src/perf_multi_spot_server.cpp`
  - `perf_zlink_spot_publish_parts(state->pub, "bench", &part, 1, flags)`
- core publish entry:
  - `spot_pub_t::publish()`
  - socket-backed pub side면 `logical_multipart_publish(socket, topic, parts, part_count, flags, true)`
- multipart publish:
  - `send_publish_once()`
  - topic frame 생성 후 `ZLINK_SNDMORE`로 topic을 보내고, payload frame을 보낸다.

## 검토한 후보

### 후보 A: `spot_pub_t::_publish_sync` 제거 또는 socket-backed 경로에서 생략

- 가설:
  - `logical_multipart_publish(..., force_sync=true)`가 socket public send scope를 이미 잡으므로,
    `_publish_sync`가 hot path에서 중복 lock일 수 있다.
- 코드 확인:
  - `socket_public_send_scope_t`는 public API sync를 잡지만, blocking retry나 send-ready handler 상황에서
    sync를 풀 수 있는 경로가 있다.
  - `logical_multipart_publish()`는 topic frame과 payload frame을 하나의 multipart transaction으로 보내야 한다.
  - `_publish_sync`를 제거하면 publish 호출 간 transaction 직렬화 의미가 socket scope 세부 구현에 더 강하게
    의존한다.
- POSD 판단:
  - 겉보기 중복 lock 제거처럼 보이지만, multipart transaction 경계를 `spot_pub_t` 밖의 socket retry 정책에
    새로 의존하게 만든다.
  - 정보 은닉과 오류를 정의로 없애는 원칙에 맞지 않는다.
- 결론:
  - 적용하지 않는다.

### 후보 B: topic frame 생성 경로 최적화

- 가설:
  - 64B publish는 topic `"bench"`를 매번 `strlen`, `init_size`, `memcpy`로 만든다.
- 코드 확인:
  - topic frame은 5B라 `msg_t`의 small-message 경로에 머문다.
  - topic id 길이를 별도 인자로 전달하는 API/내부 overload를 추가하면 call surface와 정보 전달이 늘어난다.
- POSD 판단:
  - 비용은 작고, 새 overload는 public/internal 경계에 topic length 지식을 퍼뜨린다.
- 결론:
  - 적용하지 않는다.

## 기존 반려 후보와의 관계

- SPOT publish ingress move는 WSS/TLS를 올렸지만 tcp/ws를 낮춰 반려됐다.
- ASIO handler allocator 확대는 WSS 일부를 올렸지만 SPOT_SENDSEND/wss와 SPOT_REQREP/wss를 낮춰 반려됐다.
- direct refresh 계열은 non-tcp 일부 상승과 tcp/ws 하락 tradeoff가 있어 반려됐다.

## 판단

- 이번 round에서는 source 변경을 하지 않는다.
- `SPOT/wss`는 아직 May26 full 대비 큰 회귀 경계에 있지만, 현재 코드에서 tcp/ws 하락 없이 좁게 고칠 수 있는
  후보는 확인하지 못했다.
- 다음 후보는 SPOT 내부가 아니라 WSS transport write/read scheduling 또는 ROUTER/DEALER 공통 64B 경로에서
  찾아야 한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 없음. source 변경 없이 triage만 수행했다.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message 전체 사본 제거, mtrie 비재귀화, 포트 파싱 검증, IPC unlink 순서,
    decoder/message/send guard, `maxmsgsize` 정책을 수정하지 않는다.
- 추가로 실행한 회귀 테스트:
  - source 변경 없음.
