#[allow(dead_code)]
#[path = "../src/perf_measurement.rs"]
mod measurement;

#[test]
fn paced_reply_progress_runs_at_64_and_128() {
    let mut replies = 0;
    let rechecks: Vec<_> = (1..=130)
        .filter(|_| measurement::reply_turn(&mut replies))
        .collect();
    assert_eq!(rechecks, [64, 128]);
}
