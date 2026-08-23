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
    "zlink_recv_part_with_hwm_budget_lease",
    "zlink_dealer_recv_part_with_hwm_budget_lease",
    "zlink_router_recv_part_v2_with_hwm_budget_lease",
    "zlink_subscribe_part_with_hwm_budget_lease",
    "zlink_hwm_budget_lease_release",
    "zlink_send_complete_handler",
    "zlink_send_async",
    "zlink_send_async_cancel",
    "zlink_select_routed_submit_target",
    "zlink_send_part_transport_pair",
    "zlink_dealer_send_transport_pair_part",
    "zlink_router_request_transport_pair_part",
    "zlink_dealer_request_transport_pair_part",
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
fn routed_async_uses_the_existing_exact_part_contract() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
    let all = source_files(&root)
        .iter()
        .map(|path| fs::read_to_string(path).unwrap())
        .collect::<Vec<_>>()
        .join("\n");

    assert!(!all.contains("zlink_routed_send_parts"));
    assert!(!all.contains("zlink_routed_request_parts"));
    assert!(!all.contains("submit_async"));
    assert!(!all.contains("RequestCallback"));
}

/// The 0.13.0 contract removed the `send_ready` readiness-hint surface. Nothing
/// in the binding may reference it any more.
#[test]
fn binding_has_no_send_ready_surface() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
    let mut violations = Vec::new();
    for path in source_files(&root) {
        let body = fs::read_to_string(&path).unwrap();
        if body.contains("send_ready") {
            violations.push(path.display().to_string());
        }
    }
    assert!(violations.is_empty(), "{violations:#?}");
}

/// `bindings/doc/spec/async-coroutine-policy.ko.md` (3rd revision): the binding
/// owns no thread, no park queue, no retry policy and no deadline timer. Core
/// drives every completion.
#[test]
fn binding_owns_no_thread_queue_or_deadline_machinery() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
    let forbidden = [
        "thread::spawn",
        "thread::Builder",
        "std::sync::mpsc",
        "BinaryHeap",
        "RoutedAdmission",
        "DeadlineToken",
        "RoutedWake",
    ];
    // `deferred_cleanup.rs` owns the one pre-existing worker in the binding.
    // It exists for the C callback-lifetime contract (`Drop` cannot report a
    // busy native close, and callback userdata must outlive Core's last use),
    // not for send admission, retry or deadlines.
    let lifecycle_worker = root.join("internal").join("deferred_cleanup.rs");
    let mut violations = Vec::new();
    for path in source_files(&root) {
        if path == lifecycle_worker {
            continue;
        }
        let body = fs::read_to_string(&path).unwrap();
        // The callback-lifecycle unit test spawns a thread to stand in for a
        // Core callback arriving on a foreign thread. That is test scaffolding,
        // not a binding-owned worker.
        let body = match body.find("#[cfg(test)]") {
            Some(offset) => body[..offset].to_string(),
            None => body,
        };
        for token in forbidden {
            if body.contains(token) {
                violations.push(format!("{}:{token}", path.display()));
            }
        }
    }
    assert!(violations.is_empty(), "{violations:#?}");
}

/// Publish is synchronous-only: PUB semantics are lossy and `zlink_send_async`
/// answers `ENOTSUP` for PUB/XPUB.
#[test]
fn publish_terminal_is_synchronous() {
    let body = fs::read_to_string(
        Path::new(env!("CARGO_MANIFEST_DIR")).join("src/contracts/messaging/operations.rs"),
    )
    .unwrap();
    assert!(body.contains("pub fn submit(self) -> Result<(), SubmitError> {"));
    assert!(!body.contains("PublishOp<Ready>::submit_async"));
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
