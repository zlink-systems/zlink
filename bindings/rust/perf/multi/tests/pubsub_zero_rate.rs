#[allow(dead_code)]
#[path = "../src/perf_measurement.rs"]
mod measurement;

#[test]
fn zero_rate_cannot_catch_up() {
    let mut tracker = measurement::CatchUp::default();
    tracker.observe(0, &[0], 0, 0.0);
    assert!(!tracker.observe(500_000_000, &[0], 0, 0.0));
}
