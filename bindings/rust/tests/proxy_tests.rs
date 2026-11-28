use zlink::{ConfigResult, Context, proxy};

#[test]
fn proxy_reports_an_invalid_socket_as_a_config_error() {
    let ctx = Context::new().unwrap();
    let mut frontend = ctx.create_pair_socket().unwrap();
    let backend = ctx.create_pair_socket().unwrap();
    frontend.close().unwrap();
    assert_eq!(
        proxy(&frontend, &backend, None).unwrap_err().code(),
        ConfigResult::InvalidHandle
    );
}
