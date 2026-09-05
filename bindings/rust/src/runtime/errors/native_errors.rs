use crate::error::{
    BindError, BindResult, CloseError, CloseResult, ConfigError, ConfigResult, ConnectError,
    ConnectResult, RecvError, RecvResult, RequestError, RequestResult, SubmitError, SubmitResult,
};
use crate::ffi;

pub(crate) fn last_errno() -> i32 {
    unsafe { ffi::zlink_errno() }
}

pub(crate) fn submit_result_from_errno(err: i32) -> SubmitResult {
    match err {
        0 => SubmitResult::Ok,
        libc::EAGAIN | libc::ETIMEDOUT | libc::ENOBUFS => SubmitResult::Backpressured,
        libc::ENOTCONN | libc::EHOSTUNREACH => SubmitResult::NotConnected,
        libc::ENOENT => SubmitResult::NotFound,
        libc::EACCES | libc::ECONNREFUSED | libc::EPROTOTYPE => SubmitResult::NotAdmitted,
        libc::ECANCELED | libc::ESHUTDOWN => SubmitResult::Terminated,
        x if x == eterm() => SubmitResult::Terminated,
        libc::EFAULT => SubmitResult::InvalidHandle,
        libc::EINVAL | libc::EMSGSIZE => SubmitResult::InvalidArgument,
        libc::ENOTSUP => SubmitResult::NotSupported,
        x if x == eopnotsupp() => SubmitResult::NotSupported,
        libc::EBUSY | libc::ESTALE | libc::EALREADY => SubmitResult::InvalidState,
        libc::EDEADLK | libc::EPERM => SubmitResult::ThreadViolation,
        x if x == emthread() => SubmitResult::ThreadViolation,
        libc::ENOMEM => SubmitResult::OutOfMemory,
        libc::EOVERFLOW => SubmitResult::SeqExhausted,
        _ => SubmitResult::InternalError,
    }
}

pub(crate) fn submit_error_from_errno(err: i32) -> SubmitError {
    SubmitError::new(submit_result_from_errno(err), err)
}

pub(crate) fn submit_result_from_rc(rc: i32) -> Option<SubmitResult> {
    match rc {
        0 => Some(SubmitResult::Ok),
        1 => Some(SubmitResult::Backpressured),
        2 => Some(SubmitResult::NotConnected),
        3 => Some(SubmitResult::NotFound),
        4 => Some(SubmitResult::Terminated),
        5 => Some(SubmitResult::InvalidHandle),
        6 => Some(SubmitResult::InvalidArgument),
        7 => Some(SubmitResult::NotSupported),
        8 => Some(SubmitResult::InvalidState),
        9 => Some(SubmitResult::ThreadViolation),
        10 => Some(SubmitResult::OutOfMemory),
        11 => Some(SubmitResult::SeqExhausted),
        12 => Some(SubmitResult::InternalError),
        13 => Some(SubmitResult::NotAdmitted),
        _ => None,
    }
}

pub(crate) fn submit_error_from_rc(rc: i32, native_errno: i32) -> SubmitError {
    let code = submit_result_from_rc(rc).unwrap_or_else(|| {
        if rc == -1 {
            submit_result_from_errno(native_errno)
        } else {
            SubmitResult::InternalError
        }
    });
    SubmitError::new(code, native_errno)
}

pub(crate) fn send_terminal_error(native_errno: i32) -> SubmitError {
    let code = if native_errno == libc::ENOENT {
        SubmitResult::NotFound
    } else if native_errno == libc::ESHUTDOWN || native_errno == eterm() {
        SubmitResult::Terminated
    } else {
        SubmitResult::InternalError
    };
    SubmitError::new(code, native_errno)
}

fn recv_result_from_errno(err: i32) -> RecvResult {
    match err {
        0 => RecvResult::Ok,
        libc::EAGAIN => RecvResult::NoData,
        libc::EBUSY => RecvResult::Busy,
        x if x == eterm() => RecvResult::Terminated,
        libc::EFAULT => RecvResult::InvalidHandle,
        libc::ENOTSUP => RecvResult::NotSupported,
        x if x == eopnotsupp() => RecvResult::NotSupported,
        _ => RecvResult::InternalError,
    }
}

fn close_result_from_errno(err: i32) -> CloseResult {
    match err {
        0 => CloseResult::Ok,
        libc::EBUSY => CloseResult::Busy,
        x if x == eshutdown() => CloseResult::Shutdown,
        libc::EFAULT => CloseResult::InvalidHandle,
        _ => CloseResult::InternalError,
    }
}

fn bind_result_from_errno(err: i32) -> BindResult {
    match err {
        0 => BindResult::Ok,
        libc::EINVAL => BindResult::InvalidArgument,
        libc::EADDRINUSE => BindResult::AddrInUse,
        libc::ENOTSUP => BindResult::NotSupported,
        x if x == eopnotsupp() => BindResult::NotSupported,
        libc::EFAULT => BindResult::InvalidHandle,
        _ => BindResult::InternalError,
    }
}

fn connect_result_from_errno(err: i32) -> ConnectResult {
    match err {
        0 => ConnectResult::Ok,
        libc::EINVAL => ConnectResult::InvalidArgument,
        libc::ENOTSUP => ConnectResult::NotSupported,
        x if x == eopnotsupp() => ConnectResult::NotSupported,
        libc::EFAULT => ConnectResult::InvalidHandle,
        libc::ENOENT => ConnectResult::NotFound,
        libc::EBUSY => ConnectResult::Busy,
        _ => ConnectResult::InternalError,
    }
}

fn config_result_from_errno(err: i32) -> ConfigResult {
    match err {
        0 => ConfigResult::Ok,
        libc::EFAULT => ConfigResult::InvalidHandle,
        libc::EINVAL => ConfigResult::InvalidArgument,
        libc::ENOTSUP => ConfigResult::NotSupported,
        x if x == eopnotsupp() => ConfigResult::NotSupported,
        libc::ENOENT => ConfigResult::NotFound,
        libc::EBUSY => ConfigResult::InvalidState,
        _ => ConfigResult::InternalError,
    }
}

pub(crate) fn submit_validation_error() -> SubmitError {
    SubmitError::new(SubmitResult::InvalidArgument, libc::EINVAL)
}

pub(crate) fn request_error_from_result(code: RequestResult) -> RequestError {
    let native_errno = match code {
        RequestResult::Ok => 0,
        RequestResult::TimedOut => libc::ETIMEDOUT,
        RequestResult::NotFound => libc::ENOENT,
        RequestResult::Terminated => eterm(),
        RequestResult::ProtocolError => libc::EPROTO,
        RequestResult::InternalError => libc::EIO,
        RequestResult::Rejected => libc::ECONNREFUSED,
        RequestResult::Conflict => libc::EINVAL,
        RequestResult::Busy => libc::EBUSY,
        RequestResult::NotConnected => libc::ENOTCONN,
        RequestResult::InvalidArgument => libc::EINVAL,
        RequestResult::InvalidState => libc::EINVAL,
        RequestResult::NotSupported => libc::ENOTSUP,
        RequestResult::Backpressured => libc::EAGAIN,
    };
    RequestError::new(code, native_errno)
}

pub(crate) fn config_validation_error() -> ConfigError {
    ConfigError::new(ConfigResult::InvalidArgument, libc::EINVAL)
}

pub(crate) fn check_submit_rc(rc: i32) -> Result<(), SubmitError> {
    if rc == SubmitResult::Ok as i32 {
        Ok(())
    } else {
        Err(submit_error_from_rc(rc, last_errno()))
    }
}

pub(crate) fn check_recv_rc(rc: i32) -> Result<(), RecvError> {
    if rc == 0 {
        Ok(())
    } else {
        Err(RecvError::new(
            recv_result_from_errno(last_errno()),
            last_errno(),
        ))
    }
}

pub(crate) fn check_close_rc(rc: i32) -> Result<(), CloseError> {
    if rc == 0 {
        Ok(())
    } else {
        Err(CloseError::new(
            close_result_from_errno(last_errno()),
            last_errno(),
        ))
    }
}

pub(crate) fn check_bind_rc(rc: i32) -> Result<(), BindError> {
    if rc == 0 {
        Ok(())
    } else {
        Err(BindError::new(
            bind_result_from_errno(last_errno()),
            last_errno(),
        ))
    }
}

pub(crate) fn check_connect_rc(rc: i32) -> Result<(), ConnectError> {
    if rc == 0 {
        Ok(())
    } else {
        Err(ConnectError::new(
            connect_result_from_errno(last_errno()),
            last_errno(),
        ))
    }
}

pub(crate) fn check_config_rc(rc: i32) -> Result<(), ConfigError> {
    if rc == 0 {
        Ok(())
    } else {
        Err(ConfigError::new(
            config_result_from_errno(last_errno()),
            last_errno(),
        ))
    }
}

const fn eterm() -> i32 {
    156_384_765
}

const fn emthread() -> i32 {
    156_384_766
}

const fn eopnotsupp() -> i32 {
    #[cfg(any(target_os = "linux", target_os = "android"))]
    {
        libc::EOPNOTSUPP
    }
    #[cfg(not(any(target_os = "linux", target_os = "android")))]
    {
        libc::ENOTSUP
    }
}

const fn eshutdown() -> i32 {
    #[cfg(any(
        target_os = "linux",
        target_os = "android",
        target_os = "freebsd",
        target_os = "dragonfly",
        target_os = "netbsd",
        target_os = "openbsd",
        target_os = "macos",
        target_os = "ios"
    ))]
    {
        libc::ESHUTDOWN
    }
    #[cfg(not(any(
        target_os = "linux",
        target_os = "android",
        target_os = "freebsd",
        target_os = "dragonfly",
        target_os = "netbsd",
        target_os = "openbsd",
        target_os = "macos",
        target_os = "ios"
    )))]
    {
        libc::EPIPE
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_native_submit_result_has_an_exact_typed_mapping() {
        let expected = [
            SubmitResult::Ok,
            SubmitResult::Backpressured,
            SubmitResult::NotConnected,
            SubmitResult::NotFound,
            SubmitResult::Terminated,
            SubmitResult::InvalidHandle,
            SubmitResult::InvalidArgument,
            SubmitResult::NotSupported,
            SubmitResult::InvalidState,
            SubmitResult::ThreadViolation,
            SubmitResult::OutOfMemory,
            SubmitResult::SeqExhausted,
            SubmitResult::InternalError,
            SubmitResult::NotAdmitted,
        ];

        for result in expected {
            assert_eq!(submit_result_from_rc(result as i32), Some(result));
            if result != SubmitResult::Ok {
                let error = submit_error_from_rc(result as i32, libc::EIO);
                assert_eq!(error.code(), result);
                assert_eq!(error.native_errno(), libc::EIO);
            }
        }
    }

    #[test]
    fn legacy_enobufs_is_backpressure_not_out_of_memory() {
        assert_eq!(
            submit_error_from_rc(-1, libc::ENOBUFS).code(),
            SubmitResult::Backpressured
        );
    }

    #[test]
    fn terminal_send_errno_has_a_narrow_typed_mapping() {
        assert_eq!(
            send_terminal_error(libc::ENOENT).code(),
            SubmitResult::NotFound
        );
        assert_eq!(
            send_terminal_error(libc::ESHUTDOWN).code(),
            SubmitResult::Terminated
        );
        assert_eq!(
            send_terminal_error(eterm()).code(),
            SubmitResult::Terminated
        );
        assert_eq!(
            send_terminal_error(libc::EAGAIN).code(),
            SubmitResult::InternalError
        );
    }
}
