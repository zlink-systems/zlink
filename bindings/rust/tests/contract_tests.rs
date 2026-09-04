//! Contract tests – verify FFI/native call mapping, type conversions,
//! and resource lifecycle.

mod test_support;

use std::io::Write;
use std::process::{Command, Stdio};
use std::sync::{Arc, Barrier};
use std::thread;

use zlink::{
    Context, DealerSocket, Message, POLLCOMPLETION, POLLIN, Poller, Received, RecvFlags,
    RouterSocket, RoutingId, SubmitResult, has, version,
};

fn packaged_core_version() -> (i32, i32, i32) {
    let header = std::fs::read_to_string(format!("{}/include/zlink.h", env!("CARGO_MANIFEST_DIR")))
        .expect("packaged Core header must be readable");
    let component = |name: &str| -> i32 {
        header
            .lines()
            .find_map(|line| line.strip_prefix(&format!("#define ZLINK_VERSION_{name} ")))
            .expect("packaged Core header must define its version")
            .parse()
            .expect("packaged Core version component must be numeric")
    };
    (component("MAJOR"), component("MINOR"), component("PATCH"))
}

// ---------------------------------------------------------------------------
// Context lifecycle
// ---------------------------------------------------------------------------

#[test]
fn direct_common_header_version_matches_packaged_core() {
    let mut child = Command::new("cc")
        .args([
            "-E",
            &format!("-I{}/include", env!("CARGO_MANIFEST_DIR")),
            "-x",
            "c",
            "-",
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("failed to start C preprocessor");

    child
        .stdin
        .as_mut()
        .expect("preprocessor stdin should be available")
        .write_all(b"#include <zlink/common.h>\nZLINK_VERSION_PATCH\n")
        .expect("failed to write preprocessor input");

    let output = child
        .wait_with_output()
        .expect("failed to wait for C preprocessor");
    assert!(
        output.status.success(),
        "preprocess zlink/common.h failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    let stdout = String::from_utf8_lossy(&output.stdout);
    let version_patch = stdout
        .lines()
        .rev()
        .map(str::trim)
        .find(|line| !line.is_empty())
        .unwrap_or("");
    assert_eq!(version_patch, packaged_core_version().2.to_string());
}

#[test]
fn context_create_and_drop() {
    let ctx = Context::new().unwrap();
    // Context should create successfully and drop without leak
    drop(ctx);
}

#[test]
fn context_typed_options() {
    let ctx = Context::new().unwrap();
    let _options = ctx.options();
}

// ---------------------------------------------------------------------------
// Socket lifecycle
// ---------------------------------------------------------------------------

#[test]
fn socket_create_and_drop() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    drop(sock);
    // Socket close should complete without native leak
}

#[test]
fn socket_bind_connect_lifecycle() {
    let ctx = Context::new().unwrap();
    let server = ctx.pair_socket().unwrap();
    server.bind("inproc://contract-lifecycle").unwrap();

    let client = ctx.pair_socket().unwrap();
    client.connect("inproc://contract-lifecycle").unwrap();

    drop(client);
    drop(server);
    drop(ctx);
}

// ---------------------------------------------------------------------------
// Message lifecycle
// ---------------------------------------------------------------------------

#[test]
fn message_create_empty() {
    let msg = Message::new().unwrap();
    assert!(msg.is_empty());
    assert_eq!(msg.size(), 0);
}

#[test]
fn message_try_from_bytes() {
    let data = b"contract-test-payload";
    let msg = Message::try_from(data).unwrap();
    assert_eq!(msg.as_bytes(), data);
    assert_eq!(msg.to_vec(), data);
    assert_eq!(msg.size(), data.len());
}

#[test]
fn message_try_from_string() {
    let msg = Message::try_from("contract-text").unwrap();
    assert_eq!(msg.as_bytes(), b"contract-text");
    assert_eq!(msg.as_str().unwrap(), "contract-text");
}

#[test]
fn message_with_size() {
    let msg = Message::with_size(128).unwrap();
    assert_eq!(msg.size(), 128);
}

#[test]
fn message_allocate_writable_payload() {
    let mut msg = Message::allocate(3).unwrap();
    msg.data_mut().copy_from_slice(&[0x01, 0x02, 0x03]);
    assert_eq!(msg.as_bytes(), &[0x01, 0x02, 0x03]);
}

#[test]
fn message_as_str() {
    let msg = Message::try_from(b"hello").unwrap();
    assert_eq!(msg.as_str().unwrap(), "hello");
}

#[test]
fn message_copy_helpers_copy_payload() {
    let msg = Message::try_from(b"copy-payload").unwrap();
    let copy = msg.try_clone().unwrap();
    assert_eq!(copy.as_bytes(), msg.as_bytes());

    let mut destination = [0u8; 12];
    let written = msg.copy_to(&mut destination).unwrap();
    assert_eq!(written, 12);
    assert_eq!(&destination, b"copy-payload");

    let mut too_small = [0u8; 11];
    assert!(msg.copy_to(&mut too_small).is_err());
}

#[test]
fn message_drop_calls_close() {
    // Create and drop many messages to verify no native leak
    for _ in 0..1000 {
        let msg = Message::try_from(b"drop-test").unwrap();
        drop(msg);
    }
}

// ---------------------------------------------------------------------------
// RoutingId managed ↔ native conversion
// ---------------------------------------------------------------------------

#[test]
fn routing_id_roundtrip() {
    let rid = RoutingId::from(b"peer-42");
    assert_eq!(rid.as_bytes(), b"peer-42");
    assert_eq!(rid.size(), 7);
}

#[test]
fn routing_id_max_length() {
    let data = vec![0xABu8; 255];
    let rid = RoutingId::from(data.as_slice());
    assert_eq!(rid.size(), 255);
}

// ---------------------------------------------------------------------------
// Version / capability native mapping
// ---------------------------------------------------------------------------

#[test]
fn version_returns_valid_triple() {
    let (major, minor, patch) = version();
    let expected = packaged_core_version();
    assert_eq!((major, minor, patch), expected);
}

#[test]
fn has_capability_check() {
    // "tcp" should always be supported
    // Just verify the function doesn't crash
    let _ = has("tcp");
}

#[test]
fn strerror_returns_native_error_text() {
    let message = zlink::strerror(libc::EINVAL);
    assert!(!message.is_empty());
}

// ---------------------------------------------------------------------------
// Error path: native handle creation failure
// ---------------------------------------------------------------------------

#[test]
fn all_socket_types_create_successfully() {
    let ctx = Context::new().unwrap();
    let _ = ctx.pair_socket().unwrap();
    let _ = ctx.pub_socket().unwrap();
    let _ = ctx.sub_socket().unwrap();
    let _ = ctx.dealer_socket().unwrap();
    let _ = ctx.router_socket().unwrap();
    let _ = ctx.xpub_socket().unwrap();
    let _ = ctx.xsub_socket().unwrap();
    let _ = ctx.stream_socket().unwrap();
}

#[test]
fn request_reply_surface_exists() {
    let _dealer_request = DealerSocket::request;
    let _router_request = RouterSocket::request;
    let _router_reply =
        |socket: &RouterSocket, rid: &RoutingId, token: zlink::ReplyToken, msg: Message| {
            socket.reply(rid, token).message(msg).submit()
        };
}

#[test]
fn concurrent_multipart_publish_exposes_core_rejection_and_releases_parts() {
    const WORKERS: usize = 8;
    const PER_WORKER: usize = 500;

    let ctx = Context::new().unwrap();
    let publisher = ctx.pub_socket().unwrap();

    let mut work = (0..WORKERS).map(|_| Vec::new()).collect::<Vec<_>>();
    for (worker, worker_records) in work.iter_mut().enumerate() {
        for index in 0..PER_WORKER {
            let first_bytes = format!("record-{worker}-{index:03}-a").into_bytes();
            let second_bytes = format!("record-{worker}-{index:03}-b").into_bytes();
            let first = Message::try_from(first_bytes.as_slice()).unwrap();
            let second = Message::try_from(second_bytes.as_slice()).unwrap();
            let first_owner = first.try_clone().unwrap();
            let second_owner = second.try_clone().unwrap();
            let publish = publisher.publish("contract").message(first).message(second);
            worker_records.push((
                publish,
                first_owner,
                second_owner,
                first_bytes,
                second_bytes,
            ));
        }
    }

    let start = Arc::new(Barrier::new(WORKERS));
    let handles = work
        .into_iter()
        .map(|requests| {
            let start = Arc::clone(&start);
            thread::spawn(move || {
                let mut accepted = 0usize;
                let mut rejected = 0usize;
                start.wait();
                for (publish, first, second, first_bytes, second_bytes) in requests {
                    match publish.submit() {
                        Err(error)
                            if error.code() == SubmitResult::InvalidArgument
                                && error.native_errno() == libc::EINVAL =>
                        {
                            assert_eq!(first.as_bytes(), first_bytes);
                            assert_eq!(second.as_bytes(), second_bytes);
                            assert_eq!(first.ref_count(), 1);
                            assert_eq!(second.ref_count(), 1);
                            rejected += 1;
                        }
                        Ok(()) => accepted += 1,
                        Err(error) => panic!("unexpected concurrent publish error: {error}"),
                    }
                }
                (accepted, rejected)
            })
        })
        .collect::<Vec<_>>();

    let (accepted, rejected) = handles
        .into_iter()
        .map(|handle| handle.join().unwrap())
        .fold((0usize, 0usize), |total, result| {
            (total.0 + result.0, total.1 + result.1)
        });
    assert!(accepted > 0, "Core accepted no multipart publish");
    assert!(rejected > 0, "Core exposed no competing-attempt rejection");
    assert_eq!(accepted + rejected, WORKERS * PER_WORKER);
    eprintln!(
        "concurrent multipart publishes: attempts={} accepted={accepted} rejected={rejected}",
        WORKERS * PER_WORKER
    );
}

#[test]
fn poller_modify_transfers_completion_ownership() {
    let ctx = Context::new().unwrap();
    let dealer = ctx.dealer_socket().unwrap();
    let poller = Poller::new().unwrap();

    poller.add_socket(&dealer, POLLCOMPLETION, 1).unwrap();
    poller.modify_socket(&dealer, POLLIN).unwrap();
    poller.remove_socket(&dealer).unwrap();

    poller.add_socket(&dealer, POLLIN, 2).unwrap();
    poller
        .modify_socket(&dealer, POLLIN | POLLCOMPLETION)
        .unwrap();
}

#[test]
fn pollcompletion_reports_only_after_request_future_is_settled() {
    let ctx = Context::new().unwrap();
    let router = ctx.router_socket().unwrap();
    let dealer = ctx.dealer_socket().unwrap();
    router
        .bind("inproc://rust-public-completion-owner")
        .unwrap();
    dealer
        .connect("inproc://rust-public-completion-owner")
        .unwrap();
    dealer
        .common_options()
        .set_send_timeout(std::time::Duration::from_secs(5))
        .unwrap();
    router
        .common_options()
        .set_receive_timeout(std::time::Duration::from_secs(5))
        .unwrap();

    // Complete a blocking data handshake before the first DONTWAIT REQUEST.
    // This removes transport attachment from the completion-owner assertion.
    dealer
        .send()
        .message(Message::try_from(b"ready").unwrap())
        .submit_sync()
        .unwrap();
    let mut ready = Received::empty();
    assert!(router.recv(&mut ready, RecvFlags::NONE).unwrap());
    assert_eq!(ready.single_part().unwrap().as_bytes(), b"ready");

    let poller = Poller::new().unwrap();
    poller.add_socket(&dealer, POLLCOMPLETION, 17).unwrap();
    let responder = thread::spawn(move || {
        let mut request = Received::empty();
        assert!(router.recv(&mut request, RecvFlags::NONE).unwrap());
        request
            .reply()
            .message(Message::try_from(b"poller-reply").unwrap())
            .submit()
            .unwrap();
    });

    let mut future = Box::pin(
        dealer
            .request()
            .message(Message::try_from(b"poller-request").unwrap())
            .timeout(std::time::Duration::from_secs(2))
            .submit(),
    );
    assert!(test_support::poll_once(&mut future).is_pending());
    let mut events = [zlink::PollEvent::default()];
    assert_eq!(poller.wait(&mut events, 5_000).unwrap(), 1);
    assert_eq!(events[0].slot, 17);
    assert_ne!(events[0].revents & POLLCOMPLETION, 0);
    let reply = test_support::block_on(future).unwrap();
    assert_eq!(reply[0].as_bytes(), b"poller-reply");

    poller.modify_socket(&dealer, POLLIN).unwrap();
    poller.modify_socket(&dealer, POLLCOMPLETION).unwrap();
    poller.remove_socket(&dealer).unwrap();
    responder.join().unwrap();
}

#[test]
fn ordinary_router_message_has_no_reply_token() {
    let ctx = Context::new().unwrap();
    let router_socket = ctx.router_socket().unwrap();
    let dealer_socket = ctx.dealer_socket().unwrap();
    router_socket
        .bind("inproc://rust-request-reply-data")
        .unwrap();
    dealer_socket
        .connect("inproc://rust-request-reply-data")
        .unwrap();

    test_support::block_on(
        dealer_socket
            .send()
            .message(Message::try_from(b"plain-data").unwrap())
            .submit(),
    )
    .unwrap();

    let mut received = Received::empty();
    router_socket.recv(&mut received, RecvFlags::NONE).unwrap();
    let reply_token = received.reply_token();
    assert_eq!(received.single_part().unwrap().as_bytes(), b"plain-data");
    assert_eq!(reply_token, None);
}

#[test]
fn reply_token_rejects_a_different_router_owner_before_native_submit() {
    let ctx = Context::new().unwrap();
    let router = ctx.router_socket().unwrap();
    let other_router = ctx.router_socket().unwrap();
    let dealer = ctx.dealer_socket().unwrap();
    router.bind("inproc://rust-reply-token-owner").unwrap();
    dealer.connect("inproc://rust-reply-token-owner").unwrap();
    dealer
        .send()
        .message(Message::try_from(b"ready").unwrap())
        .submit_sync()
        .unwrap();
    let mut ready = Received::empty();
    assert!(router.recv(&mut ready, RecvFlags::NONE).unwrap());

    let responder = std::thread::spawn(move || {
        let mut request = Received::empty();
        assert!(router.recv(&mut request, RecvFlags::NONE).unwrap());
        let rid = *request.routing_id().expect("request routing id");
        let token = request.reply_token().expect("request reply token");
        assert_eq!(token, token.clone());
        assert_eq!(format!("{token:?}"), "ReplyToken");

        let error = other_router
            .reply(&rid, token.clone())
            .message(Message::try_from(b"wrong-owner").unwrap())
            .submit()
            .unwrap_err();
        assert_eq!(error.code(), SubmitResult::InvalidArgument);

        request
            .reply()
            .message(Message::try_from(b"right-owner").unwrap())
            .submit()
            .unwrap();
    });

    let reply = dealer
        .request()
        .message(Message::try_from(b"request").unwrap())
        .timeout(std::time::Duration::from_secs(2))
        .submit_sync()
        .unwrap();
    assert_eq!(reply[0].as_bytes(), b"right-owner");
    responder.join().unwrap();
}

#[test]
fn reply_token_from_closed_router_is_rejected_by_recreated_router() {
    let ctx = Context::new().unwrap();
    let mut original_router = ctx.router_socket().unwrap();
    let dealer = ctx.dealer_socket().unwrap();
    original_router
        .bind("inproc://rust-stale-reply-token-owner")
        .unwrap();
    dealer
        .connect("inproc://rust-stale-reply-token-owner")
        .unwrap();
    dealer
        .send()
        .message(Message::try_from(b"ready").unwrap())
        .submit_sync()
        .unwrap();
    let mut ready = Received::empty();
    assert!(original_router.recv(&mut ready, RecvFlags::NONE).unwrap());

    let (stale_tx, stale_rx) = std::sync::mpsc::channel();
    let responder = std::thread::spawn(move || {
        let mut request = Received::empty();
        assert!(original_router.recv(&mut request, RecvFlags::NONE).unwrap());
        let rid = *request.routing_id().expect("request routing id");
        let token = request.reply_token().expect("request reply token");
        request
            .reply()
            .message(Message::try_from(b"original-owner").unwrap())
            .submit()
            .unwrap();
        original_router.close().unwrap();
        stale_tx.send((rid, token)).unwrap();
    });

    let reply = dealer
        .request()
        .message(Message::try_from(b"request-before-recreate").unwrap())
        .timeout(std::time::Duration::from_secs(2))
        .submit_sync()
        .unwrap();
    assert_eq!(reply[0].as_bytes(), b"original-owner");
    let (rid, stale_token) = stale_rx.recv().unwrap();
    responder.join().unwrap();

    let recreated_router = ctx.router_socket().unwrap();
    let error = recreated_router
        .reply(&rid, stale_token)
        .message(Message::try_from(b"must-not-reach-native").unwrap())
        .submit()
        .unwrap_err();
    assert_eq!(error.code(), SubmitResult::InvalidArgument);
}
