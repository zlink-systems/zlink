#[path = "perf_common.rs"]
mod common;

use std::time::{Duration, Instant};
use zlink::{
    DealerSocket, Message, PollEvent, Poller, RecvFlags, RoutingId, SendFlags, SocketMonitor,
    POLLIN, POLLOUT,
};

fn drain_socket(
    index: usize,
    socket: &DealerSocket,
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
    common::apply_multi_auto_hwm_msg_unit(&ctx, args.msg_size);
    let mut sockets: Vec<DealerSocket> = Vec::with_capacity(settings.clients);
    let payload_size = args.msg_size.max(common::HEADER_SIZE);
    let mut waiting_reply = vec![false; settings.clients];
    let mut send_pending = vec![false; settings.clients];
    let mut seqs = vec![1u64; settings.clients];
    let mut monitors: Vec<SocketMonitor> = Vec::with_capacity(settings.clients);

    for index in 0..settings.clients {
        let sock = ctx.dealer_socket().expect("dealer");
        // C parity: numeric HWM remains behind the manual-override gate.
        common::apply_multi_hwm(&sock, &settings);
        sock.common_options()
            .set_send_timeout(Duration::from_millis(settings.send_timeout_ms))
            .expect("send timeout");
        sock.common_options()
            .set_receive_timeout(Duration::from_millis(settings.receive_timeout_ms))
            .expect("recv timeout");
        let rid = RoutingId::from(format!("CLIENT-{index}").as_bytes());
        sock.set_routing_id(&rid).expect("set rid");
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
        common::wait_monitor_ready(mon, ready_timeout, "multi dealer-router client");
    }

    // C perf_multi_client_helpers.hpp run_echo loop: unified poller, signal-
    // driven perf_socket_poll(...,-1) when no socket made progress; inflight==1
    // per socket via waiting_reply/send_pending. No hot-loop sleep(1ms).
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
            match sockets[index]
                .send()
                .message(msg)
                .flags(SendFlags::DONT_WAIT)
                .submit()
            {
                Ok(true) => {
                    waiting_reply[index] = true;
                    send_pending[index] = false;
                    seqs[index] += 1;
                    progressed = true;
                }
                Ok(false) => {
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
        "MULTI_DEALER_ROUTER",
        &args.transport,
        args.msg_size,
        settings.duration_seconds,
        &latency.finish(),
    );
}
