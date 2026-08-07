//! DEALER-DEALER multi server: one-way active receiver.

#[path = "perf_common.rs"]
mod common;

use std::io::{self, BufRead};
use std::time::{Duration, Instant};
use zlink::{POLLIN, PollEvent, Poller, Received, RecvFlags, RecvResult};

fn main() {
    let args = common::MultiArgs::parse();
    let settings = common::MultiSettings::from_env();

    let ctx = common::perf_server_context();
    common::apply_multi_auto_hwm_msg_unit(&ctx, args.msg_size);
    let server = ctx.dealer_socket().expect("dealer");
    // C parity: numeric HWM remains behind the manual-override gate.
    common::apply_multi_hwm(&server, &settings);
    server
        .common_options()
        .set_receive_timeout(Duration::from_millis(settings.receive_timeout_ms))
        .expect("rcvtimeo");
    if matches!(args.transport.as_str(), "tls" | "wss") {
        let tls = common::resolve_perf_tls_paths().expect("TLS certs not found");
        common::setup_raw_tls_server(&server, &tls).expect("server tls");
    }

    let Some(bind_endpoint) =
        common::resolve_server_bind_endpoint("MULTI_DEALER_DEALER", &args.transport)
    else {
        return;
    };
    if let Err(err) = server.bind(&bind_endpoint) {
        if common::handle_transport_setup_error("MULTI_DEALER_DEALER", &args.transport, "bind", err)
        {
            return;
        }
        panic!("bind: {err}");
    }
    let endpoint = server.last_endpoint().expect("endpoint");
    common::print_ready(&endpoint);

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

    // C perf_multi_dealer_dealer_server.cpp run_receive_window(): poller POLLIN,
    // perf_socket_poll(...,-1) signal-driven, drain non-blocking, count until
    // the measure-seconds deadline. The wire stop token only wakes the poll; it
    // does NOT anchor aggregation (the server deadline does).
    let deadline = Instant::now() + Duration::from_secs(settings.duration_seconds.max(1));
    let mut latency_stats = common::LatencyStats::new();
    let mut active_count: u64 = 0;
    let mut received = Received::empty();

    let poller = Poller::new().expect("poller");
    poller.add_socket(&server, POLLIN, 0).expect("poller add");
    let mut events = vec![PollEvent::default(); 1];

    let mut record = |received: &Received, latency_stats: &mut common::LatencyStats| {
        let data = common::message_payload(received.parts());
        // Stop token (is_valid_active_message is false for it) only ends the
        // window via the deadline; it is never counted.
        if Instant::now() < deadline && common::is_valid_active_message(data, args.msg_size) {
            let sent_ts_ns = common::decode_sent_ts_ns(data);
            latency_stats
                .record_ns(common::now_ns().saturating_sub(sent_ts_ns.max(0) as u64) as f64);
            active_count += 1;
        }
    };

    'outer: loop {
        let remaining_ms = deadline
            .saturating_duration_since(Instant::now())
            .as_millis()
            .max(1) as i64;
        match poller.wait(&mut events, remaining_ms) {
            Ok(n) if n > 0 && events[0].is_readable() => {}
            Ok(_) if Instant::now() < deadline => continue,
            Ok(_) => break 'outer,
            Err(err) => panic!("poller wait failed: {err}"),
        }
        // Drain everything currently queued without blocking.
        loop {
            match server.recv(&mut received, RecvFlags::DONT_WAIT) {
                Ok(true) => {
                    record(&received, &mut latency_stats);
                }
                Ok(false) => break,
                Err(err) if err.code() == RecvResult::NoData => break,
                Err(err) => panic!("recv failed: {err}"),
            }
        }
        if Instant::now() >= deadline {
            break 'outer;
        }
    }

    // C drain_phase_until_idle(2.0s max, 50ms idle): UNCOUNTED tail drain so
    // stale messages do not spill into the next size case.
    let drain_deadline = Instant::now() + Duration::from_secs_f64(2.0);
    let mut idle_deadline = Instant::now() + Duration::from_millis(50);
    while Instant::now() < drain_deadline {
        match server.recv(&mut received, RecvFlags::DONT_WAIT) {
            Ok(true) => {
                idle_deadline = Instant::now() + Duration::from_millis(50);
            }
            Ok(false) | Err(_) => {
                if Instant::now() >= idle_deadline {
                    break;
                }
                common::poll_idle_until(idle_deadline, Duration::from_millis(1));
            }
        }
    }

    let stats = latency_stats.finish();
    let final_stats = common::StatsResult {
        count: active_count,
        ..stats
    };
    common::print_result(
        "MULTI_DEALER_DEALER",
        &args.transport,
        args.msg_size,
        settings.duration_seconds,
        &final_stats,
    );
}
