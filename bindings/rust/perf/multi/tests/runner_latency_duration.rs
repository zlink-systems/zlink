use std::path::Path;
use std::process::Command;

#[test]
fn runner_parses_latency_duration_before_building() {
    let runner = Path::new(env!("CARGO_MANIFEST_DIR")).join("../run_benchmarks_multi.sh");
    // The conflicting build options stop the runner after parsing and before any build.
    for (args, env_value, expected) in [
        (&["--latency-duration", "3"][..], "0", "mutually exclusive"),
        (&["--latency-duration=009"][..], "", "mutually exclusive"),
        (&["--duration", "7"][..], "", "mutually exclusive"),
        (&[][..], "3", "mutually exclusive"),
        (&[][..], "invalid", "must be a positive integer"),
        (
            &["--latency-duration=0"][..],
            "",
            "must be a positive integer",
        ),
        (
            &["--latency-duration="][..],
            "",
            "must be a positive integer",
        ),
        (&["--latency-duration"][..], "", "requires a value"),
    ] {
        let output = Command::new("bash")
            .arg(&runner)
            .args(["--reuse-build", "--clean-build"])
            .args(args)
            .env("ZLINK_CORE_SOURCE", "local")
            .env("PERF_MULTI_DURATION_SECONDS", "5")
            .env("PERF_MULTI_LATENCY_DURATION_SECONDS", env_value)
            .output()
            .expect("run the multi runner parser");
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(
            !output.status.success() && stderr.contains(expected),
            "{args:?} with env {env_value:?}: {stderr}"
        );
    }
}
