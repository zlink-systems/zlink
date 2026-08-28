//! DEALER-DEALER multi client: one-way active sender with N dealer sockets.

#[path = "perf_common.rs"]
mod common;

use std::io::{self, BufRead, Write};
use std::time::{Duration, Instant};
use zlink::{DealerSocket, Message, RoutingId, SocketMonitor};

fn main() {
    let args = common::MultiArgs::parse();
    let settings = common::MultiSettings::from_env();

    let ctx = common::perf_client_context();
    let mut sockets: Vec<DealerSocket> = Vec::with_capacity(settings.clients);
    let mut monitors: Vec<SocketMonitor> = Vec::with_capacity(settings.clients);

    for index in 0..settings.clients {
        let sock = ctx.dealer_socket().expect("dealer");
        // C parity: numeric HWM remains behind the manual-override gate.
        common::apply_multi_hwm(&sock, &settings);
        let routing_id = RoutingId::from(format!("client_{index}").as_bytes());
        sock.set_routing_id(&routing_id).expect("routing id");
        sock.common_options()
            .set_receive_timeout(Duration::from_millis(settings.receive_timeout_ms))
            .expect("rcvtimeo");
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
    for monitor in &mut monitors {
        common::wait_monitor_ready(monitor, ready_timeout, "multi dealer-dealer client");
    }

    println!("CLIENT_READY,{}", args.msg_size);
    io::stdout().flush().ok();

    let stdin = io::stdin();
    let mut start_seen = false;
    for line in stdin.lock().lines() {
        let line = line.unwrap_or_default();
        if line.trim() == format!("START,{}", args.msg_size) {
            start_seen = true;
            break;
        }
        if matches!(line.trim(), "STOP" | "QUIT") {
            return;
        }
    }
    if !start_seen {
        return;
    }

    let deadline = Instant::now() + Duration::from_secs(settings.duration_seconds);
    let drain_deadline = deadline + common::resolve_multi_send_drain_timeout();
    let payload_size = args.msg_size.max(common::HEADER_SIZE);
    let mut sequence = 1u64;
    let mut tasks = common::ConcurrentTasks::new(sockets.len());
    while Instant::now() < deadline || (tasks.any_pending() && Instant::now() < drain_deadline) {
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
                    sequence,
                );
                sequence += 1;
                tasks.insert(slot, perf_submit_measurement_async!(socket.send(), msg));
            }
        }
        let ready = tasks.poll_ready();
        for (_, result) in &ready {
            if let Err(err) = result {
                panic!("send failed: {err}");
            }
        }
        if ready.is_empty() && tasks.any_pending() {
            let wait_deadline = if Instant::now() < deadline {
                deadline
            } else {
                drain_deadline
            };
            tasks.wait_for_wake(wait_deadline.saturating_duration_since(Instant::now()));
        }
    }
    assert!(!tasks.any_pending(), "send admission drain timed out");
    let stop_futures = sockets
        .iter()
        .map(|socket| {
            socket
                .send()
                .message(Message::try_from(common::STOP_TOKEN).expect("stop token"))
                .submit()
        })
        .collect();
    for result in common::block_on_all(stop_futures) {
        result.unwrap_or_else(|err| panic!("stop token send failed: {err}"));
    }

    println!("CLIENT_DONE,{}", args.msg_size);
    io::stdout().flush().ok();
}
