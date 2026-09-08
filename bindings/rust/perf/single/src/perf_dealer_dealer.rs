//! Single DEALER/DEALER throughput/latency benchmark.

mod common;
mod one_way;

use zlink::{SocketMonitor, SubmitResult};

fn main() {
    let config = common::PerfConfig::from_env_and_args();
    let Some(bind_endpoint) = common::resolve_endpoint_or_emit_unsupported(
        "DEALER_DEALER",
        &config.transport,
        "dealer-dealer",
    ) else {
        return;
    };

    let ctx = common::perf_context();
    let receiver = ctx.dealer_socket().expect("receiver");
    let sender = ctx.dealer_socket().expect("sender");
    // Match C perf: numeric socket HWM remains behind the manual-override gate.
    common::apply_single_hwm(&receiver);
    common::apply_single_hwm(&sender);
    if matches!(config.transport.as_str(), "tls" | "wss") {
        let tls = common::resolve_perf_tls_paths().expect("TLS certs not found");
        common::setup_raw_tls_server(&receiver, &tls).expect("receiver tls");
        common::setup_raw_tls_client(&sender, &tls).expect("sender tls");
    }

    let mut receiver_mon = SocketMonitor::open(&receiver).expect("receiver monitor");
    let mut mon = SocketMonitor::open(&sender).expect("monitor");
    if let Err(err) = receiver.bind(&bind_endpoint) {
        if common::handle_transport_setup_error("DEALER_DEALER", &config.transport, "bind", err) {
            return;
        }
        panic!("bind: {err}");
    }
    let endpoint = receiver.last_endpoint().unwrap_or(bind_endpoint);
    if let Err(err) = sender.connect(&endpoint) {
        if common::handle_transport_setup_error("DEALER_DEALER", &config.transport, "connect", err)
        {
            return;
        }
        panic!("connect: {err}");
    }
    let ready_timeout = common::resolve_single_ready_timeout();
    common::wait_monitor_ready(&mut receiver_mon, ready_timeout, "dealer-dealer receiver");
    common::wait_monitor_ready(&mut mon, ready_timeout, "dealer-dealer sender");
    let collector = common::MetricCollector::new();
    let stats = collector.shared();

    // PERF_SINGLE_TEST_POLICY § 1.4: sender signals phase end via wire-level
    // stop token; receiver loops on blocking `recv()` and exits when the
    // stop token arrives on the wire.
    let active = std::time::Duration::from_secs(config.duration_seconds);
    let active_deadline = common::now_ns() + active.as_nanos() as u64;
    let send_thread = std::thread::spawn(move || {
        common::drive_sends_with_poller(&sender);
        common::send_loop(
            active_deadline,
            config.size,
            common::PHASE_ACTIVE,
            |msg| match perf_submit_measurement!(sender.send(), msg) {
                Ok(()) => true,
                Err(err) if err.code() == SubmitResult::NotConnected => false,
                Err(err) if common::is_single_send_retry_error(&err) => false,
                Err(err) => panic!("active send: {err}"),
            },
        );
        common::send_stop_token(|msg| {
            // Match C perf: the phase terminator must be queued after every
            // accepted payload even when the data path has reached its HWM.
            common::submit_now(sender.send().message(msg).submit()).map(|()| true)
        });
        sender
    });

    let mut received = zlink::Received::empty();
    one_way::recv_until_stop(&receiver, || {
        match receiver.recv(&mut received, zlink::RecvFlags::DONT_WAIT) {
            Ok(true) => {
                let data = match one_way::classify(received.parts()) {
                    one_way::Content::Payload(data) => data,
                    one_way::Content::Stop => return one_way::RecvStep::Stop,
                };
                common::handle_recv(data, config.size, &stats, active_deadline);
                one_way::RecvStep::Payload
            }
            Ok(false) => one_way::RecvStep::Empty,
            Err(err) => panic!("dealer-dealer receiver recv failed: {err}"),
        }
    });
    let _sender = send_thread.join().expect("sender thread");

    let result = collector.finish();
    common::print_result(
        "DEALER_DEALER",
        &config.transport,
        config.size,
        config.duration_seconds,
        &result,
    );
}
