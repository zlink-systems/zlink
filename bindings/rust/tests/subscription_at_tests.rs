// SPDX-License-Identifier: MPL-2.0

#![cfg(target_os = "linux")]

use std::{fs, process::Command};
use zlink::{ConfigResult, Context};

const CASE_ENV: &str = "ZLINK_RUST_SUBSCRIPTION_TEST";
const TRACE_ENV: &str = "ZLINK_RUST_SUBSCRIPTION_TRACE";
const LONG_FILTER_SIZE: usize = 4096;

fn check_with_shim(test: &str, results: &[ConfigResult]) {
    let directory = std::env::temp_dir().join(format!(
        "zlink-rust-subscription-{}-{test}",
        std::process::id()
    ));
    fs::create_dir(&directory).unwrap();
    let shim = directory.join("subscription_at_shim.so");
    let trace = directory.join("subscription_at.log");
    let manifest = env!("CARGO_MANIFEST_DIR");
    let build = Command::new("cc")
        .args(["-shared", "-fPIC", "-o"])
        .arg(&shim)
        .arg("-I")
        .arg(format!("{manifest}/include"))
        .arg(format!(
            "{manifest}/tests/test_support/subscription_at_shim.c"
        ))
        .arg("-ldl")
        .output()
        .unwrap();
    assert!(
        build.status.success(),
        "{}",
        String::from_utf8_lossy(&build.stderr)
    );

    let output = Command::new(std::env::current_exe().unwrap())
        .args(["--exact", test, "--test-threads=1"])
        .env("LD_PRELOAD", shim)
        .env(CASE_ENV, test)
        .env(TRACE_ENV, &trace)
        .output()
        .unwrap();
    let calls = fs::read_to_string(trace);
    fs::remove_dir_all(directory).unwrap();
    assert!(
        output.status.success(),
        "{}\nCore calls (buffer capacity, result): {:?}",
        String::from_utf8_lossy(&output.stdout),
        calls,
    );
    let calls: Vec<(usize, i32)> = calls
        .unwrap()
        .lines()
        .map(|line| {
            let (capacity, result) = line.split_once(' ').unwrap();
            (capacity.parse().unwrap(), result.parse().unwrap())
        })
        .collect();
    assert_eq!(
        calls.iter().map(|&(_, result)| result).collect::<Vec<_>>(),
        results
            .iter()
            .map(|&result| result as i32)
            .collect::<Vec<_>>(),
        "Core calls (buffer capacity, result): {calls:?}"
    );
    assert!(
        calls[0].0 > 0,
        "first Core call must supply a buffer: {calls:?}"
    );
    if results.len() == 2 {
        assert_eq!(calls[1].0, LONG_FILTER_SIZE);
    }
}

#[test]
fn missing() {
    if std::env::var_os(CASE_ENV).is_none() {
        return check_with_shim("missing", &[ConfigResult::NotFound]);
    }
    let context = Context::new().unwrap();
    let socket = context.create_sub_socket().unwrap();
    assert_eq!(socket.subscription_at(0).unwrap(), None);
}

#[test]
fn empty() {
    if std::env::var_os(CASE_ENV).is_none() {
        return check_with_shim("empty", &[ConfigResult::Ok]);
    }
    let context = Context::new().unwrap();
    let socket = context.create_xsub_socket().unwrap();
    socket.set_subscription("").unwrap();
    assert_eq!(
        socket.subscription_at(0).unwrap(),
        Some((String::new(), false))
    );
}

#[test]
fn long() {
    if std::env::var_os(CASE_ENV).is_none() {
        return check_with_shim("long", &[ConfigResult::BufferTooSmall, ConfigResult::Ok]);
    }
    let context = Context::new().unwrap();
    let socket = context.create_sub_socket().unwrap();
    let filter = "x".repeat(LONG_FILTER_SIZE);
    socket.set_subscription(&filter).unwrap();
    assert_eq!(socket.subscription_at(0).unwrap(), Some((filter, false)));
}

#[test]
fn removed_before_retry() {
    if std::env::var_os(CASE_ENV).is_none() {
        return check_with_shim(
            "removed_before_retry",
            &[ConfigResult::BufferTooSmall, ConfigResult::NotFound],
        );
    }
    let context = Context::new().unwrap();
    let socket = context.create_xsub_socket().unwrap();
    socket
        .set_subscription(&"x".repeat(LONG_FILTER_SIZE))
        .unwrap();
    assert_eq!(socket.subscription_at(0).unwrap(), None);
}

#[test]
fn invalid_argument() {
    if std::env::var_os(CASE_ENV).is_none() {
        return check_with_shim("invalid_argument", &[ConfigResult::InvalidArgument]);
    }
    let context = Context::new().unwrap();
    let socket = context.create_sub_socket().unwrap();
    let error = socket.subscription_at(0).unwrap_err();
    assert_eq!(error.code(), ConfigResult::InvalidArgument);
    assert_eq!(error.internal_errno(), libc::EINVAL);
}
