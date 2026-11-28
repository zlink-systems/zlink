use zlink::{
    BindError, BindResult, CloseError, CloseResult, ConfigError, ConfigResult, ConnectError,
    ConnectResult, HandlerError, HandlerResult, RecvError, RecvResult, RequestError, RequestResult,
    SubmitError, SubmitResult, ZlinkError,
};

#[test]
fn typed_errors_expose_internal_errno_as_field_and_accessor() {
    macro_rules! check_error {
        ($error:ident, $result:ident) => {{
            let code = $result::InternalError;
            let error = $error {
                code,
                internal_errno: libc::EIO,
            };
            assert_eq!(error, $error::new(code, libc::EIO));
            assert_eq!(error.internal_errno(), error.internal_errno);
            assert_eq!(ZlinkError::from(error).internal_errno(), libc::EIO);
            assert!(error.to_string().contains("internal_errno="));
        }};
    }

    check_error!(SubmitError, SubmitResult);
    check_error!(RequestError, RequestResult);
    check_error!(RecvError, RecvResult);
    check_error!(HandlerError, HandlerResult);
    check_error!(CloseError, CloseResult);
    check_error!(BindError, BindResult);
    check_error!(ConnectError, ConnectResult);
    check_error!(ConfigError, ConfigResult);
}
