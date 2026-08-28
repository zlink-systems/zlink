//! Single DEALER/ROUTER throughput/latency benchmark.

mod common;

use std::time::Duration;
use zlink::{Message, Received, RoutingId, SocketMonitor, SubmitResult};

fn main() {
    let config = common::PerfConfig::from_env_and_args();
    let Some(bind_endpoint) = common::resolve_endpoint_or_emit_unsupported(
        "DEALER_ROUTER",
        &config.transport,
        "dealer-router",
    ) else {
        return;
    };

    let ctx = common::perf_context();
    let router = ctx.router_socket().expect("router");
    let dealer = ctx.dealer_socket().expect("dealer");
    let rid = RoutingId::from(b"perf-dealer");
    dealer.set_routing_id(&rid).expect("set rid");
    // Match C perf: numeric socket HWM remains behind the manual-override gate.
    common::apply_single_hwm(&router);
    common::apply_single_hwm(&dealer);
    dealer
        .common_options()
        .set_send_timeout(common::resolve_single_send_timeout())
        .expect("dealer send timeout");
    // PERF_SINGLE_TEST_POLICY § 1.4: receiver blocks on `recv()` until the
    // wire-level stop token arrives, so no recv timeout is needed.

    if matches!(config.transport.as_str(), "tls" | "wss") {
        let tls = common::resolve_perf_tls_paths().expect("TLS certs not found");
        common::setup_raw_tls_server(&router, &tls).expect("router tls");
        common::setup_raw_tls_client(&dealer, &tls).expect("dealer tls");
    }

    let mut router_mon = SocketMonitor::open(&router).expect("router monitor");
    let mut mon = SocketMonitor::open(&dealer).expect("monitor");
    if let Err(err) = router.bind(&bind_endpoint) {
        if common::handle_transport_setup_error("DEALER_ROUTER", &config.transport, "bind", err) {
            return;
        }
        panic!("bind: {err}");
    }
    let endpoint = router.last_endpoint().unwrap_or(bind_endpoint);
    if let Err(err) = dealer.connect(&endpoint) {
        if common::handle_transport_setup_error("DEALER_ROUTER", &config.transport, "connect", err)
        {
            return;
        }
        panic!("connect: {err}");
    }
    let ready_timeout = common::resolve_single_ready_timeout();
    common::wait_monitor_ready(&mut router_mon, ready_timeout, "dealer-router router");
    common::wait_monitor_ready(&mut mon, ready_timeout, "dealer-router dealer");
    dealer
        .send()
        .message(Message::try_from(b"PING").expect("dealer ping"))
        .submit_sync(zlink::SendFlags::NONE)
    .expect("dealer handshake send");
    let mut handshake = zlink::Received::empty();
    if let Err(err) = router.recv(&mut handshake, zlink::RecvFlags::NONE) {
        panic!("router handshake recv: {err}");
    }
    let reply_rid = handshake
        .routing_id()
        .expect("router handshake rid")
        .clone();
    assert_eq!(handshake.parts()[0].as_bytes(), b"PING");
    router
        .send(&reply_rid)
        .message(Message::try_from(b"PONG").expect("router pong"))
        .submit_sync(zlink::SendFlags::NONE)
    .expect("router handshake reply");
    let mut handshake_reply = zlink::Received::empty();
    if let Err(err) = dealer.recv(&mut handshake_reply, zlink::RecvFlags::NONE) {
        panic!("dealer handshake recv: {err}");
    }
    assert_eq!(handshake_reply.parts()[0].as_bytes(), b"PONG");

    let collector = common::MetricCollector::new();
    let stats = collector.shared();
    let active = Duration::from_secs(config.duration_seconds);
    let active_deadline = std::time::Instant::now() + active;
    let send_thread = std::thread::spawn(move || {
        common::send_loop(active_deadline, config.size, common::PHASE_ACTIVE, |msg| {
            match perf_submit_measurement!(dealer.send(), msg) {
                Ok(()) => true,
                Err(err) if err.code() == SubmitResult::NotConnected => false,
                Err(err) if common::is_single_send_retry_error(&err) => false,
                Err(err) => panic!("active send: {err}"),
            }
        });
        common::send_stop_token(|msg| {
            perf_submit_measurement!(dealer.send(), msg).map(|()| true)
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
        match router.recv(&mut received, flags) {
            Ok(true) => {
                debug_assert_eq!(received.parts().len(), 1);
                let data = received.first_part().expect("router payload").as_bytes();
                if common::is_stop_token(data) {
                    break;
                }
                common::handle_recv(data, config.size, &stats, active_deadline);
                while router
                    .recv(&mut received, zlink::RecvFlags::DONT_WAIT)
                    .expect("dealer-router router recv failed")
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
            Err(err) => panic!("dealer-router router recv failed: {err}"),
        }
    }
    send_thread.join().expect("sender thread");

    let result = collector.finish();
    common::print_result(
        "DEALER_ROUTER",
        &config.transport,
        config.size,
        config.duration_seconds,
        &result,
    );
}
