//! Projection of the Core connection-attempt snapshot (Core socket API):
//! `connection_attempts_snapshot` and `POLLCONNECT`. Scenario mirrors the C++
//! binding's `connection_attempts_snapshot`.

use std::net::TcpListener;
use std::time::{Duration, Instant};

use zlink::{
    ConnectionAttempt, ConnectionAttemptState, Context, DealerSocket, POLLCONNECT, PollEvent,
    Poller,
};

const WAIT_MS: i64 = 5_000;

fn tcp_endpoint() -> String {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    format!("tcp://{}", listener.local_addr().unwrap())
}

fn wait_state(socket: &DealerSocket, state: ConnectionAttemptState) -> ConnectionAttempt {
    let deadline = Instant::now() + Duration::from_millis(WAIT_MS as u64);
    loop {
        let rows = socket.connection_attempts_snapshot().unwrap();
        if let Some(row) = rows.into_iter().find(|row| row.state == state) {
            return row;
        }
        assert!(Instant::now() < deadline, "state {state:?} not reached");
        std::thread::sleep(Duration::from_millis(5));
    }
}

#[test]
fn snapshot_reports_connected_row_and_pollconnect() {
    let ctx = Context::new().unwrap();
    let server = ctx.create_dealer_socket().unwrap();
    let client = ctx.create_dealer_socket().unwrap();
    let endpoint = tcp_endpoint();
    server.bind(&endpoint).unwrap();

    assert!(client.connection_attempts_snapshot().unwrap().is_empty());

    let poller = Poller::new().unwrap();
    poller.add_socket(&client, POLLCONNECT, 3).unwrap();
    client.connect(&endpoint).unwrap();

    let mut events = [PollEvent::default()];
    assert_eq!(poller.wait(&mut events, WAIT_MS).unwrap(), 1);
    assert_eq!(events[0].slot, 3);
    assert_ne!(events[0].revents & POLLCONNECT, 0);

    let row = wait_state(&client, ConnectionAttemptState::Connected);
    assert_eq!(row.endpoint, endpoint);
    assert_ne!(row.intent_id, 0);
    assert_ne!(row.revision, 0);
}

#[test]
fn snapshot_reports_failed_row_with_failure() {
    let ctx = Context::new().unwrap();
    let client = ctx.create_dealer_socket().unwrap();
    client
        .common_options()
        .set_linger(Some(Duration::ZERO))
        .unwrap();
    let endpoint = tcp_endpoint();
    client.connect(&endpoint).unwrap();

    let row = wait_state(&client, ConnectionAttemptState::AttemptFailed);
    assert_eq!(row.endpoint, endpoint);
    assert_ne!(row.last_failure_id, 0);
    assert_ne!(row.last_failure.normalized_errno, 0);
}
