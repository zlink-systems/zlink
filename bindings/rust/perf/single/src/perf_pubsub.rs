//! Single PUB/SUB throughput/latency benchmark.

mod common;

use std::time::Duration;
use zlink::{RecvFlags, SocketMonitor, SubmitResult, TopicMessage};

fn main() {
    let config = common::PerfConfig::from_env_and_args();
    let Some(bind_endpoint) =
        common::resolve_endpoint_or_emit_unsupported("PUBSUB", &config.transport, "pubsub")
    else {
        return;
    };

    let ctx = common::perf_context();
    common::apply_single_auto_hwm_msg_unit(&ctx, config.size);
    let pub_sock = ctx.pub_socket().expect("pub");
    let sub_sock = ctx.sub_socket().expect("sub");
    // Match C perf: the harness always sets NODROP explicitly. The socket
    // default is lossy fanout, which would drop samples and the stop token.
    pub_sock
        .pub_options()
        .set_no_drop(true)
        .expect("pub no_drop");
    // Match C perf: numeric socket HWM remains behind the manual-override gate.
    common::apply_single_hwm(&pub_sock);
    common::apply_single_hwm(&sub_sock);
    if matches!(config.transport.as_str(), "tls" | "wss") {
        let tls = common::resolve_perf_tls_paths().expect("TLS certs not found");
        common::setup_raw_tls_server(&pub_sock, &tls).expect("pub tls");
        common::setup_raw_tls_client(&sub_sock, &tls).expect("sub tls");
    }

    let mut pub_mon = SocketMonitor::open(&pub_sock).expect("pub monitor");
    let mut mon = SocketMonitor::open(&sub_sock).expect("monitor");
    if let Err(err) = pub_sock.bind(&bind_endpoint) {
        if common::handle_transport_setup_error("PUBSUB", &config.transport, "bind", err) {
            return;
        }
        panic!("bind: {err}");
    }
    let endpoint = pub_sock.last_endpoint().unwrap_or(bind_endpoint);
    if let Err(err) = sub_sock.connect(&endpoint) {
        if common::handle_transport_setup_error("PUBSUB", &config.transport, "connect", err) {
            return;
        }
        panic!("connect: {err}");
    }
    sub_sock.set_subscription("").expect("subscribe");
    let ready_timeout = common::resolve_single_ready_timeout();
    common::wait_monitor_ready(&mut pub_mon, ready_timeout, "pubsub publisher");
    common::wait_monitor_ready(&mut mon, ready_timeout, "pubsub subscriber");
    std::thread::sleep(common::resolve_single_pubsub_ready_settle());

    let collector = common::MetricCollector::new();
    let stats = collector.shared();

    let active = Duration::from_secs(config.duration_seconds);
    let active_deadline = std::time::Instant::now() + active;
    let send_thread =
        std::thread::spawn(move || {
            common::send_loop(active_deadline, config.size, common::PHASE_ACTIVE, |msg| {
                match pub_sock
                    .publish("P")
                    .message(msg)
                    .submit()
                {
                    Ok(sent) => sent,
                    Err(err) if err.code() == SubmitResult::NotConnected => false,
                    Err(err) => panic!("active publish: {err}"),
                }
            });
            common::send_stop_token(|msg| {
                pub_sock
                    .publish("P")
                    .message(msg)
                    .flags(zlink::SendFlags::DONT_WAIT)
                    .submit()
            });
        });

    let stop_wait_deadline = active_deadline + common::resolve_single_stop_wait();
    loop {
        if std::time::Instant::now() >= stop_wait_deadline {
            break;
        }
        let mut received = TopicMessage::empty();
        let flags = if std::time::Instant::now() < active_deadline {
            RecvFlags::NONE
        } else {
            RecvFlags::DONT_WAIT
        };
        match sub_sock.subscribe(&mut received, flags) {
            Ok(true) => {
                let data = common::message_payload(received.parts());
                if common::is_stop_token(data) {
                    break;
                }
                common::handle_recv(data, config.size, &stats, active_deadline);
            }
            Ok(false) => common::poll_idle(Duration::from_millis(1)),
            Err(err) => panic!("pubsub subscriber recv failed: {err}"),
        }
    }
    send_thread.join().expect("sender thread");

    let result = collector.finish();
    common::print_result(
        "PUBSUB",
        &config.transport,
        config.size,
        config.duration_seconds,
        &result,
    );
}
