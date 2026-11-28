#[path = "perf_common.rs"]
mod common;

use zlink::{
    DealerSocket, POLLCOMPLETION, POLLIN, Poller, RecvFlags, RecvResult, RoutingId, SocketMonitor,
};

fn main() {
    let args = common::MultiArgs::parse();
    let settings = common::MultiSettings::from_env();

    let ctx = common::perf_client_context();
    let mut sockets: Vec<DealerSocket> = Vec::with_capacity(settings.clients);
    let mut monitors: Vec<SocketMonitor> = Vec::with_capacity(settings.clients);

    for index in 0..settings.clients {
        let sock = ctx.create_dealer_socket().expect("dealer");
        // C parity: numeric HWM remains behind the manual-override gate.
        common::apply_multi_hwm(&sock, &settings);
        sock.common_options()
            .set_send_timeout(common::timeout_ms(settings.send_timeout_ms))
            .expect("send timeout");
        sock.common_options()
            .set_receive_timeout(common::timeout_ms(settings.receive_timeout_ms))
            .expect("recv timeout");
        let rid = RoutingId::from(format!("CLIENT-{index}").as_bytes());
        sock.set_routing_id(&rid).expect("set rid");
        if matches!(args.transport.as_str(), "tls" | "wss") {
            let tls = common::resolve_perf_tls_paths().expect("TLS certs not found");
            common::setup_raw_tls_client(&sock, &tls).expect("client tls");
        }
        let mon = common::open_connection_ready_monitor(&sock);
        sock.connect(&args.endpoint).expect("connect");
        sockets.push(sock);
        monitors.push(mon);
    }

    let ready_timeout = common::resolve_multi_connect_ready_timeout();
    for mon in &mut monitors {
        common::wait_monitor_ready(mon, ready_timeout, "multi dealer-router client");
    }
    ctx.recalculate_auto_hwm().expect("recalculate auto hwm");

    let poller = Poller::new().expect("poller");
    for (index, socket) in sockets.iter().enumerate() {
        poller
            .add_socket(socket, POLLIN | POLLCOMPLETION, index)
            .expect("poller add");
    }
    let submit = |slot: usize, message| {
        perf_submit_measurement_async!(sockets[slot].send(), message).expect("send")
    };
    let receive = |slot: usize, received: &mut zlink::Received| match sockets[slot]
        .recv(received, RecvFlags::DONT_WAIT)
    {
        Ok(received) => received,
        Err(err) if err.code() == RecvResult::NoData => false,
        Err(err) => panic!("recv failed: {err}"),
    };
    let active_seconds = settings.duration_seconds;
    let latency_seconds = common::latency_seconds(active_seconds);
    let drain_ns = (common::resolve_multi_send_drain_timeout().as_nanos() as u64)
        .max(active_seconds * 3 * common::SECOND_NS);
    let mut active = common::LatencyStats::new();
    let (active_sends, _) = common::run_send_phase(
        &args,
        &poller,
        sockets.len(),
        drain_ns,
        common::SendPhase {
            echo: true,
            end: common::now_ns() + active_seconds * common::SECOND_NS,
            schedule: None,
        },
        submit,
        receive,
        &mut active,
    );
    let active_received = active.finish().count;
    assert!(active_received > 0, "no active replies");
    let mut paced = common::LatencyStats::new();
    let mut schedule = common::Schedule::new(common::now_ns(), active_received, active_seconds);
    let end = schedule.epoch + common::SETTLE_NS + latency_seconds * common::SECOND_NS;
    let (_, paced_received) = common::run_send_phase(
        &args,
        &poller,
        sockets.len(),
        drain_ns,
        common::SendPhase {
            echo: true,
            end,
            schedule: Some(&mut schedule),
        },
        submit,
        receive,
        &mut paced,
    );
    let mut result = paced.finish();
    schedule.print(
        "DEALER_ROUTER_SENDSEND",
        args.msg_size,
        (active_sends, active_received, active_seconds),
        paced_received,
        latency_seconds,
        None,
    );
    result.count = active_received;

    if let Some(socket) = sockets.first() {
        common::print_multi_auto_hwm_detail(
            socket,
            "endpoint",
            &args.transport,
            args.msg_size,
            "dealer",
        );
    }

    common::print_result(
        "DEALER_ROUTER_SENDSEND",
        &args.transport,
        args.msg_size,
        settings.duration_seconds,
        &result,
    );
}
