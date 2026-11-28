use zlink::{Context, Message, SubmitResult, ZlinkError};

#[test]
fn builders_created_before_a_routed_socket_close_report_termination() {
    let ctx = Context::new().unwrap();
    let mut dealer = ctx.create_dealer_socket().unwrap();
    let send = dealer.send().message(Message::try_from(b"send").unwrap());
    let request = dealer
        .request()
        .message(Message::try_from(b"request").unwrap());
    dealer.close().unwrap();
    let error = match send.submit() {
        Err(error) => error,
        Ok(_) => panic!("send after close succeeded"),
    };
    assert_eq!(error.code, SubmitResult::Terminated);
    let error = request
        .submit_sync()
        .err()
        .expect("request after close failed");
    assert!(
        matches!(error, ZlinkError::Submit(error) if error.code == SubmitResult::Terminated),
        "{error:?}"
    );
}
