//! DEALER-DEALER multi server: one-way active receiver.

#[path = "perf_common.rs"]
mod common;

use std::time::Duration;
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
    let deadline = common::now_ns() + active_duration.as_nanos() as u64;
    let drain_limit = if args.msg_size >= 65_536 {
        (active_duration * 2).max(Duration::from_secs(10))
    } else {
        active_duration.max(Duration::from_secs(2))
    };
    let mut received = Received::empty();
    let poller = Poller::new().expect("poller");
    poller.add_socket(&server, POLLIN, 0).expect("poller add");
    let mut events = [PollEvent::default()];
    let mut receive_phase =
        |deadline, drain_end, sample_start, stats: &mut common::LatencyStats| {
            let mut remaining_senders = settings.clients;
            let mut paced_received = 0;
            while remaining_senders > 0 {
                assert!(common::now_ns() < drain_end, "receive drain timed out");
                match server.recv(&mut received, RecvFlags::DONT_WAIT) {
                    Ok(true) => {
                        let observed = common::now_ns();
                        if common::is_stop_message(received.parts()) {
                            remaining_senders -= 1;
                            continue;
                        }
                        let data = common::message_payload(received.parts());
                        paced_received += u64::from(common::record_measurement_receive(
                            data,
                            args.msg_size,
                            observed,
                            deadline,
                            sample_start,
                            1.0,
                            stats,
                        ));
                    }
                    Ok(false) => {
                        poller.wait(&mut events, 10).expect("poller wait");
                    }
                    Err(err) if err.code() == RecvResult::NoData => {
                        poller.wait(&mut events, 10).expect("poller wait");
                    }
                    Err(err) => panic!("recv failed: {err}"),
                }
            }
            paced_received
        };
    let mut active = common::LatencyStats::new();
    receive_phase(
        deadline,
        deadline + drain_limit.as_nanos() as u64,
        None,
        &mut active,
    );
    let active_count = active.finish().count;
    assert!(active_count > 0, "no active messages");
    println!("PHASE_DRAINED,{},{}", args.msg_size, active_count);

    let latency_seconds = common::latency_seconds(settings.duration_seconds);
    let sample_start = common::now_ns() + common::SETTLE_NS;
    let paced_end = sample_start + latency_seconds * common::SECOND_NS;
    let mut latency = common::LatencyStats::new();
    let paced_received = receive_phase(
        paced_end,
        paced_end + drain_limit.as_nanos() as u64,
        Some(sample_start),
        &mut latency,
    );
    let mut final_stats = latency.finish();
    println!("PHASE_COMPLETE,{},{}", args.msg_size, paced_received);
    final_stats.count = active_count;
    common::print_multi_auto_hwm_detail(
        &server,
        "endpoint",
        &args.transport,
        args.msg_size,
        "dealer",
    );
    common::print_result(
        "DEALER_DEALER",
        &args.transport,
        args.msg_size,
        settings.duration_seconds,
        &final_stats,
    );
}
