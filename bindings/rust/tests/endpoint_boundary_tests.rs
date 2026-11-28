use zlink::{BindResult, ConnectResult, Context};

#[test]
fn raw_connection_operations_reject_endpoints_over_255_bytes() {
    let ctx = Context::new().unwrap();
    let socket = ctx.create_pair_socket().unwrap();
    let endpoint = format!("inproc://{}", "x".repeat(246));
    assert_eq!(endpoint.len(), 255);
    socket.bind(&endpoint).unwrap();
    socket.unbind(&endpoint).unwrap();
    let endpoint = format!("{}é", &endpoint[..254]);
    assert_eq!(endpoint.len(), 256);
    assert_eq!(
        socket.bind(&endpoint).unwrap_err().code,
        BindResult::InvalidArgument
    );
    assert_eq!(
        socket.connect(&endpoint).unwrap_err().code,
        ConnectResult::InvalidArgument
    );
    assert_eq!(
        socket.unbind(&endpoint).unwrap_err().code,
        ConnectResult::InvalidArgument
    );
    assert_eq!(
        socket.disconnect(&endpoint).unwrap_err().code,
        ConnectResult::InvalidArgument
    );
}
