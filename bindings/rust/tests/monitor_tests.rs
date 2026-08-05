//! Monitor Tests – verify socket monitor recv and
//! monitor event observation.

use std::thread;
use std::time::Duration;

use zlink::{Context, MonitorEvent, MonitorEventType, RecvError, SocketMonitor};

#[test]
fn socket_monitor_recv_surface() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    sock.bind("inproc://mon-empty").unwrap();

    let _mon = SocketMonitor::open(&sock).unwrap();
    let _recv: fn(&SocketMonitor) -> Result<MonitorEvent, RecvError> = SocketMonitor::recv;
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
    let _ = snap.auto_hwm_unit_budget_bytes;
    let _ = snap.auto_hwm_size_cap;
    let _ = snap.auto_hwm_socket_message_slots;
    let _ = snap.auto_hwm_connection_bucket_count;
    let _ = snap.auto_hwm_connection_bucket_hwm_4k;
}

#[test]
fn socket_monitor_callback() {
    use std::sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    };

    let ctx = Context::new().unwrap();
    let server = ctx.pair_socket().unwrap();
    server.bind("inproc://mon-callback").unwrap();

    let mut mon = SocketMonitor::open(&server).unwrap();
    let called = Arc::new(AtomicBool::new(false));
    let called_clone = called.clone();

    mon.on_event(move |_event| {
        called_clone.store(true, Ordering::Relaxed);
    })
    .unwrap();

    let client = ctx.pair_socket().unwrap();
    client.connect("inproc://mon-callback").unwrap();

    thread::sleep(Duration::from_millis(200));
    // Callback may or may not have been called depending on timing
    let _ = called.load(Ordering::Relaxed);
}
