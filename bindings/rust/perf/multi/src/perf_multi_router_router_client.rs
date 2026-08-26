#[path = "perf_common.rs"]
mod common;

use std::time::{Duration, Instant};
use zlink::{
    Message, POLLIN, POLLOUT, PollEvent, Poller, RecvFlags, RouterSocket, RoutingId, SocketMonitor,
    SubmitResult,
};

fn drain_socket(
    index: usize,
    socket: &RouterSocket,
    msg_size: usize,
    latency: &mut common::LatencyStats,
    waiting_reply: &mut [bool],
) -> bool {
    let mut processed = false;
    let mut received = zlink::Received::empty();
    loop {
        match socket.recv(&mut received, RecvFlags::DONT_WAIT) {
            Ok(true) => {
                let data = common::message_payload(received.parts());
                if !common::is_valid_active_message(data, msg_size) {
                    continue;
                }
                let sent_ts_ns = common::decode_sent_ts_ns(data);
                let latency_ns =
                    common::now_ns().saturating_sub(sent_ts_ns.max(0) as u64) as f64 / 2.0;
                latency.record_ns(latency_ns);
                waiting_reply[index] = false;
                processed = true;
            }
            Ok(false) => break,
            Err(err) => panic!("recv failed: {err}"),
        }
    }
    processed
}

fn main() {
    let args = common::MultiArgs::parse();
    let settings = common::MultiSettings::from_env();

    let ctx = common::perf_client_context();
    let server_rid = RoutingId::from(b"perf-rr-server");
    let mut sockets: Vec<RouterSocket> = Vec::with_capacity(settings.clients);
    let payload_size = args.msg_size.max(common::HEADER_SIZE);
    let mut waiting_reply = vec![false; settings.clients];
    let mut send_pending = vec![false; settings.clients];
    let mut seqs = vec![1u64; settings.clients];
    let mut monitors: Vec<SocketMonitor> = Vec::with_capacity(settings.clients);

    for index in 0..settings.clients {
        let sock = ctx.router_socket().expect("router");
        sock.common_options()
            .set_send_high_water_mark(settings.send_high_water_mark)
            .expect("sndhwm");
        sock.common_options()
            .set_receive_high_water_mark(settings.receive_high_water_mark)
            .expect("rcvhwm");
        sock.common_options()
            .set_receive_timeout(Duration::from_millis(1))
            .expect("recv timeout");
        let rid = RoutingId::from(format!("CLIENT-{index}").as_bytes());
        sock.set_routing_id(&rid).expect("set rid");
        sock.router_options()
            .set_connect_routing_id(&server_rid)
            .expect("connect rid");
        if matches!(args.transport.as_str(), "tls" | "wss") {
            let tls = common::resolve_perf_tls_paths().expect("TLS certs not found");
            common::setup_raw_tls_client(&sock, &tls).expect("client tls");
        }
        let mon = SocketMonitor::open(&sock).expect("monitor");
        sock.connect(&args.endpoint).expect("connect");
        sockets.push(sock);
        monitors.push(mon);
    }

    let ready_timeout = common::resolve_multi_connect_ready_timeout();
    for mon in &mut monitors {
        common::wait_monitor_ready(mon, ready_timeout, "multi router-router client");
    }

    // Match the dealer-router client: unified poller with signal-driven
    // perf_socket_poll(...,-1) when no socket made progress. The previous
    // hot-loop thread::sleep(1ms) throttled small-message throughput to
    // ~18-21% of C; a poller-driven wait removes that fixed-latency floor.
    let poller = Poller::new().expect("poller");
    for (index, sock) in sockets.iter().enumerate() {
        poller
            .add_socket(sock, POLLIN | POLLOUT, index)
            .expect("poller add");
    }
    let mut poll_events = vec![PollEvent::default(); sockets.len().max(1)];

    let mut latency = common::LatencyStats::new();
    let deadline = Instant::now() + Duration::from_secs(settings.duration_seconds);
    while Instant::now() < deadline {
        let mut progressed = false;
        for index in 0..sockets.len() {
            progressed |= drain_socket(
                index,
                &sockets[index],
                args.msg_size,
                &mut latency,
                &mut waiting_reply,
            );
            if waiting_reply[index] {
                continue;
            }
            let mut msg = Message::with_size(payload_size).expect("msg");
            common::encode_header(
                msg.data_mut(),
                common::PHASE_ACTIVE,
                args.msg_size as u32,
                seqs[index],
            );
            match perf_submit_measurement!(sockets[index].send(&server_rid), msg) {
                Ok(()) => {
                    waiting_reply[index] = true;
                    send_pending[index] = false;
                    seqs[index] += 1;
                    progressed = true;
                }
                Err(err) if err.code() == SubmitResult::Backpressured => {
                    send_pending[index] = true;
                }
                Err(err) => panic!("send failed: {err}"),
            }
        }
        if progressed {
            continue;
        }
        if Instant::now() >= deadline {
            break;
        }
        let remaining_ms = deadline
            .saturating_duration_since(Instant::now())
            .as_millis()
            .max(1) as i64;
        match poller.wait(&mut poll_events, remaining_ms) {
            Ok(_) => {}
            Err(err) => panic!("poller wait failed: {err}"),
        }
    }

    common::print_result(
        "MULTI_ROUTER_ROUTER",
        &args.transport,
        args.msg_size,
        settings.duration_seconds,
        &latency.finish(),
    );
}
