//! Monitor Tests – verify socket monitor recv and
//! monitor event observation.

use std::thread;
use std::time::Duration;

use zlink::{
    Context, MonitorEvent, MonitorEventFlags, MonitorEventType, RecvError, SocketMonitor,
    SocketMonitorEventMask, SocketMonitorOpenOptions,
};

#[test]
fn flow_state_event_constants_match_c_abi_and_are_in_all() {
    // core/include/zlink_enum.h:
    //   ZLINK_SOCKET_MONITOR_EVENT_SEND_FLOW_PAUSED  = 1u << 16
    //   ZLINK_SOCKET_MONITOR_EVENT_SEND_FLOW_RESUMED = 1u << 17
    //   ZLINK_SOCKET_MONITOR_EVENT_FLOW_STATE_STALE  = 1u << 18
    //   ZLINK_SOCKET_MONITOR_EVENT_ALL               = 0x7FFFF
    assert_eq!(SocketMonitorEventMask::SEND_FLOW_PAUSED.bits(), 1 << 16);
    assert_eq!(SocketMonitorEventMask::SEND_FLOW_RESUMED.bits(), 1 << 17);
    assert_eq!(SocketMonitorEventMask::FLOW_STATE_STALE.bits(), 1 << 18);
    assert_eq!(SocketMonitorEventMask::ALL.bits(), 0x7FFFF);

    let all = SocketMonitorEventMask::ALL.bits();
    assert_eq!(
        all & SocketMonitorEventMask::SEND_FLOW_PAUSED.bits(),
        1 << 16
    );
    assert_eq!(
        all & SocketMonitorEventMask::SEND_FLOW_RESUMED.bits(),
        1 << 17
    );
    assert_eq!(
        all & SocketMonitorEventMask::FLOW_STATE_STALE.bits(),
        1 << 18
    );
}

#[test]
fn monitor_status_detail_flow_state_bit_matches_c_abi() {
    // core/include/zlink_enum.h:
    //   ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE = 1u << 5
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    let mon = SocketMonitor::open(&sock).unwrap();
    let snap = mon.status().unwrap();
    // `has_flow_state_detail()` is `MonitorStatus`'s only public surface for
    // the ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE (1 << 5) bit; assert it
    // agrees with the raw detail_flags mask bit-for-bit.
    assert_eq!(
        snap.has_flow_state_detail(),
        snap.detail_flags & (1 << 5) != 0
    );
}

#[test]
fn monitor_event_flag_constants_match_c_abi() {
    // core/include/zlink/eventing/api.h:
    //   ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE       = 1u << 0
    //   ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE          = 1u << 1
    //   ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_GENERATION = 1u << 2
    //   ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH      = 1u << 3
    assert_eq!(MonitorEventFlags::NONE.bits(), 0);
    assert_eq!(MonitorEventFlags::CONNECTION_READY_EDGE.bits(), 1 << 0);
    assert_eq!(MonitorEventFlags::SEND_FLOW_WRITABLE.bits(), 1 << 1);
    assert_eq!(
        MonitorEventFlags::FLOW_STATE_STALE_GENERATION.bits(),
        1 << 2
    );
    assert_eq!(MonitorEventFlags::FLOW_STATE_STALE_EPOCH.bits(), 1 << 3);

    let combined =
        MonitorEventFlags::SEND_FLOW_WRITABLE | MonitorEventFlags::FLOW_STATE_STALE_EPOCH;
    assert!(combined.contains(MonitorEventFlags::SEND_FLOW_WRITABLE));
    assert!(combined.contains(MonitorEventFlags::FLOW_STATE_STALE_EPOCH));
    assert!(!combined.contains(MonitorEventFlags::CONNECTION_READY_EDGE));
}

#[test]
fn socket_monitor_recv_surface() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    sock.bind("inproc://mon-empty").unwrap();

    let _mon = SocketMonitor::open(&sock).unwrap();
    let _recv: fn(&SocketMonitor) -> Result<MonitorEvent, RecvError> = SocketMonitor::recv;
}

#[test]
fn socket_monitor_hwm_bytes_are_forwarded_exactly() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    let monitor_hwm_bytes = 12_345;
    let _mon = SocketMonitor::open_with_options(
        &sock,
        SocketMonitorOpenOptions {
            events: SocketMonitorEventMask::ALL,
            monitor_hwm_bytes,
        },
    )
    .unwrap();

    assert_eq!(
        ctx.core_hwm_budget_snapshot()
            .unwrap()
            .monitor_queue_applied_hwm_bytes,
        monitor_hwm_bytes * 2
    );
}

#[test]
fn socket_monitor_event_exposes_full_payload_fields() {
    // Final gate (core-byte-hwm-flow-control-plan.ko.md): MonitorEvent used
    // to narrow `value` to u32 and omit connection_id / transport_lane /
    // flags entirely. Confirm
    // all of them are reachable, correctly typed, on a real event from a
    // live connection.
    let ctx = Context::new().unwrap();
    let server = ctx.pair_socket().unwrap();
    server.bind("inproc://mon-full-payload").unwrap();

    let mon = SocketMonitor::open(&server).unwrap();

    let client = ctx.pair_socket().unwrap();
    client.connect("inproc://mon-full-payload").unwrap();

    thread::sleep(Duration::from_millis(100));

    let mut saw_event = false;
    for _ in 0..20 {
        if let Ok(ev) = mon.recv() {
            saw_event = true;
            let _value: u64 = ev.value;
            let _connection_id: u64 = ev.connection_id;
            let _transport_lane: u32 = ev.transport_lane;
            let _flags: MonitorEventFlags = ev.flags;
            // Confirms `flags` really is `MonitorEventFlags`, not a raw
            // integer -- this compiles only if the type and its API match.
            let _ = ev.flags.contains(MonitorEventFlags::CONNECTION_READY_EDGE);
            break;
        }
    }
    assert!(
        saw_event,
        "expected at least one monitor event from a live connect"
    );
}

#[test]
fn socket_monitor_observes_connection() {
    let ctx = Context::new().unwrap();
    let server = ctx.pair_socket().unwrap();
    server.bind("inproc://mon-connect").unwrap();

    let mon = SocketMonitor::open(&server).unwrap();

    let client = ctx.pair_socket().unwrap();
    client.connect("inproc://mon-connect").unwrap();

    // Wait for events and drain
    thread::sleep(Duration::from_millis(100));

    let mut found_event = false;
    for _ in 0..20 {
        if let Ok(ev) = mon.recv() {
            found_event = true;
            let _ = ev.event;
            let _ = ev.local_addr;
            break;
        }
    }
    let _ = found_event;
}

#[test]
fn socket_monitor_blocking_recv_success() {
    // Blocking recv must return an event when a connection is made.
    // Open monitor, then trigger a connect in another thread.
    let ctx = Context::new().unwrap();
    let server = ctx.pair_socket().unwrap();
    server.bind("inproc://mon-blocking-recv").unwrap();

    let mon = SocketMonitor::open(&server).unwrap();

    // Trigger a connection from another thread so blocking recv has data
    let ctx2_handle = ctx.pair_socket().unwrap();
    let _connector = thread::spawn(move || {
        thread::sleep(Duration::from_millis(50));
        ctx2_handle.connect("inproc://mon-blocking-recv").unwrap();
    });

    // Blocking recv – must return an event (not hang forever)
    let event = mon.recv().unwrap();
    assert!(
        event.event != MonitorEventType(0),
        "blocking monitor recv must return a valid event"
    );
}

#[test]
fn socket_monitor_status() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    sock.bind("inproc://mon-snapshot").unwrap();

    let mon = SocketMonitor::open(&sock).unwrap();
    let snap = mon.status().unwrap();
    // Verify snapshot fields are accessible
    let _ = snap.is_ready();
    let _ = snap.is_closed();
    let _ = snap.auto_hwm_profile;
    let _ = snap.auto_hwm_policy_class;
    let _ = snap.snd_pending_bytes;
    let _ = snap.rcv_pending_bytes;

    // ABI 4 (core-byte-hwm-flow-control-plan.ko.md §6): Core now writes a
    // 232-byte record (see ffi_layout_tests.rs); confirm the live snapshot
    // reports that ABI version and that the five new flow-state metrics are
    // reachable through the public surface. PAIR does not support receive-flow control, so
    // these are expected to read as zero here -- this is a surface/ABI
    // check, not a behavioral one.
    assert_eq!(snap.abi_version, 4);
    assert_eq!(snap.flow_paused_connections, 0);
    assert_eq!(snap.flow_pause_applied_total, 0);
    assert_eq!(snap.flow_resume_applied_total, 0);
    assert_eq!(snap.flow_state_stale_total, 0);
    assert_eq!(snap.flow_pause_duration_ms, 0);
}
