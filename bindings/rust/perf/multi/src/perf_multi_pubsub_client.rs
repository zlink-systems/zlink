#[path = "perf_common.rs"]
mod common;

use std::collections::HashSet;
use std::io::{self, BufRead, Write};
use std::time::{Duration, Instant};
use zlink::{
    POLLIN, PollEvent, Poller, RecvFlags, RecvResult, SocketMonitor, SubSocket, TopicMessage,
};

const DRAIN_LIMIT: Duration = Duration::from_secs(10);
const DRAIN_QUIET: Duration = Duration::from_millis(50);
const RECEIVE_POLL_MS: i64 = 10;
fn main() {
    let args = common::MultiArgs::parse();
    let settings = common::MultiSettings::from_env();

    let ctx = common::perf_client_context();
    let mut sockets: Vec<SubSocket> = Vec::with_capacity(settings.clients);
    let mut monitors: Vec<SocketMonitor> = Vec::with_capacity(settings.clients);

    for _ in 0..settings.clients {
        let sub = ctx.create_sub_socket().expect("sub");
        // C parity: numeric HWM remains behind the manual-override gate.
        common::apply_multi_hwm(&sub, &settings);
        if matches!(args.transport.as_str(), "tls" | "wss") {
            let tls = common::resolve_perf_tls_paths().expect("TLS certs not found");
            common::setup_raw_tls_client(&sub, &tls).expect("client tls");
        }
        // C multi helper subscribes all topics before connect. Keep that order
        // so subscription state is present when the PUB endpoint sees us.
        sub.set_subscription("").expect("subscribe");
        let mon = common::open_connection_ready_monitor(&sub);
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

    if !common::wait_for_start_stdin(args.msg_size) {
        return;
    }
    ctx.recalculate_auto_hwm().expect("recalculate auto hwm");

    let msg_size = args.msg_size;
    let mut phase_control = Some(std::thread::spawn(move || {
        let done_line = format!("PHASE_DONE,{msg_size},{}", common::PHASE_ACTIVE);
        for line in io::stdin().lock().lines() {
            if line.expect("phase control read failed").trim() == done_line {
                return Some(Instant::now());
            }
        }
        None
    }));

    let poller = Poller::new().expect("poller");
    for (index, socket) in sockets.iter().enumerate() {
        poller
            .add_socket(socket, POLLIN, index)
            .expect("poller add");
    }
    let mut events = vec![PollEvent::default(); sockets.len().max(1)];

    let mut last_payload = Instant::now();
    let active_deadline = last_payload + Duration::from_secs(settings.duration_seconds);
    let drain_deadline = active_deadline + DRAIN_LIMIT;
    let mut latency_stats = common::LatencyStats::new();
    let mut active_count: u64 = 0;
    let mut stopped = HashSet::with_capacity(sockets.len());
    let mut producer_done = None;
    let mut topic_msg = TopicMessage::empty();

    loop {
        let now = Instant::now();
        if now >= drain_deadline {
            eprintln!("multi_client_error: pubsub receive drain timed out");
            std::process::exit(1);
        }
        if now >= active_deadline || stopped.len() == sockets.len() {
            if let Some(done) = phase_control.take_if(|control| control.is_finished()) {
                producer_done = done.join().expect("phase control thread failed");
            }
        }
        // PUB may drop stop markers. Use the publisher's phase completion
        // signal before treating a quiet receive interval as drained.
        if let Some(done_at) = producer_done {
            if stopped.len() == sockets.len()
                || (now >= active_deadline
                    && now.saturating_duration_since(last_payload.max(done_at)) >= DRAIN_QUIET)
            {
                break;
            }
        }
        let wait_ms = common::poll_timeout_until(drain_deadline).min(RECEIVE_POLL_MS);
        let event_count = poller.wait(&mut events, wait_ms).expect("poller wait");
        for event in &events[..event_count] {
            if event.revents & POLLIN == 0 || event.slot >= sockets.len() {
                continue;
            }
            let slot = event.slot;
            for _ in 0..common::MAX_BURST_PER_SENDER {
                match sockets[slot].subscribe(&mut topic_msg, RecvFlags::DONT_WAIT) {
                    Ok(true) => {}
                    Ok(false) => break,
                    Err(err) if err.code == RecvResult::NoData => break,
                    Err(err) => panic!("recv failed: {err}"),
                }
                last_payload = Instant::now();
                if common::is_stop_message(topic_msg.parts()) {
                    stopped.insert(slot);
                    continue;
                }
                let data = common::message_payload(topic_msg.parts());
                if last_payload < active_deadline
                    && common::is_valid_active_message(data, args.msg_size)
                {
                    active_count += 1;
                    if let Some(elapsed_ns) = common::elapsed_since_sent_ns(data) {
                        latency_stats.record_latency_sample_ns(elapsed_ns);
                    }
                }
            }
        }
    }

    let stats = latency_stats.finish();
    let final_stats = common::StatsResult {
        count: active_count,
        ..stats
    };
    if let Some(socket) = sockets.first() {
        common::print_multi_auto_hwm_detail(
            socket,
            "endpoint",
            &args.transport,
            args.msg_size,
            "sub",
        );
    }
    common::print_result(
        "PUBSUB",
        &args.transport,
        args.msg_size,
        settings.duration_seconds,
        &final_stats,
    );
    // PERF_POLICY.md:469-471 / D-2 - the C PUBSUB client emits CLIENT_DONE
    // (bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp:367).
    println!("CLIENT_DONE,{}", args.msg_size);
    // C returns normally from main; print_result already flushed stdout. Avoid
    // std::process::exit so the flush is not raced.
}
