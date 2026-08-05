#[path = "perf_common.rs"]
mod common;

use std::io::{self, BufRead, Write};
use std::time::{Duration, Instant};
use zlink::{
    POLLIN, PollEvent, Poller, RecvFlags, RecvResult, SocketMonitor, SubSocket, TopicMessage,
};

const STOP_DRAIN_GRACE: Duration = Duration::from_millis(500);

// Returns (processed_any, stop_seen). C perf_multi_pubsub_client.cpp
// recv_one_pubsub_message(): the wire stop token (pubsub_recv_stop) ends the
// recv phase.
fn drain_subscriber(
    socket: &SubSocket,
    msg_size: usize,
    latency_stats: &mut common::LatencyStats,
    active_count: &mut u64,
    active_deadline: Instant,
    latency_stride: u64,
) -> bool {
    let mut stop_seen = false;
    let mut topic_msg = TopicMessage::empty();
    loop {
        match socket.subscribe(&mut topic_msg, RecvFlags::DONT_WAIT) {
            Ok(true) => {
                let data = common::message_payload(topic_msg.parts());
                if common::is_stop_token(data) {
                    stop_seen = true;
                    continue;
                }
                if !common::is_valid_active_message(data, msg_size) {
                    continue;
                }
                let now = Instant::now();
                // C: messages arriving after the active deadline are not counted.
                if now >= active_deadline {
                    continue;
                }
                *active_count += 1;
                if should_sample_latency(*active_count, latency_stride) {
                    let sent_ts_ns = common::decode_sent_ts_ns(data);
                    latency_stats.record_ns(
                        common::now_ns().saturating_sub(sent_ts_ns.max(0) as u64) as f64,
                    );
                }
            }
            Ok(false) => break,
            Err(err) if err.code == RecvResult::NoData => break,
            Err(err) => panic!("recv failed: {err}"),
        }
    }
    stop_seen
}

fn resolve_latency_sample_stride() -> u64 {
    std::env::var("PERF_MULTI_PUBSUB_LATENCY_SAMPLE_STRIDE")
        .ok()
        .and_then(|value| value.parse::<u64>().ok())
        .filter(|value| *value > 0)
        .unwrap_or(32)
}

fn should_sample_latency(index: u64, stride: u64) -> bool {
    stride <= 1 || index == 1 || index % stride == 0
}

fn main() {
    let args = common::MultiArgs::parse();
    let settings = common::MultiSettings::from_env();

    let ctx = common::perf_client_context();
    common::apply_multi_auto_hwm_msg_unit(&ctx, args.msg_size);
    let mut sockets: Vec<SubSocket> = Vec::with_capacity(settings.clients);
    let mut monitors: Vec<SocketMonitor> = Vec::with_capacity(settings.clients);

    for _ in 0..settings.clients {
        let sub = ctx.sub_socket().expect("sub");
        // C parity: numeric HWM remains behind the manual-override gate.
        common::apply_multi_hwm(&sub, &settings);
        if matches!(args.transport.as_str(), "tls" | "wss") {
            let tls = common::resolve_perf_tls_paths().expect("TLS certs not found");
            common::setup_raw_tls_client(&sub, &tls).expect("client tls");
        }
        // C multi helper subscribes all topics before connect. Keep that order
        // so subscription state is present when the PUB endpoint sees us.
        sub.set_subscription("").expect("subscribe");
        let mon = SocketMonitor::open(&sub).expect("monitor");
        sub.connect(&args.endpoint).expect("connect");
        sockets.push(sub);
        monitors.push(mon);
    }

    let ready_timeout = common::resolve_multi_connect_ready_timeout();
    for monitor in &mut monitors {
        common::wait_monitor_ready(monitor, ready_timeout, "multi pubsub client");
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
    }
    if !start_seen {
        return;
    }

    // C perf_multi_pubsub_client.cpp run_recv_duration(): zlink_poller_wait(-1)
    // signal-driven; the phase ends on the wire stop token (not a wall clock).
    // No hot-loop sleep(1ms).
    let poller = Poller::new().expect("poller");
    for (index, socket) in sockets.iter().enumerate() {
        poller
            .add_socket(socket, POLLIN, index)
            .expect("poller add");
    }
    let mut events = vec![PollEvent::default(); sockets.len().max(1)];

    let active_deadline = Instant::now() + Duration::from_secs(settings.duration_seconds);
    let mut latency_stats = common::LatencyStats::new();
    let mut active_count: u64 = 0;
    let mut phase_done = false;
    let latency_stride = resolve_latency_sample_stride();

    while !phase_done {
        let now = Instant::now();
        let wait_ms = if now < active_deadline {
            let remaining = active_deadline.saturating_duration_since(now);
            remaining.as_millis().clamp(1, i64::MAX as u128) as i64
        } else {
            let stop_deadline = active_deadline + STOP_DRAIN_GRACE;
            if now >= stop_deadline {
                break;
            }
            let remaining = stop_deadline.saturating_duration_since(now);
            remaining.as_millis().clamp(1, i64::MAX as u128) as i64
        };
        match poller.wait(&mut events, wait_ms) {
            Ok(event_count) => {
                for event in &events[..event_count] {
                    if event.revents & POLLIN == 0 || event.slot >= sockets.len() {
                        continue;
                    }
                    if drain_subscriber(
                        &sockets[event.slot],
                        args.msg_size,
                        &mut latency_stats,
                        &mut active_count,
                        active_deadline,
                        latency_stride,
                    ) {
                        phase_done = true;
                    }
                }
            }
            Err(err) => panic!("poller wait failed: {err}"),
        }
    }

    let stats = latency_stats.finish();
    let final_stats = common::StatsResult {
        count: active_count,
        ..stats
    };
    common::print_result(
        "MULTI_PUBSUB",
        &args.transport,
        args.msg_size,
        settings.duration_seconds,
        &final_stats,
    );
    // C returns normally from main; print_result already flushed stdout. Avoid
    // std::process::exit so the flush is not raced.
}
