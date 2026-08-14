//! Single ROUTER/ROUTER throughput/latency benchmark.

mod common;

use std::time::Duration;
use zlink::{Message, Received, RoutingId, SocketMonitor, SubmitResult};

fn main() {
    let config = common::PerfConfig::from_env_and_args();
    let Some(bind_endpoint) = common::resolve_endpoint_or_emit_unsupported(
        "ROUTER_ROUTER",
        &config.transport,
        "router-router",
    ) else {
        return;
    };

    let ctx = common::perf_context();
    let receiver = ctx.router_socket().expect("receiver");
    let sender = ctx.router_socket().expect("sender");
    // Match C perf: numeric socket HWM remains behind the manual-override gate.
    common::apply_single_hwm(&receiver);
    common::apply_single_hwm(&sender);
    sender
        .common_options()
        .set_send_timeout(common::resolve_single_send_timeout())
        .expect("sender send timeout");
    // PERF_SINGLE_TEST_POLICY § 1.4: receiver blocks on `recv()` until the
    // wire-level stop token arrives, so no recv timeout is needed.

    let sender_rid = RoutingId::from(b"perf-rr-sender");
    sender.set_routing_id(&sender_rid).expect("set rid");
    let receiver_rid = RoutingId::from(b"perf-rr-receiver");
    receiver.set_routing_id(&receiver_rid).expect("set rid");
    receiver
        .router_options()
        .set_mandatory(true)
        .expect("receiver mandatory");
    sender
        .router_options()
        .set_mandatory(true)
        .expect("sender mandatory");
    sender
        .router_options()
        .set_connect_routing_id(&receiver_rid)
        .expect("connect rid");

    if matches!(config.transport.as_str(), "tls" | "wss") {
        let tls = common::resolve_perf_tls_paths().expect("TLS certs not found");
        common::setup_raw_tls_server(&receiver, &tls).expect("receiver tls");
        common::setup_raw_tls_client(&sender, &tls).expect("sender tls");
    }

    let mut receiver_mon = SocketMonitor::open(&receiver).expect("receiver monitor");
    let mut mon = SocketMonitor::open(&sender).expect("monitor");
    if let Err(err) = receiver.bind(&bind_endpoint) {
        if common::handle_transport_setup_error("ROUTER_ROUTER", &config.transport, "bind", err) {
            return;
        }
        panic!("bind: {err}");
    }
    let endpoint = receiver.last_endpoint().unwrap_or(bind_endpoint);
    if let Err(err) = sender.connect(&endpoint) {
        if common::handle_transport_setup_error("ROUTER_ROUTER", &config.transport, "connect", err)
        {
            return;
        }
        panic!("connect: {err}");
    }
    let ready_timeout = common::resolve_single_ready_timeout();
    let target = receiver_rid.clone();
    common::wait_monitor_ready(&mut receiver_mon, ready_timeout, "router-router receiver");
    common::wait_monitor_ready(&mut mon, ready_timeout, "router-router sender");
    common::block_on(
        sender
            .send(&target)
            .message(Message::try_from(b"PING").expect("router ping"))
            .submit(),
    )
    .expect("router handshake send");
    let mut handshake = zlink::Received::empty();
    if let Err(err) = receiver.recv(&mut handshake, zlink::RecvFlags::NONE) {
        panic!("receiver handshake recv: {err}");
    }
    let reply_rid = handshake
        .routing_id()
        .expect("receiver handshake rid")
        .clone();
    assert_eq!(handshake.parts()[0].as_bytes(), b"PING");
    common::block_on(
        receiver
            .send(&reply_rid)
            .message(Message::try_from(b"PONG").expect("router pong"))
            .submit(),
    )
    .expect("receiver handshake reply");
    let mut handshake_reply = zlink::Received::empty();
    if let Err(err) = sender.recv(&mut handshake_reply, zlink::RecvFlags::NONE) {
        panic!("sender handshake recv: {err}");
    }
    assert_eq!(handshake_reply.parts()[0].as_bytes(), b"PONG");

    let collector = common::MetricCollector::new();
    let stats = collector.shared();

    let active = Duration::from_secs(config.duration_seconds);
    let active_deadline = std::time::Instant::now() + active;
    let send_target = target.clone();
    let send_thread = std::thread::spawn(move || {
        common::send_loop(active_deadline, config.size, common::PHASE_ACTIVE, |msg| {
            match common::block_on(sender.send(&send_target).message(msg).submit()) {
                Ok(()) => true,
                Err(err) if err.code() == SubmitResult::NotConnected => false,
                Err(err) if common::is_single_send_retry_error(&err) => false,
                Err(err) => panic!("active send: {err}"),
            }
        });
        common::send_stop_token(|msg| {
            common::block_on(sender.send(&send_target).message(msg).submit()).map(|()| true)
        });
    });

    let mut received = Received::empty();
    let stop_wait_deadline = active_deadline + common::resolve_single_stop_wait();
    'recv: loop {
        if std::time::Instant::now() >= stop_wait_deadline {
            break;
        }
        let flags = if std::time::Instant::now() < active_deadline {
            zlink::RecvFlags::NONE
        } else {
            zlink::RecvFlags::DONT_WAIT
        };
        match receiver.recv(&mut received, flags) {
            Ok(true) => {
                debug_assert_eq!(received.parts().len(), 1);
                let data = received.first_part().expect("router payload").as_bytes();
                if common::is_stop_token(data) {
                    break;
                }
                common::handle_recv(data, config.size, &stats, active_deadline);
                while receiver
                    .recv(&mut received, zlink::RecvFlags::DONT_WAIT)
                    .expect("router-router receiver recv failed")
                {
                    debug_assert_eq!(received.parts().len(), 1);
                    let data = received.first_part().expect("router payload").as_bytes();
                    if common::is_stop_token(data) {
                        break 'recv;
                    }
                    common::handle_recv(data, config.size, &stats, active_deadline);
                }
            }
            Ok(false) => common::poll_idle(Duration::from_millis(1)),
            Err(err) => panic!("router-router receiver recv failed: {err}"),
        }
    }
    send_thread.join().expect("sender thread");

    let result = collector.finish();
    common::print_result(
        "ROUTER_ROUTER",
        &config.transport,
        config.size,
        config.duration_seconds,
        &result,
    );
}
