#[allow(dead_code)]
#[path = "../src/perf_measurement.rs"]
mod measurement;

use measurement::CatchUp;

#[test]
fn per_subscriber_delta_allows_exactly_one_lost_observation() {
    for (increment, caught_up) in [(9, true), (8, false)] {
        let mut tracker = CatchUp::default();
        assert!(!tracker.observe(0, &[92, 100], 100));
        assert_eq!(
            tracker.observe(500_000_000, &[102, 100 + increment], 110),
            caught_up
        );
    }
}

#[test]
fn transient_in_flight_growth_restarts_the_window() {
    let mut tracker = CatchUp::default();
    assert!(!tracker.observe(0, &[92, 92], 100));
    assert!(!tracker.observe(100_000_000, &[93, 93], 102));
    assert!(!tracker.observe(500_000_000, &[102, 102], 110));
    assert!(tracker.observe(600_000_000, &[104, 104], 112));
}
