use std::time::Duration;

use zlink::{ConfigResult, Context};

#[test]
fn common_socket_times_round_trip_finite_and_unlimited_values() {
    let context = Context::new().unwrap();
    let socket = context.create_pair_socket().unwrap();
    let options = socket.common_options();

    assert_eq!(options.linger().unwrap(), None);
    assert_eq!(
        options.send_timeout().unwrap(),
        Some(Duration::from_secs(1))
    );
    assert_eq!(
        options.receive_timeout().unwrap(),
        Some(Duration::from_secs(1))
    );

    for value in [
        None,
        Some(Duration::ZERO),
        Some(Duration::from_millis(37)),
        Some(Duration::from_millis(i32::MAX as u64)),
    ] {
        options.set_linger(value).unwrap();
        options.set_send_timeout(value).unwrap();
        options.set_receive_timeout(value).unwrap();
        assert_eq!(options.linger().unwrap(), value);
        assert_eq!(options.send_timeout().unwrap(), value);
        assert_eq!(options.receive_timeout().unwrap(), value);
    }

    options.set_linger(Some(Duration::ZERO)).unwrap();
}

#[test]
fn common_socket_times_reject_lossy_values_without_changing_options() {
    let context = Context::new().unwrap();
    let socket = context.create_pair_socket().unwrap();
    let options = socket.common_options();
    let original = Some(Duration::from_millis(37));
    options.set_linger(original).unwrap();
    options.set_send_timeout(original).unwrap();
    options.set_receive_timeout(original).unwrap();

    for value in [
        Some(Duration::from_nanos(1)),
        Some(Duration::from_millis(i32::MAX as u64 + 1)),
    ] {
        assert_eq!(
            options.set_linger(value).unwrap_err().code(),
            ConfigResult::InvalidArgument
        );
        assert_eq!(
            options.set_send_timeout(value).unwrap_err().code(),
            ConfigResult::InvalidArgument
        );
        assert_eq!(
            options.set_receive_timeout(value).unwrap_err().code(),
            ConfigResult::InvalidArgument
        );
        assert_eq!(options.linger().unwrap(), original);
        assert_eq!(options.send_timeout().unwrap(), original);
        assert_eq!(options.receive_timeout().unwrap(), original);
    }
}
