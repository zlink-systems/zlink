# Rust Poller socket monitor 작업 요약

## 변경

- `Poller::add_monitor(&self, &SocketMonitor, events, slot)`, `modify_monitor`, `remove_monitor`를 추가했다.
- monitor 등록/수정 마스크는 정확히 `POLLIN`만 허용하며, `POLLOUT`, `POLLCOMPLETION` 및 혼합 마스크는 `ConfigError(ConfigResult::InvalidArgument, EINVAL)`로 거절한다.
- monitor는 Core의 socket-handle 등록 경로를 직접 사용하고 completion-owner 상태에는 등록하지 않는다. `PollEvent`에서는 raw socket과 동일하게 `PollSourceKind::Socket`으로 식별된다.
- `SocketMonitor: Pollable` 구현은 선택하지 않았다. sealed `Pollable`은 `Poller` 외에도 public `proxy`의 socket-only 인자 계약이므로, monitor까지 구현하면 `proxy(&monitor, ...)`가 컴파일되는 잘못된 surface 확장이 생긴다. 별도 monitor 메서드가 기존 sealing 설계를 보존한다.
- sample의 blocking `monitor.recv()` 연결 대기를 monitor poller readiness 후 `recv_with_flags(RecvFlags::DONT_WAIT)`로 drain하도록 변경했다.
- DEALER→ROUTER 연결의 READY, 서버 close 후 DISCONNECTED, remove 후 poll event 미전달, 잘못된 쓰기/completion 마스크의 typed 오류를 inproc/tcp에서 검증했다.
- Rust README Poller 설명에 monitor 등록과 DONTWAIT drain 규칙 한 줄을 추가했다.
- 변경 파일: `bindings/rust/src/contracts/eventing/poller.rs`, `bindings/rust/src/runtime/eventing/poller.rs`, `bindings/rust/tests/monitor_tests.rs`, `bindings/rust/samples/sample_support.rs`, `bindings/rust/README.rustdoc.md`.
- 금지된 spec 경로와 Core 파일은 변경하지 않았다.

## 테스트 및 게이트

- 새 monitor poller 테스트 3건 × 5회: 모두 통과.
- `bindings/rust/tests/run_tests.sh`: 14/14 통과(samples 포함). main 트리 Core `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.17.0` 사용, Core 재빌드 시도 없음.
- `cargo clippy -j3 --all-targets -- -D warnings`: 통과.
- `cargo fmt --check`: 통과.
- `git diff --check`: 통과.
- Cargo target: `/home/hep7hep7/project/zlink-work/c016/rust-target-poller`, jobs 3.

## 언어 spec signature 문장 제안

- KO: `Poller`는 `add_monitor(&self, monitor: &SocketMonitor, events: i16, slot: usize) -> Result<(), ConfigError>`, `modify_monitor(&self, monitor: &SocketMonitor, events: i16) -> Result<(), ConfigError>`, `remove_monitor(&self, monitor: &SocketMonitor) -> Result<(), ConfigError>`를 제공해야 하며, monitor에는 `POLLIN`만 허용하고 ready `PollEvent`는 socket과 동일한 source kind/slot로 식별한 뒤 `SocketMonitor::recv_with_flags(RecvFlags::DONT_WAIT)`로 drain해야 한다.
- EN: `Poller` shall provide `add_monitor(&self, monitor: &SocketMonitor, events: i16, slot: usize) -> Result<(), ConfigError>`, `modify_monitor(&self, monitor: &SocketMonitor, events: i16) -> Result<(), ConfigError>`, and `remove_monitor(&self, monitor: &SocketMonitor) -> Result<(), ConfigError>`; a monitor accepts only `POLLIN`, and after a ready `PollEvent` identified with the same source kind and slot semantics as a socket, callers shall drain it with `SocketMonitor::recv_with_flags(RecvFlags::DONT_WAIT)`.

## BLOCKERS

- 없음.
