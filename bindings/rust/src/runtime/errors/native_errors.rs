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
        libc::ESHUTDOWN => SubmitResult::Terminated,
        x if x == eterm() => SubmitResult::Terminated,
        libc::EFAULT => SubmitResult::InvalidHandle,
        libc::EINVAL | libc::EMSGSIZE => SubmitResult::InvalidArgument,
        libc::ENOTSUP => SubmitResult::NotSupported,
        x if x == eopnotsupp() => SubmitResult::NotSupported,
        libc::EBUSY | libc::ESTALE | libc::EALREADY => SubmitResult::InvalidState,
        x if x == efsm() => SubmitResult::InvalidState,
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

// Each `*_result_from_errno` below follows the Core projection table of the
// same result family (`core/src/api/*/*_result_internal.hpp`) row for row.

pub(crate) fn recv_result_from_errno(err: i32) -> RecvResult {
    match err {
        0 => RecvResult::Ok,
        x if is_not_supported(x) => RecvResult::NotSupported,
        libc::EAGAIN | libc::ETIMEDOUT => RecvResult::NoData,
        libc::EBUSY => RecvResult::Busy,
        x if x == eterm() => RecvResult::Terminated,
        libc::EFAULT => RecvResult::InvalidHandle,
        libc::ENOBUFS => RecvResult::BufferTooSmall,
        libc::EINVAL | libc::ESTALE => RecvResult::InvalidState,
        x if x == eshutdown() => RecvResult::InvalidState,
        _ => RecvResult::InternalError,
    }
}

/// Projects a failed receive-family call through the recv table. Pass the
/// errno read on the returning thread right after the native call.
pub(crate) fn recv_error_from_errno(err: i32) -> RecvError {
    RecvError::new(recv_result_from_errno(err), err)
}

fn close_result_from_errno(err: i32) -> CloseResult {
    match err {
        0 => CloseResult::Ok,
        libc::EBUSY | libc::EDEADLK => CloseResult::Busy,
        x if x == eshutdown() => CloseResult::Shutdown,
        libc::EFAULT | libc::ESTALE => CloseResult::InvalidHandle,
        _ => CloseResult::InternalError,
    }
}

fn bind_result_from_errno(err: i32) -> BindResult {
    match err {
        0 => BindResult::Ok,
        x if is_not_supported(x) || x == libc::EPROTONOSUPPORT => BindResult::NotSupported,
        libc::EINVAL => BindResult::InvalidArgument,
        libc::EADDRINUSE => BindResult::AddrInUse,
        libc::EFAULT => BindResult::InvalidHandle,
        _ => BindResult::InternalError,
    }
}

fn connect_result_from_errno(err: i32) -> ConnectResult {
    match err {
        0 => ConnectResult::Ok,
        x if is_not_supported(x) || x == libc::EPROTONOSUPPORT => ConnectResult::NotSupported,
        libc::EINVAL => ConnectResult::InvalidArgument,
        libc::EFAULT => ConnectResult::InvalidHandle,
        libc::ENOENT => ConnectResult::NotFound,
        libc::EADDRINUSE | libc::EEXIST | libc::ESTALE => ConnectResult::Conflict,
        libc::EBUSY => ConnectResult::Busy,
        x if x == eshutdown() => ConnectResult::Busy,
        libc::EACCES => ConnectResult::AuthFailed,
        _ => ConnectResult::InternalError,
    }
}

fn config_result_from_errno(err: i32) -> ConfigResult {
    match err {
        0 => ConfigResult::Ok,
        x if is_not_supported(x) => ConfigResult::NotSupported,
        libc::EFAULT => ConfigResult::InvalidHandle,
        libc::EINVAL | libc::EMSGSIZE => ConfigResult::InvalidArgument,
        libc::EBUSY
        | libc::ESTALE
        | libc::EALREADY
        | libc::ENOTCONN
        | libc::ETIMEDOUT
        | libc::EPROTO => ConfigResult::InvalidState,
        x if x == eshutdown() => ConfigResult::InvalidState,
        libc::ENOENT => ConfigResult::NotFound,
        libc::EEXIST => ConfigResult::Conflict,
        libc::ENOBUFS => ConfigResult::BufferTooSmall,
        _ => ConfigResult::InternalError,
    }
}

/// Core already projected the result; each native value is the public value
/// with the same number.
pub(crate) fn config_result_from_native(result: ffi::zlink_config_result_t) -> ConfigResult {
    use ffi::zlink_config_result_t as Native;
    match result {
        Native::ZLINK_CONFIG_OK => ConfigResult::Ok,
        Native::ZLINK_CONFIG_INVALID_HANDLE => ConfigResult::InvalidHandle,
        Native::ZLINK_CONFIG_INVALID_ARGUMENT => ConfigResult::InvalidArgument,
        Native::ZLINK_CONFIG_NOT_SUPPORTED => ConfigResult::NotSupported,
        Native::ZLINK_CONFIG_INTERNAL_ERROR => ConfigResult::InternalError,
        Native::ZLINK_CONFIG_INVALID_STATE => ConfigResult::InvalidState,
        Native::ZLINK_CONFIG_NOT_FOUND => ConfigResult::NotFound,
        Native::ZLINK_CONFIG_CONFLICT => ConfigResult::Conflict,
        Native::ZLINK_CONFIG_BUFFER_TOO_SMALL => ConfigResult::BufferTooSmall,
        Native::ZLINK_CONFIG_BUSY => ConfigResult::Busy,
    }
}

pub(crate) fn request_error_from_result(code: RequestResult) -> RequestError {
    let native_errno = match code {
        RequestResult::Ok => 0,
        RequestResult::TimedOut => libc::ETIMEDOUT,
        RequestResult::NotFound => libc::ENOENT,
        RequestResult::Terminated => eterm(),
        RequestResult::ProtocolError => libc::EPROTO,
        RequestResult::InternalError => libc::EIO,
        RequestResult::Rejected => libc::EACCES,
        RequestResult::Conflict => libc::ESTALE,
        RequestResult::Busy => libc::EBUSY,
        RequestResult::NotConnected => libc::ENOTCONN,
        RequestResult::InvalidArgument => libc::EINVAL,
        RequestResult::InvalidState => efsm(),
        RequestResult::NotSupported => libc::ENOTSUP,
        RequestResult::Backpressured => libc::EAGAIN,
    };
    RequestError::new(code, native_errno)
}

pub(crate) fn config_validation_error() -> ConfigError {
    ConfigError::new(ConfigResult::InvalidArgument, libc::EINVAL)
}

// Each `check_*_rc` reads `zlink_errno()` once, on the thread the native call
// returned on, before any other runtime code runs.

pub(crate) fn check_recv_rc(rc: i32) -> Result<(), RecvError> {
    if rc == 0 {
        Ok(())
    } else {
        Err(recv_error_from_errno(last_errno()))
    }
}

pub(crate) fn check_close_rc(rc: i32) -> Result<(), CloseError> {
    if rc == 0 {
        Ok(())
    } else {
        let errno = last_errno();
        Err(CloseError::new(close_result_from_errno(errno), errno))
    }
}

pub(crate) fn check_bind_rc(rc: i32) -> Result<(), BindError> {
    if rc == 0 {
        Ok(())
    } else {
        let errno = last_errno();
        Err(BindError::new(bind_result_from_errno(errno), errno))
    }
}

pub(crate) fn check_connect_rc(rc: i32) -> Result<(), ConnectError> {
    if rc == 0 {
        Ok(())
    } else {
        let errno = last_errno();
        Err(ConnectError::new(connect_result_from_errno(errno), errno))
    }
}

/// A nonzero `rc` that is a Core configuration result is used as it is (Core
/// reaches `ZLINK_CONFIG_BUSY` only as a result); any other failure value is
/// projected from errno.
pub(crate) fn check_config_rc(rc: i32) -> Result<(), ConfigError> {
    if rc == 0 {
        Ok(())
    } else {
        let errno = last_errno();
        Err(ConfigError::new(config_result_from_rc(rc, errno), errno))
    }
}

fn config_result_from_rc(rc: i32, errno: i32) -> ConfigResult {
    match rc {
        701 => ConfigResult::InvalidHandle,
        702 => ConfigResult::InvalidArgument,
        703 => ConfigResult::NotSupported,
        704 => ConfigResult::InternalError,
        705 => ConfigResult::InvalidState,
        706 => ConfigResult::NotFound,
        707 => ConfigResult::Conflict,
        708 => ConfigResult::BufferTooSmall,
        709 => ConfigResult::Busy,
        _ => config_result_from_errno(errno),
    }
}

const fn efsm() -> i32 {
    156_384_763
}

pub(crate) const fn eterm() -> i32 {
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

fn is_not_supported(err: i32) -> bool {
    err == libc::ENOTSUP || err == eopnotsupp()
}

pub(crate) const fn eshutdown() -> i32 {
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

    #[test]
    fn catalog_values_missing_before_have_core_codes_and_errno_rows() {
        // Public Result Enum catalog values (bindings/doc/spec README).
        assert_eq!(RecvResult::BufferTooSmall as i32, 207);
        assert_eq!(ConnectResult::AuthFailed as i32, 608);
        assert_eq!(ConfigResult::Conflict as i32, 707);
        assert_eq!(ConfigResult::BufferTooSmall as i32, 708);
        assert_eq!(ConfigResult::Busy as i32, 709);
        // Core recv/connect/config tables.
        assert_eq!(
            recv_result_from_errno(libc::ENOBUFS),
            RecvResult::BufferTooSmall
        );
        assert_eq!(
            connect_result_from_errno(libc::EACCES),
            ConnectResult::AuthFailed
        );
        assert_eq!(
            config_result_from_errno(libc::EEXIST),
            ConfigResult::Conflict
        );
        assert_eq!(
            config_result_from_errno(libc::ENOBUFS),
            ConfigResult::BufferTooSmall
        );
        // Core's config table has no errno row for BUSY: EBUSY stays
        // INVALID_STATE, and CONFIG_BUSY is reached only as a Core result.
        assert_eq!(
            config_result_from_errno(libc::EBUSY),
            ConfigResult::InvalidState
        );
        assert_eq!(
            config_result_from_native(ffi::zlink_config_result_t::ZLINK_CONFIG_BUSY),
            ConfigResult::Busy
        );
        assert_eq!(
            config_result_from_native(ffi::zlink_config_result_t::ZLINK_CONFIG_CONFLICT),
            ConfigResult::Conflict
        );
        assert_eq!(
            config_result_from_native(ffi::zlink_config_result_t::ZLINK_CONFIG_BUFFER_TOO_SMALL),
            ConfigResult::BufferTooSmall
        );
    }

    #[test]
    fn a_core_config_result_is_used_as_it_is() {
        // 05-polling §5: a call during a wait returns ZLINK_CONFIG_BUSY with
        // EBUSY, which the errno table alone would read as INVALID_STATE.
        assert_eq!(config_result_from_rc(709, libc::EBUSY), ConfigResult::Busy);
        assert_eq!(
            config_result_from_rc(707, libc::EEXIST),
            ConfigResult::Conflict
        );
        assert_eq!(
            config_result_from_rc(-1, libc::EBUSY),
            ConfigResult::InvalidState
        );
    }

    #[test]
    fn submit_and_request_errno_rows_follow_core() {
        // Core submit_result_internal.hpp: ECANCELED has no row (INTERNAL_ERROR)
        // and EFSM is INVALID_STATE.
        assert_eq!(
            submit_result_from_errno(libc::ECANCELED),
            SubmitResult::InternalError
        );
        assert_eq!(submit_result_from_errno(efsm()), SubmitResult::InvalidState);
        // Core request_result_internal.hpp to_errno.
        assert_eq!(
            request_error_from_result(RequestResult::Rejected).native_errno(),
            libc::EACCES
        );
        assert_eq!(
            request_error_from_result(RequestResult::Conflict).native_errno(),
            libc::ESTALE
        );
        assert_eq!(
            request_error_from_result(RequestResult::InvalidState).native_errno(),
            efsm()
        );
    }

    #[test]
    fn errno_tables_follow_core_rows() {
        assert_eq!(recv_result_from_errno(libc::ETIMEDOUT), RecvResult::NoData);
        assert_eq!(
            recv_result_from_errno(libc::ESTALE),
            RecvResult::InvalidState
        );
        assert_eq!(
            recv_result_from_errno(libc::EINVAL),
            RecvResult::InvalidState
        );
        assert_eq!(recv_result_from_errno(libc::EIO), RecvResult::InternalError);
        assert_eq!(
            connect_result_from_errno(libc::EADDRINUSE),
            ConnectResult::Conflict
        );
        assert_eq!(
            connect_result_from_errno(libc::EEXIST),
            ConnectResult::Conflict
        );
        assert_eq!(
            connect_result_from_errno(libc::ESTALE),
            ConnectResult::Conflict
        );
        assert_eq!(
            connect_result_from_errno(libc::EPROTONOSUPPORT),
            ConnectResult::NotSupported
        );
        assert_eq!(
            bind_result_from_errno(libc::EPROTONOSUPPORT),
            BindResult::NotSupported
        );
        assert_eq!(close_result_from_errno(libc::EDEADLK), CloseResult::Busy);
        assert_eq!(
            close_result_from_errno(libc::ESTALE),
            CloseResult::InvalidHandle
        );
        assert_eq!(
            config_result_from_errno(libc::EMSGSIZE),
            ConfigResult::InvalidArgument
        );
        assert_eq!(
            config_result_from_errno(libc::EPROTO),
            ConfigResult::InvalidState
        );
        assert_eq!(
            config_result_from_errno(libc::ETIMEDOUT),
            ConfigResult::InvalidState
        );
    }
}
