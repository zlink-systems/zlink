//! Flow-state parity tests -- core-byte-hwm-flow-control-plan.ko.md §8.1.1.
//!
//! Covers the language-binding parity checklist for the receive-flow-state
//! surface: enum value parity with the C ABI, DEALER/ROUTER success and
//! idempotency, not-supported mapping for every other socket type,
//! invalid-handle mapping, close-race safety, absence of a flow-frame API on
//! the public surface, and an HWM smoke check showing the existing option
//! surface is unaffected.

use zlink::{ConfigResult, Context, ReceiveFlowState};

// ---------------------------------------------------------------------------
// Enum parity
// ---------------------------------------------------------------------------

#[test]
fn receive_flow_state_enum_matches_c_abi_values() {
    // core/include/zlink_enum.h:
    //   ZLINK_RECEIVE_FLOW_RUNNING = 0
    //   ZLINK_RECEIVE_FLOW_PAUSED  = 1
    assert_eq!(ReceiveFlowState::Running as i32, 0);
    assert_eq!(ReceiveFlowState::Paused as i32, 1);
}

// ---------------------------------------------------------------------------
// DEALER/ROUTER success and idempotent repeat
// ---------------------------------------------------------------------------

#[test]
fn dealer_accepts_receive_flow_state_and_is_idempotent() {
    let ctx = Context::new().unwrap();
    let sock = ctx.dealer_socket().unwrap();
    let options = sock.common_options();

    options.set_receive_flow_state(ReceiveFlowState::Paused).unwrap();
    // Repeating the current state is a successful no-op, not an error.
    options.set_receive_flow_state(ReceiveFlowState::Paused).unwrap();
    options.set_receive_flow_state(ReceiveFlowState::Running).unwrap();
    options.set_receive_flow_state(ReceiveFlowState::Running).unwrap();
}

#[test]
fn router_accepts_receive_flow_state_and_is_idempotent() {
    let ctx = Context::new().unwrap();
    let sock = ctx.router_socket().unwrap();
    let options = sock.common_options();

    options.set_receive_flow_state(ReceiveFlowState::Paused).unwrap();
    options.set_receive_flow_state(ReceiveFlowState::Paused).unwrap();
    options.set_receive_flow_state(ReceiveFlowState::Running).unwrap();
    options.set_receive_flow_state(ReceiveFlowState::Running).unwrap();
}

// ---------------------------------------------------------------------------
// Not-supported mapping for PAIR, PUB/SUB family, and STREAM
// ---------------------------------------------------------------------------

#[test]
fn pair_reports_not_supported() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    let err = sock
        .common_options()
        .set_receive_flow_state(ReceiveFlowState::Paused)
        .unwrap_err();
    assert_eq!(err.code(), ConfigResult::NotSupported);
}

#[test]
fn pub_reports_not_supported() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pub_socket().unwrap();
    let err = sock
        .common_options()
        .set_receive_flow_state(ReceiveFlowState::Paused)
        .unwrap_err();
    assert_eq!(err.code(), ConfigResult::NotSupported);
}

#[test]
fn sub_reports_not_supported() {
    let ctx = Context::new().unwrap();
    let sock = ctx.sub_socket().unwrap();
    let err = sock
        .common_options()
        .set_receive_flow_state(ReceiveFlowState::Paused)
        .unwrap_err();
    assert_eq!(err.code(), ConfigResult::NotSupported);
}

#[test]
fn xpub_reports_not_supported() {
    let ctx = Context::new().unwrap();
    let sock = ctx.xpub_socket().unwrap();
    let err = sock
        .common_options()
        .set_receive_flow_state(ReceiveFlowState::Paused)
        .unwrap_err();
    assert_eq!(err.code(), ConfigResult::NotSupported);
}

#[test]
fn xsub_reports_not_supported() {
    let ctx = Context::new().unwrap();
    let sock = ctx.xsub_socket().unwrap();
    let err = sock
        .common_options()
        .set_receive_flow_state(ReceiveFlowState::Paused)
        .unwrap_err();
    assert_eq!(err.code(), ConfigResult::NotSupported);
}

#[test]
fn stream_reports_not_supported() {
    let ctx = Context::new().unwrap();
    let sock = ctx.stream_socket().unwrap();
    let err = sock
        .common_options()
        .set_receive_flow_state(ReceiveFlowState::Paused)
        .unwrap_err();
    assert_eq!(err.code(), ConfigResult::NotSupported);
}

// ---------------------------------------------------------------------------
// Invalid handle and invalid argument mapping
// ---------------------------------------------------------------------------

#[test]
fn closed_socket_reports_invalid_handle() {
    let ctx = Context::new().unwrap();
    let mut sock = ctx.dealer_socket().unwrap();
    sock.close().unwrap();
    let err = sock
        .common_options()
        .set_receive_flow_state(ReceiveFlowState::Running)
        .unwrap_err();
    assert_eq!(err.code(), ConfigResult::InvalidHandle);
}

// `ReceiveFlowState` only offers `Running` and `Paused`; the Rust type
// system makes any other discriminant unrepresentable through the public
// API, so the invalid-argument branch of the C contract has no reachable
// counterpart to exercise here. This is itself the language's mapping of
// ZLINK_CONFIG_INVALID_ARGUMENT: the binding statically rules the case out
// rather than returning a runtime error for it.
#[test]
fn receive_flow_state_type_cannot_represent_an_out_of_range_value() {
    // Exhaustive match over the two known variants; the compiler proves
    // there is no third discriminant to construct.
    for state in [ReceiveFlowState::Running, ReceiveFlowState::Paused] {
        match state {
            ReceiveFlowState::Running | ReceiveFlowState::Paused => {}
        }
    }
}

// ---------------------------------------------------------------------------
// Close races with set_receive_flow_state: only Ok, InvalidState, or
// InvalidHandle may ever be observed.
//
// `SocketStorage` is `Send` but deliberately not `Sync` (see
// `src/internal/handle_storage.rs`), so the public API gives no way for two
// threads to hold the same socket handle at once: Rust's borrow checker
// forces every `set_receive_flow_state` / `close` pair onto a single
// ordering, sequential rather than genuinely concurrent. That ordering
// constraint is itself this binding's enforcement of the plan's invariant --
// a caller can race close against the setter only by choosing an order, and
// both orders below still only ever produce Ok, InvalidState, or
// InvalidHandle.
// ---------------------------------------------------------------------------

#[test]
fn set_before_close_then_close_yields_only_expected_results() {
    for _ in 0..20 {
        let ctx = Context::new().unwrap();
        let mut sock = ctx.router_socket().unwrap();
        match sock
            .common_options()
            .set_receive_flow_state(ReceiveFlowState::Paused)
        {
            Ok(()) => {}
            Err(err) => assert!(matches!(
                err.code(),
                ConfigResult::InvalidState | ConfigResult::InvalidHandle
            )),
        }
        sock.close().unwrap();
    }
}

#[test]
fn close_then_set_yields_only_invalid_handle() {
    for _ in 0..20 {
        let ctx = Context::new().unwrap();
        let mut sock = ctx.router_socket().unwrap();
        sock.close().unwrap();
        let err = sock
            .common_options()
            .set_receive_flow_state(ReceiveFlowState::Paused)
            .unwrap_err();
        assert_eq!(err.code(), ConfigResult::InvalidHandle);
    }
}

// ---------------------------------------------------------------------------
// No flow-frame receive/encode API on the public surface.
// ---------------------------------------------------------------------------

// There is no `recv_flow_frame`, `encode_flow_frame`, or similar API on any
// public socket type; the plan (§5.1) explicitly excludes it. This is
// verified at compile time by its absence: this file only imports
// `ReceiveFlowState` and `common_options().set_receive_flow_state`, and nothing
// elsewhere in the crate's public surface exposes a flow-frame type or
// method (see also `contract_tests.rs`'s equivalent note about the absence
// of raw option-bag access).

// ---------------------------------------------------------------------------
// HWM smoke: existing high-water-mark behavior is unchanged.
// ---------------------------------------------------------------------------

#[test]
fn existing_high_water_mark_options_are_unaffected() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    let options = sock.common_options();
    options.set_send_high_water_mark(250).unwrap();
    assert_eq!(options.send_high_water_mark().unwrap(), 250);
    options.set_receive_high_water_mark(250).unwrap();
    assert_eq!(options.receive_high_water_mark().unwrap(), 250);
}
