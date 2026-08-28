#[path = "perf_common.rs"]
mod common;

use std::time::{Duration, Instant};
use zlink::{
    DealerSocket, Message, POLLCOMPLETION, POLLIN, PollEvent, Poller, RecvFlags, RecvResult,
    RoutingId, SocketMonitor,
};

fn drain_socket(
    socket: &DealerSocket,
    msg_size: usize,
    deadline: Instant,
    latency: &mut common::LatencyStats,
) -> bool {
    let mut processed = false;
    let mut received = zlink::Received::empty();
    loop {
        if Instant::now() >= deadline {
            break;
        }
        match socket.recv(&mut received, RecvFlags::DONT_WAIT) {
            Ok(true) => {
                if Instant::now() >= deadline {
                    break;
                }
                let data = common::message_payload(received.parts());
                if common::record_active_rtt_latency(data, msg_size, latency) {
                    processed = true;
                }
            }
            Ok(false) => break,
            Err(err) if err.code() == RecvResult::NoData => break,
            Err(err) => panic!("recv failed: {err}"),
        }
    }
    processed
}

fn main() {
    let args = common::MultiArgs::parse();
    let settings = common::MultiSettings::from_env();

    let ctx = common::perf_client_context();
    let mut sockets: Vec<DealerSocket> = Vec::with_capacity(settings.clients);
    let payload_size = args.msg_size.max(common::HEADER_SIZE);
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
        let mon = common::open_connection_ready_monitor(&sock);
        sock.connect(&args.endpoint).expect("connect");
        sockets.push(sock);
        monitors.push(mon);
    }

    let ready_timeout = common::resolve_multi_connect_ready_timeout();
    for mon in &mut monitors {
        common::wait_monitor_ready(mon, ready_timeout, "multi dealer-router client");
    }

    // Receive and Future-based admission run concurrently. Echo receipt never
    // gates the next send; each socket starts its next send after admission.
    let poller = Poller::new().expect("poller");
    for (index, sock) in sockets.iter().enumerate() {
        poller
            .add_socket(sock, POLLIN | POLLCOMPLETION, index)
            .expect("poller add");
    }
    let mut poll_events = vec![PollEvent::default(); sockets.len().max(1)];

    let mut latency = common::LatencyStats::new();
    let deadline = Instant::now() + Duration::from_secs(settings.duration_seconds);
    let drain_deadline = deadline + common::resolve_multi_send_drain_timeout();
    let mut tasks = common::ConcurrentTasks::new(sockets.len());
    let mut seqs = vec![1u64; sockets.len()];
    while Instant::now() < deadline || (tasks.any_pending() && Instant::now() < drain_deadline) {
        let mut inserted = vec![false; sockets.len()];
        if Instant::now() < deadline {
            for (slot, socket) in sockets.iter().enumerate() {
                if tasks.is_pending(slot) {
                    continue;
                }
                let mut msg = Message::with_size(payload_size).expect("msg");
                common::encode_header(
                    msg.data_mut(),
                    common::PHASE_ACTIVE,
                    args.msg_size as u32,
                    seqs[slot],
                );
                seqs[slot] += 1;
                tasks.insert(slot, perf_submit_measurement_async!(socket.send(), msg));
                inserted[slot] = true;
            }
        }
        let ready = tasks.poll_ready();
        let mut progressed = !ready.is_empty();
        for (_, result) in ready {
            result.unwrap_or_else(|err| panic!("send failed: {err}"));
        }
        progressed |= inserted
            .iter()
            .enumerate()
            .any(|(slot, was_inserted)| *was_inserted && tasks.is_pending(slot));
        if Instant::now() >= deadline && !tasks.any_pending() {
            break;
        }
        let wait_deadline = if Instant::now() < deadline {
            deadline
        } else {
            drain_deadline
        };
        let wait_ms = if progressed {
            0
        } else {
            common::poll_timeout_until(wait_deadline)
        };
        match poller.wait(&mut poll_events, wait_ms) {
            Ok(event_count) => {
                for event in &poll_events[..event_count] {
                    if event.slot < sockets.len() && event.revents & POLLIN != 0 {
                        drain_socket(&sockets[event.slot], args.msg_size, deadline, &mut latency);
                    }
                }
            }
            Err(err) => panic!("poller wait failed: {err}"),
        }
    }
    assert!(!tasks.any_pending(), "send admission drain timed out");

    common::print_result(
        "MULTI_DEALER_ROUTER_SENDSEND",
        &args.transport,
        args.msg_size,
        settings.duration_seconds,
        &latency.finish(),
    );
}
