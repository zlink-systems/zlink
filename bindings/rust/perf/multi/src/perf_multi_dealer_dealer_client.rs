//! DEALER-DEALER multi client: one-way active sender with N dealer sockets.

#[path = "perf_common.rs"]
mod common;

use std::io::{self, BufRead, Write};
use std::time::{Duration, Instant};
use zlink::{
    DealerSocket, Message, POLLOUT, PollEvent, Poller, RoutingId, SocketMonitor, SubmitResult,
};

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
        let mon = SocketMonitor::open(&sock).expect("monitor");
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

    let poller = Poller::new().expect("poller");
    for (index, socket) in sockets.iter().enumerate() {
        poller
            .add_socket(socket, POLLOUT, index)
            .expect("poller add");
    }
    let mut poll_events = vec![PollEvent::default(); sockets.len().max(1)];

    let deadline = Instant::now() + Duration::from_secs(settings.duration_seconds);
    let payload_size = args.msg_size.max(common::HEADER_SIZE);
    let mut send_pending = vec![false; sockets.len()];
    let mut seq: u64 = 1;
    let burst_until_blocked = args.msg_size <= 1024;
    while Instant::now() < deadline {
        let mut progressed = false;
        for (index, socket) in sockets.iter().enumerate() {
            if burst_until_blocked && send_pending[index] {
                continue;
            }
            if Instant::now() >= deadline {
                break;
            }

            while Instant::now() < deadline {
                let mut msg = Message::with_size(payload_size).expect("msg");
                common::encode_header(
                    msg.data_mut(),
                    common::PHASE_ACTIVE,
                    args.msg_size as u32,
                    seq,
                );
                match common::block_on(socket.send().message(msg).submit()) {
                    Ok(()) => {
                        seq += 1;
                        progressed = true;
                    }
                    Err(err) if err.code() == SubmitResult::Backpressured => {
                        send_pending[index] = true;
                        break;
                    }
                    Err(err) if err.code() == SubmitResult::NotConnected => {
                        send_pending[index] = true;
                        break;
                    }
                    Err(err) => panic!("send failed: {err}"),
                }
                if !burst_until_blocked {
                    break;
                }
            }
        }
        if !progressed {
            if Instant::now() >= deadline {
                break;
            }
            let remaining_ms = deadline
                .saturating_duration_since(Instant::now())
                .as_millis()
                .max(1) as i64;
            match poller.wait(&mut poll_events, remaining_ms) {
                Ok(event_count) => {
                    for event in &poll_events[..event_count] {
                        if event.revents & POLLOUT != 0 && event.slot < send_pending.len() {
                            send_pending[event.slot] = false;
                        }
                    }
                }
                Err(err) => panic!("poller wait failed: {err}"),
            }
        }
    }

    // C perf_multi_dealer_dealer_client.cpp run_single_size_case(): after the
    // send window, publish the wire stop token on every socket so the server's
    // poll wakes (the stop token is NOT the server's count anchor — its
    // measure-seconds deadline is). Bounded retry through transient backpressure.
    for socket in &sockets {
        let mut sent = false;
        for _ in 0..100 {
            match common::block_on(
                socket
                    .send()
                    .message(Message::try_from(common::STOP_TOKEN).expect("stop token"))
                    .submit(),
            ) {
                Ok(()) => {
                    sent = true;
                    break;
                }
                Err(err)
                    if matches!(
                        err.code(),
                        SubmitResult::Backpressured | SubmitResult::NotConnected
                    ) =>
                {
                    continue;
                }
                Err(err) => panic!("stop token send failed: {err}"),
            }
        }
        let _ = sent;
    }

    println!("CLIENT_DONE,{}", args.msg_size);
    io::stdout().flush().ok();
}
