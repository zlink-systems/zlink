# Round 94: PUBSUB/tls ASIO 경로 재점검

## 목적

`PUBSUB/tls/64B`는 May26 full/smoke 대비 반복 하락으로 남아 있다. 이전 round에서
`dist_t::write_at()` msg_more cache, XSUB empty subscription load 완화, TLS async_write_some 후보가
효과가 없거나 하락을 만들어 되돌렸다. 이번 round는 새 코드를 넣기 전에 ASIO/TLS steady-state 출력
경로에서 POSD 기준에 맞는 작은 후보가 남아 있는지 재점검했다.

## 확인한 코드

- `core/src/runtime/sockets/pubsub/xpub.cpp`
- `core/src/runtime/sockets/pubsub/xsub.cpp`
- `core/src/runtime/sockets/internal/dist.cpp`
- `core/src/runtime/engine/asio/asio_engine.cpp`
- `core/src/runtime/transports/tls/ssl_transport.cpp`

## 관찰

- `dist_t`에는 이미 single-pipe fast path와 matching HWM cache가 있다.
- `PUBSUB` 100 subscriber fanout은 일반 LMSG refcount fanout 경로를 탄다. 64B payload는 VSM이 아니므로
  refcount와 각 pipe write 비용이 누적된다.
- LMSG lifetime/refcount 정책을 바꾸는 후보는 `close()`, `rm_refs()`, zero-copy/slice, custom free
  function과 엮여 영향 범위가 크다. 1-2% 기대 후보로 남길 수 있는 변화가 아니다.
- ASIO output 경로에는 STREAM 중심 speculative/gather 정책이 이미 있다. `ssl_transport`의 write 방식을
  `async_write_some` 쪽으로 바꾸는 후보는 round80에서 `PUBSUB/tls`를 낮췄다.
- `PUBSUB/tls`만을 위해 TLS write 정책을 특수화하면 transport semantics가 새로 갈라지고, 실패 시
  TLS/WS/WSS 쪽 하락 위험이 크다.

## POSD 판단

- `PUBSUB/tls` 전용 shortcut은 정보 은닉과 깊은 모듈 원칙에 맞지 않는다.
- fanout message lifetime을 바꾸는 작업은 작은 hot path 변경이 아니라 별도 설계 작업이다.
- HWM, retry, flush 의미를 약화하는 변경은 공개 계약과 보안/안정성 리스크가 있어 제외한다.

## 결론

이 round에서는 코드 변경 후보를 채택하지 않는다. 현재 유지 후보는 round92의
`SPOT_SENDSEND` 단일 FINAL fast path뿐이다.

다음 후보는 다음 둘 중 하나로 제한한다.

- `PUBSUB/tls`를 더 보려면 LMSG fanout lifetime 설계를 별도 draft 수준으로 분리해 검토한다.
- 전체 목표를 계속 밀려면 `SPOT_SENDSEND`처럼 공개 계약을 바꾸지 않는 단일 FINAL/단일 part 상태 머신
  우회 후보를 다른 echo 경로에서 찾는다.
