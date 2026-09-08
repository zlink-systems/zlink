//! Shared receive progression for single one-way benchmarks.

pub enum RecvStep {
    Payload,
    Empty,
    Stop,
}

pub enum Content<'a> {
    Payload(&'a [u8]),
    Stop,
}

pub fn classify(parts: &[zlink::Message]) -> Content<'_> {
    if parts.len() == 1 && crate::common::is_stop_token(parts[0].as_bytes()) {
        return Content::Stop;
    }
    let payload = crate::common::message_payload(parts);
    assert!(
        !payload.is_empty(),
        "invalid one-way application frame shape"
    );
    Content::Payload(payload)
}

/// Match the C one-way receive model: wait for POLLIN on this thread, then
/// drain every currently readable message with DONT_WAIT until the wire stop
/// token is observed.
pub fn recv_until_stop(socket: &dyn zlink::Pollable, mut recv_one: impl FnMut() -> RecvStep) {
    let poller = zlink::Poller::new().expect("receiver poller");
    poller
        .add_socket(socket, zlink::POLLIN, 0)
        .expect("receiver poller registration");
    let mut events = [zlink::PollEvent::default()];

    loop {
        match poller.wait(&mut events, -1) {
            Ok(0) => continue,
            Ok(_) => {}
            Err(error)
                if error.native_errno() == libc::EINTR || error.native_errno() == libc::EAGAIN =>
            {
                continue;
            }
            Err(error) => panic!("receiver poller wait: {error}"),
        }

        loop {
            match recv_one() {
                RecvStep::Payload => continue,
                RecvStep::Empty => break,
                RecvStep::Stop => return,
            }
        }
    }
}
