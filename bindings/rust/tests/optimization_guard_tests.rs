use std::fs;
use std::mem::size_of;
use std::path::{Path, PathBuf};

use zlink::Message;

const AGGREGATE_SYMBOLS: &[&str] = &[
    "zlink_send",
    "zlink_recv",
    "zlink_publish",
    "zlink_subscribe",
    "zlink_router_recv",
    "zlink_dealer_request",
    "zlink_router_request",
    "zlink_router_reply",
];

const REQUIRED_PART_SYMBOLS: &[&str] = &[
    "zlink_send_part",
    "zlink_recv_part",
    "zlink_publish_part",
    "zlink_subscribe_part",
    "zlink_router_recv_part",
    "zlink_dealer_request_part",
    "zlink_router_request_part",
    "zlink_router_reply_part",
    "zlink_send_part_rid",
    "zlink_xpub_recv_part",
];

fn source_files(root: &Path) -> Vec<PathBuf> {
    fn visit(dir: &Path, out: &mut Vec<PathBuf>) {
        for entry in fs::read_dir(dir).unwrap() {
            let entry = entry.unwrap();
            let path = entry.path();
            if path.is_dir() {
                visit(&path, out);
            } else if path.extension().and_then(|ext| ext.to_str()) == Some("rs") {
                out.push(path);
            }
        }
    }

    let mut files = Vec::new();
    visit(root, &mut files);
    files
}

#[test]
fn hot_paths_use_part_substrate_not_aggregate_calls() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
    let files = source_files(&root);
    let all = files
        .iter()
        .map(|path| fs::read_to_string(path).unwrap())
        .collect::<Vec<_>>()
        .join("\n");

    for symbol in REQUIRED_PART_SYMBOLS {
        assert!(
            all.contains(symbol),
            "missing required helper substrate {symbol}"
        );
    }

    let mut violations = Vec::new();
    for path in files {
        let body = fs::read_to_string(&path).unwrap();
        for symbol in AGGREGATE_SYMBOLS {
            let needle = format!("ffi::{symbol}(");
            let mut start = 0;
            while let Some(offset) = body[start..].find(&needle) {
                let idx = start + offset;
                if !body[idx..].starts_with(&format!("ffi::{symbol}_part")) {
                    violations.push(format!("{}:{symbol}", path.display()));
                }
                start = idx + needle.len();
            }
        }
    }

    assert!(violations.is_empty(), "{violations:#?}");
}

#[test]
fn binding_runtime_does_not_hide_sleep_or_join_in_hot_paths() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
    for path in source_files(&root) {
        let body = fs::read_to_string(&path).unwrap();
        if path.file_name().and_then(|name| name.to_str()) == Some("runtime.rs") {
            continue;
        }
        assert!(
            !body.contains("thread::sleep(") && !body.contains("std::thread::sleep("),
            "{} contains hidden sleep",
            path.display()
        );
    }
}

#[test]
fn message_owns_the_native_record_inline() {
    assert_eq!(size_of::<Message>(), 64);

    let body = fs::read_to_string(
        Path::new(env!("CARGO_MANIFEST_DIR")).join("src/runtime/messaging/message.rs"),
    )
    .unwrap();
    assert!(!body.contains("Box::new(raw)"));
    assert!(!body.contains("Box::from_raw(message.inner"));
}

#[test]
fn binding_does_not_duplicate_core_completion_progress() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
    let all = source_files(&root)
        .iter()
        .map(|path| fs::read_to_string(path).unwrap())
        .collect::<Vec<_>>()
        .join("\n");

    assert!(!all.contains("RequestProgressGuard"));
    assert!(!all.contains("ProgressWorker"));
    assert!(!all.contains("zlink-rust-progress"));
}

#[test]
fn deferred_cleanup_uses_fair_queue_order() {
    let body = fs::read_to_string(
        Path::new(env!("CARGO_MANIFEST_DIR")).join("src/internal/deferred_cleanup.rs"),
    )
    .unwrap();
    assert!(body.contains("VecDeque<DeferredCleanup>"));
    assert!(body.contains("pop_front()"));
    assert!(body.contains("push_back(cleanup)"));
}

#[test]
fn samples_and_perf_use_only_public_binding_contract() {
    let manifest_dir = Path::new(env!("CARGO_MANIFEST_DIR"));
    let roots = [
        manifest_dir.join("samples"),
        manifest_dir.join("perf").join("single").join("src"),
        manifest_dir.join("perf").join("multi").join("src"),
    ];
    let forbidden = [
        "zlink::ffi",
        "zlink::runtime",
        "zlink::ctx",
        "zlink::socket",
        "zlink::spot",
        "zlink::runtime_bridge",
        "zlink::native_errors",
        "zlink::native_routing_id",
        "crate::ffi",
        "crate::runtime",
        "crate::ctx",
        "crate::socket",
        "crate::spot",
        "crate::runtime_bridge",
        "crate::native_errors",
        "crate::native_routing_id",
        "extern \"C\"",
        "unsafe {",
    ];

    let mut violations = Vec::new();
    for root in roots {
        for path in source_files(&root) {
            let body = fs::read_to_string(&path).unwrap();
            for token in forbidden {
                if body.contains(token) {
                    violations.push(format!("{}:{token}", path.display()));
                }
            }
        }
    }

    assert!(violations.is_empty(), "{violations:#?}");
}
