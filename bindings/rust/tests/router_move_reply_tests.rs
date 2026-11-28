mod test_support;

use std::time::Duration;
use zlink::{Context, Message};

#[test]
fn a_token_received_before_the_router_moves_still_identifies_its_owner() {
    let ctx = Context::new().unwrap();
    let endpoint = "inproc://rust-router-move-reply";
    let router = ctx.create_router_socket().unwrap();
    router.bind(endpoint).unwrap();
    let (_dealer, _driver, _, pending_reply, request) = test_support::request_until_received(
        &ctx,
        &router,
        endpoint,
        b"request",
        Duration::from_secs(2),
    );
    let source = *request.routing_id().unwrap();
    let token = request.reply_token().unwrap();
    std::thread::spawn(move || {
        router
            .reply(&source, token)
            .message(Message::try_from(b"reply").unwrap())
            .submit()
            .unwrap();
    })
    .join()
    .unwrap();
    let parts = test_support::block_on(pending_reply).unwrap();
    assert_eq!(parts[0].as_bytes(), b"reply");
}
