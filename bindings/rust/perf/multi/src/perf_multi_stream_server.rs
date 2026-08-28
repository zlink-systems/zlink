#[path = "perf_common.rs"]
mod common;

use std::sync::mpsc;
use zlink::Message;

enum ServerEvent {
    Echo(zlink::RoutingId, Vec<u8>, Vec<u8>),
    Stop,
}

fn build_packet_frame(header: &[u8], body: &[u8]) -> Message {
    let mut packet = Message::with_size(6 + header.len() + body.len()).expect("packet");
    let frame = packet.data_mut();
    frame[0..2].copy_from_slice(&(header.len() as u16).to_be_bytes());
    frame[2..6].copy_from_slice(&(body.len() as u32).to_be_bytes());
    frame[6..6 + header.len()].copy_from_slice(header);
    frame[6 + header.len()..].copy_from_slice(body);
    packet
}

fn main() {
    let args = common::MultiArgs::parse();
    let settings = common::MultiSettings::from_env();
    let ctx = common::perf_server_context();
    let mut stream = ctx.stream_socket().expect("stream");
    // C parity: numeric HWM remains behind the manual-override gate.
    common::apply_multi_hwm(&stream, &settings);
    stream
        .common_options()
        .set_send_timeout(std::time::Duration::from_millis(settings.send_timeout_ms))
        .expect("sndtimeo");
    stream
        .common_options()
        .set_receive_timeout(std::time::Duration::from_millis(
            settings.receive_timeout_ms,
        ))
        .expect("rcvtimeo");
    stream
        .common_options()
        .set_tcp_no_delay(true)
        .expect("tcp_no_delay");
    let (event_tx, event_rx) = mpsc::channel::<ServerEvent>();
    // C perf_multi_stream_session.hpp handle_packet_message(): the wire stop
    // token in the body ends the run; the echo send result is NOT discarded —
    // a real send failure stops the server (it is not silently swallowed).
    let packet_tx = event_tx.clone();
    stream
        .on_packet(move |routing_id, header, body| {
            let body_bytes = body.as_bytes();
            if common::is_stop_token(body_bytes) {
                let _ = packet_tx.send(ServerEvent::Stop);
                return;
            }
            let _ = packet_tx.send(ServerEvent::Echo(
                routing_id,
                header.as_bytes().to_vec(),
                body_bytes.to_vec(),
            ));
        })
        .expect("on_packet");
    let Some(bind_endpoint) = common::resolve_server_bind_endpoint("MULTI_STREAM", &args.transport)
    else {
        return;
    };
    if matches!(args.transport.as_str(), "tls" | "wss") {
        let tls = common::resolve_perf_tls_paths().expect("TLS certs not found");
        common::setup_raw_tls_server(&stream, &tls).expect("stream tls");
    }
    if let Err(err) = stream.bind(&bind_endpoint) {
        if common::handle_transport_setup_error("MULTI_STREAM", &args.transport, "bind", err) {
            return;
        }
        panic!("bind: {err}");
    }
    let endpoint = stream.last_endpoint().expect("endpoint");
    common::print_ready(&endpoint);
    let stop_tx = event_tx;
    std::thread::spawn(move || {
        common::wait_for_stop_stdin();
        let _ = stop_tx.send(ServerEvent::Stop);
    });
    // Both stdin teardown and the wire token wake one blocking event receive;
    // there is no atomic-flag or timer polling shutdown path.
    while let Ok(event) = event_rx.recv() {
        match event {
            ServerEvent::Stop => break,
            ServerEvent::Echo(routing_id, header, body) => {
                let msg = build_packet_frame(&header, &body);
                common::block_on(stream.send(&routing_id).message(msg).submit())
                    .unwrap_or_else(|err| panic!("stream echo send failed: {err}"));
            }
        }
    }
}
