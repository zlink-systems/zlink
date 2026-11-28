// SPDX-License-Identifier: MPL-2.0

use zlink::{Context, Message, POLLCOMPLETION, Poller, SubmitResult};

#[test]
#[ignore = "requires the send_no_wait_token_shim LD_PRELOAD fixture"]
fn send_without_wait_token_returns_backpressure() {
    let ctx = Context::new().unwrap();
    let pair = ctx.create_pair_socket().unwrap();
    let poller = Poller::new().unwrap();
    poller.add_socket(&pair, POLLCOMPLETION, 0).unwrap();
    let result = pair
        .send()
        .message(Message::try_from(b"send").unwrap())
        .submit();
    let Err(error) = result else {
        panic!("SEND without a wait token returned a submission");
    };
    assert_eq!(
        (error.code(), error.internal_errno()),
        (SubmitResult::Backpressured, libc::EAGAIN)
    );
}
