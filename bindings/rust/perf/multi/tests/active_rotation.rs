#[allow(dead_code)]
#[path = "../src/perf_measurement.rs"]
mod measurement;

use measurement::SenderRotation;

#[test]
fn active_senders_yield_at_64_or_when_blocked() {
    let mut rotation = SenderRotation::default();
    for _ in 0..64 {
        assert_eq!(rotation.slot, 0);
        assert!(rotation.can_submit());
        rotation.submitted();
    }
    assert!(!rotation.can_submit());

    rotation.advance(2);
    assert_eq!(rotation.slot, 1);
    assert!(rotation.can_submit());
    rotation.submitted();
    rotation.advance(2); // The second sender blocked.
    assert_eq!(rotation.slot, 0);
    assert!(rotation.can_submit());
}
