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
    let (done_tx, done_rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        for line in io::stdin().lock().lines() {
            let line = line.expect("phase control read");
            for phase in [common::PHASE_ACTIVE, common::PHASE_LATENCY] {
                if line == format!("PHASE_DONE,{msg_size},{phase}") {
                    if done_tx.send((phase, Instant::now())).is_err() {
                        return;
                    }
                }
            }
        }
    });

    let poller = Poller::new().expect("poller");
    for (index, socket) in sockets.iter().enumerate() {
        poller
            .add_socket(socket, POLLIN, index)
            .expect("poller add");
    }
    let mut events = vec![PollEvent::default(); sockets.len().max(1)];

    let active_seconds = settings.duration_seconds;
    let latency_seconds = common::latency_seconds(active_seconds);
    let mut tracker = common::CatchUp::default();
    let mut catch_up = None;
    let mut paced_epoch = None;
    let mut active_counts = vec![0u64; sockets.len()];
    let mut paced_counts = vec![0u64; sockets.len()];
    let mut sample_counts = vec![0u64; sockets.len()];
    let mut latency = common::LatencyStats::new();
    let mut reference = common::LatencyStats::new();
    let mut inflight_floor = None;
    let mut next_progress = 0;
    let mut topic_msg = TopicMessage::empty();

    let mut receive_phase = |phase,
                             seconds,
                             schedule: Option<&common::Schedule>,
                             active_counts: &mut [u64]| {
        let mut last_payload = Instant::now();
        let deadline = common::now_ns() + seconds * common::SECOND_NS;
        let drain_deadline = deadline + DRAIN_LIMIT.as_nanos() as u64;
        let mut stopped = HashSet::with_capacity(sockets.len());
        let mut done = None;
        loop {
            let now = common::now_ns();
            assert!(now < drain_deadline, "pubsub receive drain timed out");
            if let Ok((finished, when)) = done_rx.try_recv() {
                assert_eq!(finished, phase);
                done = Some(when);
            }
            if let Some(done_at) = done {
                if stopped.len() == sockets.len()
                    || Instant::now().saturating_duration_since(last_payload.max(done_at))
                        >= DRAIN_QUIET
                {
                    break;
                }
            }
            let n = poller
                .wait(&mut events, RECEIVE_POLL_MS)
                .expect("poller wait");
            for event in &events[..n] {
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
                    let now = common::now_ns();
                    last_payload = Instant::now();
                    let data = common::message_payload(topic_msg.parts());
                    if common::is_phase_marker(data, msg_size, phase) {
                        stopped.insert(slot);
                        continue;
                    }
                    if !common::is_valid_message(data, msg_size) {
                        continue;
                    }
                    match common::decode_phase(data) {
                        common::PHASE_ACTIVE
                            if schedule.is_none() && common::active_receive(now, deadline) =>
                        {
                            active_counts[slot] += 1;
                        }
                        received_phase @ (common::PHASE_WARMUP | common::PHASE_LATENCY)
                            if schedule.is_some() =>
                        {
                            paced_counts[slot] += 1;
                            let sent = common::decode_sent_ts_ns(data);
                            if sent <= 0 || sent as u64 > now {
                                continue;
                            }
                            let sent = sent as u64;
                            paced_epoch.get_or_insert(sent);
                            let ns = (now - sent) as f64;
                            if catch_up.is_none() {
                                reference.record_ns(ns);
                            } else if received_phase == common::PHASE_LATENCY {
                                latency.record_ns(ns);
                                sample_counts[slot] += 1;
                            }
                        }
                        _ => {}
                    }
                }
            }
            if let (Some(schedule), Some(epoch), None) = (schedule, paced_epoch, catch_up) {
                let now = common::now_ns();
                let elapsed = now.saturating_sub(epoch);
                let scheduled = schedule.scheduled(elapsed);
                if now >= next_progress {
                    let minimum = *paced_counts.iter().min().expect("subscribers");
                    let floor = scheduled.saturating_sub(minimum);
                    inflight_floor =
                        Some(inflight_floor.map_or(floor, |prior: u64| prior.min(floor)));
                    let seconds = elapsed as f64 / common::SECOND_NS as f64;
                    println!(
                        "PUBSUB_CATCHUP_PROGRESS,{msg_size},{},{floor},{minimum},{scheduled},{}",
                        common::diagnostic_number(seconds, 6),
                        common::diagnostic_number(schedule.rate, 6)
                    );
                    next_progress = now + common::SECOND_NS;
                }
                if elapsed <= latency_seconds * common::SECOND_NS
                    && tracker.observe(now, &paced_counts, scheduled)
                {
                    let seconds = elapsed as f64 / common::SECOND_NS as f64;
                    catch_up = Some(seconds);
                    println!("PHASE_CATCHUP,{msg_size}");
                    println!(
                        "PUBSUB_CATCHUP_DIAG,{msg_size},{},{},window_start_in_flight,{}",
                        common::diagnostic_number(seconds, 6),
                        tracker.in_flight,
                        tracker.start_in_flight
                    );
                }
            }
        }
    };
    receive_phase(
        common::PHASE_ACTIVE,
        active_seconds,
        None,
        &mut active_counts,
    );
    // The active rates are per subscriber. The publisher paces at the smallest.
    // Finish the active callback before borrowing its counts.
    let min_received = *active_counts.iter().min().expect("subscribers");
    assert!(min_received > 0, "subscriber received no active messages");
    println!("PHASE_DRAINED,{msg_size},{min_received}");
    let schedule = common::Schedule::new(0, min_received, active_seconds);
    receive_phase(
        common::PHASE_LATENCY,
        2 * latency_seconds,
        Some(&schedule),
        &mut active_counts,
    );
    println!(
        "PHASE_COMPLETE,{msg_size},{}",
        sample_counts.iter().min().unwrap()
    );
    let mut final_stats = latency.finish();
    final_stats.count = active_counts.iter().sum();
    if let Some(socket) = sockets.first() {
        common::print_multi_auto_hwm_detail(
            socket,
            "endpoint",
            &args.transport,
            args.msg_size,
            "sub",
        );
    }
    if final_stats.latency_count > 0 {
        println!(
            "PUBSUB_LATENCY_DIAG,{msg_size},latency_p50_ms,{:.6}",
            final_stats.p50_ns / 1e6
        );
        common::print_result(
            "PUBSUB",
            &args.transport,
            msg_size,
            active_seconds,
            &final_stats,
        );
    } else {
        let key = format!("RESULT,current,PUBSUB,{},{msg_size}", args.transport);
        let phase = common::build_phase_result("PUBSUB", msg_size, active_seconds, &final_stats);
        println!("{key},throughput,{:.3}", phase.throughput);
        println!("{key},bandwidth,{:.3}", phase.bandwidth);
        for metric in ["latency", "latency_p95", "latency_p99"] {
            println!("{key},{metric},invalid:receiver_not_caught_up_by_deadline");
        }
        let key = format!("REFERENCE,current,PUBSUB,{},{msg_size}", args.transport);
        let reference = reference.finish();
        let floor = inflight_floor.unwrap_or(0);
        println!("{key},latency_ref_p50,{:.6}", reference.p50_ns / 1e6);
        println!("{key},latency_ref_p99,{:.6}", reference.p99_ns / 1e6);
        println!("{key},inflight_floor,{floor}");
        println!(
            "{key},inflight_floor_ms,{:.6}",
            floor as f64 / schedule.rate * 1_000.0
        );
    }
    println!("CLIENT_DONE,{msg_size}");
}
