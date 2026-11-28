use zlink::{Message, Received, SubmitResult};

#[test]
fn empty_received_rejects_send_and_reply_at_the_terminal() {
    let received = Received::empty();
    let send = received.send().message(Message::try_from(b"send").unwrap());
    let error = match send.submit() {
        Err(error) => error,
        Ok(_) => panic!("send without a target succeeded"),
    };
    assert_eq!(error.code, SubmitResult::InvalidArgument);
    let error = received
        .send()
        .message(Message::try_from(b"blocking send").unwrap())
        .submit_sync()
        .unwrap_err();
    assert_eq!(error.code, SubmitResult::InvalidArgument);
    let reply = received
        .reply()
        .message(Message::try_from(b"reply").unwrap());
    assert_eq!(
        reply.submit().unwrap_err().code,
        SubmitResult::InvalidArgument
    );
}
