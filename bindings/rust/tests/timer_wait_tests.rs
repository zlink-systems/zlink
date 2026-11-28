use zlink::{PollEvent, PollSourceKind, Poller, Timer};

#[test]
fn one_shot_timer_wakes_an_indefinite_poller_wait() {
    let timer = Timer::new().unwrap();
    assert_eq!(timer.recv().unwrap(), None);
    let poller = Poller::new().unwrap();
    poller.add_timer(&timer, 73).unwrap();
    timer.start(1_000_000, 1).unwrap();
    let mut events = [PollEvent::default()];
    assert_eq!(poller.wait(&mut events, -1).unwrap(), 1);
    assert_eq!(events[0].source_kind, PollSourceKind::Timer);
    assert_eq!(events[0].slot, 73);
    assert_eq!(timer.recv().unwrap(), Some(1));
    assert_eq!(timer.recv().unwrap(), None);
    poller.remove_timer(&timer).unwrap();
}
