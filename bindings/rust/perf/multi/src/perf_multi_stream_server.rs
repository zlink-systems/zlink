#[path = "perf_common.rs"]
mod common;

use std::sync::{
    Arc,
    atomic::{AtomicUsize, Ordering},
    mpsc,
};
use std::time::{Duration, Instant};
use zlink::{Message, SubmitResult};

const STOP_REQUESTED: usize = 1usize << (usize::BITS - 1);
const CALLBACK_COUNT_MASK: usize = STOP_REQUESTED - 1;

enum ServerEvent {
    Echo(zlink::RoutingId, Vec<u8>, Vec<u8>),
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
    let mut ready_monitor = common::open_connection_ready_monitor(&stream);
    let (event_tx, event_rx) = mpsc::channel::<ServerEvent>();
    // The high bit is the stop gate and the low bits count callbacks that may
    // still enqueue one pre-stop Echo. Keeping both in one atomic makes the
    // stop-vs-enqueue boundary observable without a per-packet mutex.
    let callback_state = Arc::new(AtomicUsize::new(0));
    let dispatcher = std::thread::current();
    // C perf_multi_stream_session.hpp handle_packet_message(): the wire stop
    // token in the body ends the run; the echo send result is NOT discarded —
    // a real send failure stops the server (it is not silently swallowed).
    let packet_tx = event_tx.clone();
    let packet_callback_state = callback_state.clone();
    let packet_dispatcher = dispatcher.clone();
    stream
        .on_packet(move |routing_id, header, body| {
            let state = packet_callback_state.fetch_add(1, Ordering::AcqRel);
            let finish_callback = || {
                packet_callback_state.fetch_sub(1, Ordering::AcqRel);
                packet_dispatcher.unpark();
            };
            if state & STOP_REQUESTED != 0 {
                finish_callback();
                return;
            }
            let body_bytes = body.as_bytes();
            if common::is_stop_token(body_bytes) {
                packet_callback_state.fetch_or(STOP_REQUESTED, Ordering::AcqRel);
                finish_callback();
                return;
            }
            let _ = packet_tx.send(ServerEvent::Echo(
                routing_id,
                header.as_bytes().to_vec(),
                body_bytes.to_vec(),
            ));
            finish_callback();
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
    drop(event_tx);
    let control_callback_state = callback_state.clone();
    let control_dispatcher = dispatcher.clone();
    std::thread::spawn(move || {
        common::wait_for_stop_stdin();
        control_callback_state.fetch_or(STOP_REQUESTED, Ordering::AcqRel);
        control_dispatcher.unpark();
    });
    let mut tasks = common::ConcurrentTasks::new(0);
    let mut drain_deadline = None;
    let mut prefetched_event = None;
    loop {
        let stopping = callback_state.load(Ordering::Acquire) & STOP_REQUESTED != 0;
        if stopping && drain_deadline.is_none() {
            drain_deadline =
                Some(Instant::now() + Duration::from_millis(settings.send_timeout_ms.max(1)));
        }

        // Consume at most one receive event per turn, then poll newly installed
        // or woken send Futures. A continuously busy packet callback must not
        // starve Core admission/completion progress for already queued echoes.
        let mut queue_empty = false;
        let event = if let Some(event) = prefetched_event.take() {
            Some(event)
        } else {
            match event_rx.try_recv() {
                Ok(event) => Some(event),
                Err(mpsc::TryRecvError::Empty) => {
                    queue_empty = true;
                    None
                }
                Err(mpsc::TryRecvError::Disconnected) => {
                    callback_state.fetch_or(STOP_REQUESTED, Ordering::AcqRel);
                    queue_empty = true;
                    None
                }
            }
        };
        let received_event = event.is_some();
        if let Some(ServerEvent::Echo(routing_id, header, body)) = event {
            let msg = build_packet_frame(&header, &body);
            tasks.push(stream.send(&routing_id).message(msg).submit());
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

        let state = callback_state.load(Ordering::Acquire);
        if state & STOP_REQUESTED != 0 {
            let deadline = drain_deadline.get_or_insert_with(|| {
                Instant::now() + Duration::from_millis(settings.send_timeout_ms.max(1))
            });
            if Instant::now() >= *deadline {
                break;
            }

            if queue_empty && !tasks.any_pending() && state & CALLBACK_COUNT_MASK == 0 {
                // A callback that passed the stop check before STOP keeps the
                // in-flight counter non-zero until its Echo is enqueued. Once
                // the counter reaches zero, confirm the queue is still empty
                // before declaring the ordered pre-STOP tail drained.
                match event_rx.try_recv() {
                    Ok(event) => {
                        prefetched_event = Some(event);
                        continue;
                    }
                    Err(mpsc::TryRecvError::Empty | mpsc::TryRecvError::Disconnected) => break,
                }
            }

            if queue_empty && !received_event && !completed_send {
                let wait = deadline.saturating_duration_since(Instant::now());
                tasks.wait_for_wake(wait);
            }
        } else if !received_event && !completed_send {
            // Packet callbacks, stdin STOP, and public send Future wakers all
            // unpark this dispatcher. No periodic timer is needed to advance
            // the async terminal.
            std::thread::park();
        }
    }
}
