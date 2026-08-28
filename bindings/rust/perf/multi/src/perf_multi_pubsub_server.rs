#[path = "perf_common.rs"]
mod common;

use std::io::{self, BufRead};
use std::time::{Duration, Instant};
use zlink::{Message, SubmitResult};

const TOPIC: &str = "bench";
const STOP_TOKEN_BURST: usize = 64;

fn main() {
    let args = common::MultiArgs::parse();
    let settings = common::MultiSettings::from_env();

    let ctx = common::perf_server_context();
    let pub_sock = ctx.pub_socket().expect("pub");
    common::apply_multi_hwm(&pub_sock, &settings);
    pub_sock
        .common_options()
        .set_send_timeout(Duration::from_millis(settings.send_timeout_ms))
        .expect("sndtimeo");
    if matches!(args.transport.as_str(), "tls" | "wss") {
        let tls = common::resolve_perf_tls_paths().expect("TLS certs not found");
        common::setup_raw_tls_server(&pub_sock, &tls).expect("server tls");
    }
    let Some(bind_endpoint) = common::resolve_server_bind_endpoint("MULTI_PUBSUB", &args.transport)
    else {
        return;
    };
    if let Err(err) = pub_sock.bind(&bind_endpoint) {
        if common::handle_transport_setup_error("MULTI_PUBSUB", &args.transport, "bind", err) {
            return;
        }
        panic!("bind: {err}");
    }
    let endpoint = pub_sock.last_endpoint().expect("endpoint");
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

    let deadline = Instant::now() + Duration::from_secs(settings.duration_seconds);
    let payload_size = args.msg_size.max(common::HEADER_SIZE);
    let mut seq: u64 = 1;

    while Instant::now() < deadline {
        let mut msg = Message::with_size(payload_size).expect("msg");
        common::encode_header(
            msg.data_mut(),
            common::PHASE_ACTIVE,
            args.msg_size as u32,
            seq,
        );
        match if common::measurement_part_count() == 2 {
            pub_sock
                .publish(TOPIC)
                .message(msg)
                .message(Message::try_from(&[] as &[u8]).expect("empty measurement tail"))
                .submit()
        } else {
            pub_sock.publish(TOPIC).message(msg).submit()
        } {
            Ok(()) => {
                seq += 1;
            }
            Err(err) => panic!("publish failed: {err}"),
        }
    }

    // PERF_MULTI_TEST_POLICY § 1.3.1: signal phase end via wire-level stop
    // token. Match the C runner by retrying until the token is actually
    // accepted; otherwise subscribers can wait forever after large messages.
    let mut accepted_stop_tokens = 0usize;
    while accepted_stop_tokens < STOP_TOKEN_BURST {
        let token = Message::try_from(common::STOP_TOKEN).expect("stop token");
        match pub_sock.publish(TOPIC).message(token).submit() {
            Ok(()) => {
                accepted_stop_tokens += 1;
            }
            Err(err)
                if matches!(
                    err.code(),
                    SubmitResult::Backpressured | SubmitResult::NotConnected
                ) =>
            {
                continue;
            }
            Err(err) => panic!("stop token publish failed: {err}"),
        }
    }
}
