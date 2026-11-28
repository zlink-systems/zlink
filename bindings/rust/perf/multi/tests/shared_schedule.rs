#[allow(dead_code)]
#[path = "../src/perf_measurement.rs"]
mod measurement;

#[test]
fn schedule_is_anchored_to_epoch_and_waits_floor_to_ms() {
    let epoch = 1_000_000_000;
    let mut schedule = measurement::Schedule::new(epoch, 100, 2);
    assert_eq!(schedule.rate, 40.0);
    assert!(!schedule.due(epoch - 1));
    assert_eq!(schedule.reserve(), epoch);
    assert_eq!(schedule.reserve(), epoch + 25_000_000);
    assert_eq!(schedule.next(), epoch + 50_000_000);
    assert_eq!(measurement::wait_ms(epoch, epoch + 999_999), 0);
    assert_eq!(measurement::wait_ms(epoch, epoch + 1_999_999), 1);
    assert_eq!(measurement::wait_ms(epoch, epoch + 150_000_000), 50);
}
