use std::time::Duration;
use zlink::{ConfigResult, Context, Message};

#[test]
fn options_reject_nonintegral_milliseconds_and_overflow() {
    let ctx = Context::new().unwrap();
    let router = ctx.create_router_socket().unwrap();
    let dealer = ctx.create_dealer_socket().unwrap();
    for invalid in [
        Duration::from_micros(1500),
        Duration::from_millis(i32::MAX as u64 + 1),
    ] {
        assert_eq!(
            router
                .router_options()
                .set_request_timeout(invalid)
                .unwrap_err()
                .code,
            ConfigResult::InvalidArgument
        );
        assert_eq!(
            dealer
                .dealer_options()
                .set_request_timeout(invalid)
                .unwrap_err()
                .code,
            ConfigResult::InvalidArgument
        );
        assert_eq!(
            dealer
                .common_options()
                .set_connect_timeout(invalid)
                .unwrap_err()
                .code,
            ConfigResult::InvalidArgument
        );
    }
    router
        .router_options()
        .set_request_timeout(Duration::from_millis(1))
        .unwrap();
    assert_eq!(
        router.router_options().request_timeout().unwrap(),
        Duration::from_millis(1)
    );
}

#[test]
fn request_rejects_nonintegral_timeout_before_submission() {
    let ctx = Context::new().unwrap();
    let dealer = ctx.create_dealer_socket().unwrap();
    let error = dealer
        .request()
        .message(Message::try_from(b"request").unwrap())
        .timeout(Duration::from_nanos(1))
        .submit_sync()
        .err()
        .expect("invalid timeout");
    assert!(
        matches!(error, zlink::ZlinkError::Submit(error) if error.code == zlink::SubmitResult::InvalidArgument),
        "{error:?}"
    );
}

#[cfg(target_os = "linux")]
#[test]
fn a_negative_native_duration_is_not_exposed_as_a_large_positive_value() {
    const CHILD: &str = "ZLINK_RUST_NEGATIVE_DURATION_CHILD";
    if std::env::var_os(CHILD).is_some() {
        let ctx = Context::new().unwrap();
        let socket = ctx.create_pair_socket().unwrap();
        let error = socket.common_options().reconnect_interval().unwrap_err();
        assert_eq!(error.code(), ConfigResult::InvalidArgument);
        return;
    }

    struct ShimDirectory(std::path::PathBuf);
    impl Drop for ShimDirectory {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
    let directory = ShimDirectory(
        std::env::temp_dir().join(format!("zlink-rust-duration-{}", std::process::id())),
    );
    std::fs::create_dir(&directory.0).unwrap();
    let shim = directory.0.join("duration_option_shim.so");
    let manifest = env!("CARGO_MANIFEST_DIR");
    let compiler = std::env::var_os("CC").unwrap_or_else(|| "cc".into());
    let build = std::process::Command::new(compiler)
        .args(["-shared", "-fPIC", "-o"])
        .arg(&shim)
        .arg("-I")
        .arg(format!("{manifest}/include"))
        .arg(format!(
            "{manifest}/tests/test_support/duration_option_shim.c"
        ))
        .arg("-ldl")
        .output()
        .unwrap();
    assert!(
        build.status.success(),
        "{}",
        String::from_utf8_lossy(&build.stderr)
    );

    let output = std::process::Command::new(std::env::current_exe().unwrap())
        .args([
            "--exact",
            "a_negative_native_duration_is_not_exposed_as_a_large_positive_value",
        ])
        .env("LD_PRELOAD", shim)
        .env(CHILD, "1")
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stdout)
    );
}
