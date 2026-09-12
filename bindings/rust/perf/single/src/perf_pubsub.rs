//! Single PUB/SUB throughput/latency benchmark.

mod common;
mod one_way;

use zlink::{Message, RecvFlags, SocketMonitor, SubmitResult, TopicMessage};

fn main() {
    let config = common::PerfConfig::from_env_and_args();
    let Some(bind_endpoint) =
        common::resolve_endpoint_or_emit_unsupported("PUBSUB", &config.transport, "pubsub")
    else {
        return;
    };

    let ctx = common::perf_context();
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

    let active = std::time::Duration::from_secs(config.duration_seconds);
    let active_deadline = common::now_ns() + active.as_nanos() as u64;
    let send_thread = std::thread::spawn(move || {
        common::send_loop(active_deadline, config.size, common::PHASE_ACTIVE, |msg| {
            match if common::measurement_part_count() == 2 {
                pub_sock
                    .publish("P")
                    .message(msg)
                    .message(Message::try_from(&[] as &[u8]).expect("empty measurement tail"))
                    .flags(zlink::SendFlags::DONT_WAIT)
                    .submit()
            } else {
                pub_sock
                    .publish("P")
                    .message(msg)
                    .flags(zlink::SendFlags::DONT_WAIT)
                    .submit()
            } {
                Ok(()) => true,
                Err(err)
                    if err.code() == SubmitResult::NotConnected
                        || common::is_single_send_retry_error(&err) =>
                {
                    false
                }
                Err(err) => panic!("active publish: {err}"),
            }
        });
        common::send_stop_token(|msg| pub_sock.publish("P").message(msg).submit().map(|()| true));
        pub_sock
    });

    let mut received = TopicMessage::empty();
    one_way::recv_until_stop(&sub_sock, || {
        match sub_sock.subscribe(&mut received, RecvFlags::DONT_WAIT) {
            Ok(true) => {
                let data = match one_way::classify(received.parts()) {
                    one_way::Content::Payload(data) => data,
                    one_way::Content::Stop => return one_way::RecvStep::Stop,
                };
                common::handle_recv(data, config.size, &stats, active_deadline);
                one_way::RecvStep::Payload
            }
            Ok(false) => one_way::RecvStep::Empty,
            Err(err) => panic!("pubsub subscriber recv failed: {err}"),
        }
    });
    let _pub_sock = send_thread.join().expect("sender thread");

    let result = collector.finish();
    common::print_result(
        "PUBSUB",
        &config.transport,
        config.size,
        config.duration_seconds,
        &result,
    );

    // Match the C guard destruction order: disconnect the subscriber before
    // closing the inproc publisher endpoint.
    drop(mon);
    drop(pub_mon);
    drop(sub_sock);
    drop(_pub_sock);
}
