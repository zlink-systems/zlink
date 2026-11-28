//! DEALER-DEALER multi server: one-way active receiver.

#[path = "perf_common.rs"]
mod common;

use std::time::{Duration, Instant};
use zlink::{POLLIN, PollEvent, Poller, Received, RecvFlags, RecvResult};

fn main() {
    let args = common::MultiArgs::parse();
    let settings = common::MultiSettings::from_env();

    let ctx = common::perf_server_context();
    let server = ctx.create_dealer_socket().expect("dealer");
    // C parity: numeric HWM remains behind the manual-override gate.
    common::apply_multi_hwm(&server, &settings);
    server
        .common_options()
        .set_receive_timeout(settings.receive_timeout_ms)
        .expect("rcvtimeo");
    if matches!(args.transport.as_str(), "tls" | "wss") {
        let tls = common::resolve_perf_tls_paths().expect("TLS certs not found");
        common::setup_raw_tls_server(&server, &tls).expect("server tls");
    }
    let Some(bind_endpoint) =
        common::resolve_server_bind_endpoint("DEALER_DEALER", &args.transport)
    else {
        return;
    };
    if let Err(err) = server.bind(&bind_endpoint) {
        if common::handle_transport_setup_error("DEALER_DEALER", &args.transport, "bind", err) {
            return;
        }
        panic!("bind: {err}");
    }
    let endpoint = server.last_endpoint().expect("endpoint");
    common::print_ready(&endpoint);

    if !common::wait_for_start_stdin(args.msg_size) {
        return;
    }
    ctx.recalculate_auto_hwm().expect("recalculate auto hwm");

    let active_duration = Duration::from_secs(settings.duration_seconds.max(1));
    let deadline = Instant::now() + active_duration;
    let drain_limit = if args.msg_size >= 65_536 {
        (active_duration * 2).max(Duration::from_secs(10))
    } else {
        active_duration.max(Duration::from_secs(2))
    };
    let drain_deadline = deadline + drain_limit;
    let mut latency_stats = common::LatencyStats::new();
    let mut active_count: u64 = 0;
    let mut auto_hwm_printed = false;
    let mut received = Received::empty();

    let poller = Poller::new().expect("poller");
    poller.add_socket(&server, POLLIN, 0).expect("poller add");
    let mut events = [PollEvent::default()];

    // Each sender submits one stop token after its pending sends settle.
    let mut remaining_senders = settings.clients;
    while remaining_senders > 0 {
        let now = Instant::now();
        if now >= drain_deadline {
            eprintln!("multi_server_error: dealer-dealer receive drain timed out");
            std::process::exit(1);
        }
        match server.recv(&mut received, RecvFlags::DONT_WAIT) {
            Ok(true) => {
                if common::is_stop_message(received.parts()) {
                    remaining_senders -= 1;
                    continue;
                }
                if now >= deadline {
                    continue;
                }
                let data = common::message_payload(received.parts());
                if !common::is_valid_active_message(data, args.msg_size) {
                    continue;
                }
                active_count += 1;
                if !auto_hwm_printed {
                    common::print_multi_auto_hwm_detail(
                        &server,
                        "endpoint",
                        &args.transport,
                        args.msg_size,
                        "dealer",
                    );
                    auto_hwm_printed = true;
                }
                if let Some(elapsed_ns) = common::elapsed_since_sent_ns(data) {
                    latency_stats.record_latency_sample_ns(elapsed_ns);
                }
                continue;
            }
            Ok(false) => {}
            Err(err) if err.code() == RecvResult::NoData => {}
            Err(err) => panic!("recv failed: {err}"),
        }
        let wait_ms = common::poll_timeout_until(drain_deadline).min(10);
        poller.wait(&mut events, wait_ms).expect("poller wait");
    }

    let stats = latency_stats.finish();
    let final_stats = common::StatsResult {
        count: active_count,
        ..stats
    };
    common::print_result(
        "DEALER_DEALER",
        &args.transport,
        args.msg_size,
        settings.duration_seconds,
        &final_stats,
    );
}
