#[allow(dead_code)]
#[path = "../src/perf_measurement.rs"]
mod measurement;

#[test]
fn active_receive_excludes_the_deadline_and_tail() {
    assert!(measurement::active_receive(99, 100));
    assert!(!measurement::active_receive(100, 100));
    assert!(!measurement::active_receive(101, 100));
}
