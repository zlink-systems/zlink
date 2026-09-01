//! Contract tests – verify FFI/native call mapping, type conversions,
//! and resource lifecycle.

mod test_support;

use std::io::Write;
use std::process::{Command, Stdio};
use std::sync::{Arc, Barrier};
use std::thread;

use zlink::{
    ConfigResult, Context, DealerSocket, Message, POLLCOMPLETION, POLLIN, Poller, Received,
    RecvFlags, RouterSocket, RoutingId, SendFlags, SubmitResult, has, version,
};

fn packaged_core_version() -> (i32, i32, i32) {
    let header = std::fs::read_to_string(format!(
        "{}/include/zlink.h",
        env!("CARGO_MANIFEST_DIR")
    ))
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
    let _router_reply = |socket: &RouterSocket, rid: &RoutingId, seq: u64, msg: Message| {
        socket.reply(rid, seq).message(msg).submit()
    };
}

#[test]
fn concurrent_multipart_publish_exposes_core_rejection_and_releases_parts() {
    const WORKERS: usize = 8;
    const PER_WORKER: usize = 500;

    let ctx = Context::new().unwrap();
    let publisher = ctx.pub_socket().unwrap();

    let mut work = (0..WORKERS).map(|_| Vec::new()).collect::<Vec<_>>();
    for worker in 0..WORKERS {
        for index in 0..PER_WORKER {
            let first_bytes = format!("record-{worker}-{index:03}-a").into_bytes();
            let second_bytes = format!("record-{worker}-{index:03}-b").into_bytes();
            let first = Message::try_from(first_bytes.as_slice()).unwrap();
            let second = Message::try_from(second_bytes.as_slice()).unwrap();
            let first_owner = first.try_clone().unwrap();
            let second_owner = second.try_clone().unwrap();
            let publish = publisher
                .publish("contract")
                .message(first)
                .message(second);
            work[worker].push((publish, first_owner, second_owner, first_bytes, second_bytes));
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
                                && error.native_errno() == libc::EINVAL => {
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
fn poller_modify_rejects_completion_ownership_changes() {
    let ctx = Context::new().unwrap();
    let dealer = ctx.dealer_socket().unwrap();
    let poller = Poller::new().unwrap();

    poller.add_socket(&dealer, POLLCOMPLETION, 1).unwrap();
    let remove_completion = poller.modify_socket(&dealer, POLLIN).unwrap_err();
    assert_eq!(remove_completion.code(), ConfigResult::InvalidArgument);
    poller.remove_socket(&dealer).unwrap();

    poller.add_socket(&dealer, POLLIN, 2).unwrap();
    let add_completion = poller
        .modify_socket(&dealer, POLLIN | POLLCOMPLETION)
        .unwrap_err();
    assert_eq!(add_completion.code(), ConfigResult::InvalidArgument);
}

#[test]
fn request_router_exposes_request_sequence() {
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
    let request_seq = received.request_seq();
    assert_eq!(received.single_part().unwrap().as_bytes(), b"plain-data");
    assert_eq!(request_seq, None);
}

#[test]
fn router_reply_with_non_empty_flags_fails_explicitly() {
    let ctx = Context::new().unwrap();
    let router = ctx.router_socket().unwrap();
    let rid = RoutingId::from(b"peer-42");
    let err = router
        .reply(&rid, 1)
        .message(Message::try_from(b"pong").unwrap())
        .flags(SendFlags::DONT_WAIT)
        .submit()
        .unwrap_err();
    assert_eq!(err.code(), SubmitResult::NotSupported);
}
