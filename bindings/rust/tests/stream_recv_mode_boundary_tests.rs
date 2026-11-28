use zlink::{ConfigResult, Context, StreamRecvMode};

#[test]
fn unspecified_is_observable_but_cannot_be_set_as_a_receive_mode() {
    let ctx = Context::new().unwrap();
    let stream = ctx.create_stream_socket().unwrap();
    let options = stream.stream_options();
    assert_eq!(options.recv_mode().unwrap(), StreamRecvMode::Unspecified);
    options.set_recv_mode(StreamRecvMode::Raw).unwrap();
    let error = options
        .set_recv_mode(StreamRecvMode::Unspecified)
        .unwrap_err();
    assert_eq!(error.code(), ConfigResult::InvalidArgument);
    assert_eq!(options.recv_mode().unwrap(), StreamRecvMode::Raw);
}
