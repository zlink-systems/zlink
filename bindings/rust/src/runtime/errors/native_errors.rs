use crate::error::{
    BindError, BindResult, CloseError, CloseResult, ConfigError, ConfigResult, ConnectError,
    ConnectResult, HandlerError, HandlerResult, RecvError, RecvResult, RequestError, RequestResult,
    SubmitError, SubmitResult,
};
use crate::ffi;

pub(crate) fn last_errno() -> i32 {
    unsafe { ffi::zlink_errno() }
}

fn submit_result_from_errno(err: i32) -> SubmitResult {
    match err {
        0 => SubmitResult::Ok,
        libc::EAGAIN => SubmitResult::Backpressured,
        libc::ENOTCONN | libc::EHOSTUNREACH => SubmitResult::NotConnected,
        libc::ENOENT => SubmitResult::NotFound,
        x if x == eterm() => SubmitResult::Terminated,
        libc::EFAULT => SubmitResult::InvalidHandle,
        libc::EINVAL => SubmitResult::InvalidArgument,
        libc::ENOTSUP => SubmitResult::NotSupported,
        x if x == eopnotsupp() => SubmitResult::NotSupported,
        libc::EBUSY => SubmitResult::InvalidState,
        x if x == emthread() => SubmitResult::ThreadViolation,
        libc::ENOMEM | libc::ENOBUFS => SubmitResult::OutOfMemory,
        _ => SubmitResult::InternalError,
    }
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

fn handler_result_from_errno(err: i32) -> HandlerResult {
    match err {
        0 => HandlerResult::Ok,
        libc::EINVAL => HandlerResult::InvalidArgument,
        libc::EBUSY => HandlerResult::Busy,
        libc::ENOTSUP => HandlerResult::NotSupported,
        x if x == eopnotsupp() => HandlerResult::NotSupported,
        libc::EDEADLK => HandlerResult::Deadlock,
        libc::EFAULT => HandlerResult::InvalidHandle,
        _ => HandlerResult::InternalError,
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

pub(crate) fn submit_not_supported_error() -> SubmitError {
    SubmitError::new(SubmitResult::NotSupported, libc::ENOTSUP)
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
    };
    RequestError::new(code, native_errno)
}

pub(crate) fn config_validation_error() -> ConfigError {
    ConfigError::new(ConfigResult::InvalidArgument, libc::EINVAL)
}

pub(crate) fn check_submit_rc(rc: i32) -> Result<(), SubmitError> {
    if rc == 0 {
        Ok(())
    } else {
        Err(SubmitError::new(
            submit_result_from_errno(last_errno()),
            last_errno(),
        ))
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

pub(crate) fn check_handler_rc(rc: i32) -> Result<(), HandlerError> {
    if rc == 0 {
        Ok(())
    } else {
        Err(HandlerError::new(
            handler_result_from_errno(last_errno()),
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
