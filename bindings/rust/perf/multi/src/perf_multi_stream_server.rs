#[path = "perf_common.rs"]
mod common;

use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};
use std::time::{Duration, Instant};
use zlink::{Message, RecvFlags, RecvResult, StreamPacket, StreamRecvMode, SubmitResult};

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
    let stream = ctx.stream_socket().expect("stream");
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
    stream
        .stream_options()
        .set_recv_mode(StreamRecvMode::Packet)
        .expect("stream packet receive mode");
    let mut ready_monitor = common::open_connection_ready_monitor(&stream);
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
    if !common::wait_for_start_stdin(args.msg_size) {
        return;
    }
    // CLIENT_READY proves the raw peers completed their side of connect. Confirm
    // the target-side event count before applying the connected-pipe HWM.
    common::wait_monitor_ready_count(
        &mut ready_monitor,
        settings.clients,
        common::resolve_multi_connect_ready_timeout(),
        "multi stream server",
    );
    ctx.recalculate_auto_hwm().expect("recalculate auto hwm");
    ready_monitor.status().expect("connected monitor snapshot");
    drop(ready_monitor);
    common::print_server_start_ready(args.msg_size);
    let stopping = Arc::new(AtomicBool::new(false));
    let control_stopping = Arc::clone(&stopping);
    let dispatcher = std::thread::current();
    std::thread::spawn(move || {
        common::wait_for_stop_stdin();
        control_stopping.store(true, Ordering::Release);
        dispatcher.unpark();
    });
    let mut tasks = common::ConcurrentTasks::new(0);
    let mut drain_deadline = None;
    let mut packet = StreamPacket::empty();
    loop {
        if stopping.load(Ordering::Acquire) && drain_deadline.is_none() {
            drain_deadline =
                Some(Instant::now() + Duration::from_millis(settings.send_timeout_ms.max(1)));
        }

        let received_packet = match stream.recv_packet(&mut packet, RecvFlags::DONT_WAIT) {
            Ok(true) => true,
            Ok(false) => false,
            Err(error) if error.code() == RecvResult::NoData => false,
            Err(error) => panic!("stream packet receive failed: {error}"),
        };
        if received_packet {
            let body = packet.body().expect("stream packet body").as_bytes();
            if common::is_stop_token(body) {
                stopping.store(true, Ordering::Release);
            } else if !stopping.load(Ordering::Acquire) {
                let routing_id = *packet.routing_id().expect("stream packet routing id");
                let header = packet.header().expect("stream packet header").as_bytes();
                let msg = build_packet_frame(header, body);
                tasks.push(stream.send(&routing_id).message(msg).submit());
            }
        }

        let mut completed_send = false;
        for (_, result) in tasks.poll_ready() {
            completed_send = true;
            match result {
                Ok(()) => {}
                Err(err)
                    if matches!(
                        err.code(),
                        SubmitResult::NotConnected | SubmitResult::NotFound
                    ) => {}
                Err(err) => panic!("stream echo send failed: {err}"),
            }
        }

        if stopping.load(Ordering::Acquire) {
            let deadline = drain_deadline.get_or_insert_with(|| {
                Instant::now() + Duration::from_millis(settings.send_timeout_ms.max(1))
            });
            if !tasks.any_pending() || Instant::now() >= *deadline {
                break;
            }
            if !received_packet && !completed_send {
                let wait = deadline.saturating_duration_since(Instant::now());
                tasks.wait_for_wake(wait);
            }
        } else if !received_packet && !completed_send {
            tasks.wait_for_wake(Duration::from_millis(1));
        }
    }
}
