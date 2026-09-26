#![cfg(target_os = "linux")]

use std::process::Command;

use zlink::{CloseResult, Context, POLLIN, Poller};

fn run_with_shim(test: &str, poller_busy: bool) -> String {
    let directory =
        std::env::temp_dir().join(format!("zlink-rust-close-{}-{test}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let shim = directory.join("close_contract_shim.so");
    let trace = directory.join("close.log");
    let source = concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/tests/test_support/close_contract_shim.c"
    );
    assert!(
        Command::new("cc")
            .args(["-shared", "-fPIC", "-o"])
            .arg(&shim)
            .arg(source)
            .arg("-ldl")
            .status()
            .unwrap()
            .success()
    );
    let mut child = Command::new(std::env::current_exe().unwrap());
    child
        .args(["--exact", test, "--test-threads=1"])
        .env("LD_PRELOAD", &shim)
        .env("ZLINK_RUST_CLOSE_CHILD", "1")
        .env("ZLINK_RUST_CLOSE_TRACE", &trace);
    if poller_busy {
        child.env("ZLINK_RUST_POLLER_BUSY_ONCE", "1");
    }
    let output = child.output().unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stdout)
    );
    let calls = std::fs::read_to_string(&trace).unwrap_or_default();
    std::fs::remove_dir_all(directory).unwrap();
    calls
}

#[test]
fn context_close_shutdown_precedes_term() {
    if std::env::var_os("ZLINK_RUST_CLOSE_CHILD").is_none() {
        assert_eq!(
            run_with_shim("context_close_shutdown_precedes_term", false),
            "ST"
        );
        return;
    }
    drop(Context::new().unwrap());
}

#[test]
fn poller_busy_close_keeps_registration_and_allows_retry() {
    if std::env::var_os("ZLINK_RUST_CLOSE_CHILD").is_none() {
        assert_eq!(
            run_with_shim(
                "poller_busy_close_keeps_registration_and_allows_retry",
                true
            ),
            "ST"
        );
        return;
    }
    let context = Context::new().unwrap();
    let socket = context.pair_socket().unwrap();
    let mut poller = Poller::new().unwrap();
    poller.add_socket(&socket, POLLIN, 1).unwrap();
    let busy = poller.close().unwrap_err();
    assert_eq!(busy.code(), CloseResult::Busy);
    assert_eq!(busy.native_errno(), libc::EBUSY);
    assert_eq!(poller.size().unwrap(), 1);
    poller.close().unwrap();
}
