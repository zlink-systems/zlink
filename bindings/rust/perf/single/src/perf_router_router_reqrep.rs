//! Single ROUTER/ROUTER request/reply throughput/latency benchmark.

mod common;

use zlink::{Message, Received, RoutingId, SocketMonitor};

fn main() {
    let config = common::PerfConfig::from_env_and_args();
    let pattern = "ROUTER_ROUTER_REQREP";
    let Some(bind_endpoint) = common::resolve_endpoint_or_emit_unsupported(
        pattern,
        &config.transport,
        "router-router-reqrep",
    ) else {
        return;
    };

    let ctx = common::perf_context();
    let replier = ctx.router_socket().expect("replier");
    let requester = ctx.router_socket().expect("requester");
    let replier_rid = RoutingId::from(b"SERVER");
    let requester_rid = RoutingId::from(b"CLIENT");
    replier
        .set_routing_id(&replier_rid)
        .expect("set replier routing id");
    requester
        .set_routing_id(&requester_rid)
        .expect("set requester routing id");
    replier
        .router_options()
        .set_mandatory(true)
        .expect("replier mandatory");
    requester
        .router_options()
        .set_mandatory(true)
        .expect("requester mandatory");
    requester
        .router_options()
        .set_connect_routing_id(&replier_rid)
        .expect("connect routing id");
    common::apply_single_hwm(&replier);
    common::apply_single_hwm(&requester);
    requester
        .common_options()
        .set_send_timeout(common::resolve_single_send_timeout())
        .expect("requester send timeout");

    if matches!(config.transport.as_str(), "tls" | "wss") {
        let tls = common::resolve_perf_tls_paths().expect("TLS certs not found");
        common::setup_raw_tls_server(&replier, &tls).expect("replier tls");
        common::setup_raw_tls_client(&requester, &tls).expect("requester tls");
    }

    let mut replier_monitor = SocketMonitor::open(&replier).expect("replier monitor");
    let mut requester_monitor = SocketMonitor::open(&requester).expect("requester monitor");
    if let Err(error) = replier.bind(&bind_endpoint) {
        if common::handle_transport_setup_error(pattern, &config.transport, "bind", error) {
            return;
        }
        panic!("bind: {error}");
    }
    let endpoint = replier.last_endpoint().unwrap_or(bind_endpoint);
    if let Err(error) = requester.connect(&endpoint) {
        if common::handle_transport_setup_error(pattern, &config.transport, "connect", error) {
            return;
        }
        panic!("connect: {error}");
    }
    let ready_timeout = common::resolve_single_ready_timeout();
    common::wait_monitor_ready(&mut replier_monitor, ready_timeout, "reqrep replier");
    common::wait_monitor_ready(&mut requester_monitor, ready_timeout, "reqrep requester");

    requester
        .send(&replier_rid)
        .message(Message::try_from(b"PING").expect("handshake ping"))
        .submit_sync()
        .expect("handshake send");
    let mut handshake = Received::empty();
    replier
        .recv(&mut handshake, zlink::RecvFlags::NONE)
        .expect("handshake receive");
    assert_eq!(handshake.parts()[0].as_bytes(), b"PING");
    handshake
        .send()
        .message(Message::try_from(b"PONG").expect("handshake pong"))
        .submit_sync()
        .expect("handshake reply");
    let mut handshake_reply = Received::empty();
    requester
        .recv(&mut handshake_reply, zlink::RecvFlags::NONE)
        .expect("handshake reply receive");
    assert_eq!(handshake_reply.parts()[0].as_bytes(), b"PONG");

    let target = replier_rid;
    let replier_thread = std::thread::spawn(move || common::run_router_replier(replier));
    let stats = common::run_reqrep(&config, |payload, timeout| {
        let request = requester.request(&target).message(payload);
        if common::measurement_part_count() == 2 {
            request
                .message(Message::new().expect("empty request tail"))
                .timeout(timeout)
                .submit_sync()
        } else {
            request.timeout(timeout).submit_sync()
        }
    })
    .expect("requester loop");

    common::send_stop_token(|message| {
        requester
            .send(&target)
            .message(message)
            .submit_sync()
            .map(|()| true)
    });
    replier_thread
        .join()
        .expect("replier thread")
        .expect("replier loop");

    common::print_reqrep_result(
        pattern,
        &config.transport,
        config.size,
        config.duration_seconds,
        &stats,
    );
}
