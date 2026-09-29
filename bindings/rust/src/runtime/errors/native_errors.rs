use crate::error::{
    BindError, BindResult, CloseError, CloseResult, ConfigError, ConfigResult, ConnectError,
    ConnectResult, RecvError, RecvResult, RequestError, RequestResult, SubmitError, SubmitResult,
};
use crate::ffi;

pub(crate) fn last_errno() -> i32 {
    unsafe { ffi::zlink_errno() }
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
    let code = submit_result_from_rc(rc).unwrap_or(SubmitResult::InternalError);
    SubmitError::new(code, native_errno)
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
    // Completion records carry the Core result, but no native errno.
    RequestError::new(code, 0)
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
        let code = match rc {
            201 => RecvResult::NoData,
            202 => RecvResult::Busy,
            203 => RecvResult::Terminated,
            204 => RecvResult::InvalidHandle,
            205 => RecvResult::NotSupported,
            207 => RecvResult::BufferTooSmall,
            208 => RecvResult::InvalidState,
            _ => RecvResult::InternalError,
        };
        Err(RecvError::new(code, last_errno()))
    }
}

pub(crate) fn check_close_rc(rc: i32) -> Result<(), CloseError> {
    if rc == 0 {
        Ok(())
    } else {
        let errno = last_errno();
        let code = match rc {
            401 => CloseResult::Busy,
            402 => CloseResult::Shutdown,
            403 => CloseResult::InvalidHandle,
            _ => CloseResult::InternalError,
        };
        Err(CloseError::new(code, errno))
    }
}

pub(crate) fn check_bind_rc(rc: i32) -> Result<(), BindError> {
    if rc == 0 {
        Ok(())
    } else {
        let errno = last_errno();
        let code = match rc {
            501 => BindResult::InvalidArgument,
            502 => BindResult::AddrInUse,
            503 => BindResult::NotSupported,
            504 => BindResult::InvalidHandle,
            _ => BindResult::InternalError,
        };
        Err(BindError::new(code, errno))
    }
}

pub(crate) fn check_connect_rc(rc: i32) -> Result<(), ConnectError> {
    if rc == 0 {
        Ok(())
    } else {
        let errno = last_errno();
        let code = match rc {
            601 => ConnectResult::InvalidArgument,
            602 => ConnectResult::NotSupported,
            603 => ConnectResult::InvalidHandle,
            605 => ConnectResult::NotFound,
            606 => ConnectResult::Conflict,
            607 => ConnectResult::Busy,
            608 => ConnectResult::AuthFailed,
            _ => ConnectResult::InternalError,
        };
        Err(ConnectError::new(code, errno))
    }
}

/// Projects the Core configuration result without interpreting errno.
pub(crate) fn check_config_rc(rc: i32) -> Result<(), ConfigError> {
    if rc == 0 {
        Ok(())
    } else {
        let errno = last_errno();
        Err(ConfigError::new(config_result_from_rc(rc), errno))
    }
}

fn config_result_from_rc(rc: i32) -> ConfigResult {
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
        _ => ConfigResult::InternalError,
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
    fn missing_core_submit_result_is_internal_error() {
        assert_eq!(
            submit_error_from_rc(-1, libc::ENOBUFS).code(),
            SubmitResult::InternalError
        );
    }

    #[test]
    fn catalog_values_match_core_codes() {
        // Public Result Enum catalog values (bindings/doc/spec README).
        assert_eq!(RecvResult::BufferTooSmall as i32, 207);
        assert_eq!(ConnectResult::AuthFailed as i32, 608);
        assert_eq!(ConfigResult::Conflict as i32, 707);
        assert_eq!(ConfigResult::BufferTooSmall as i32, 708);
        assert_eq!(ConfigResult::Busy as i32, 709);
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
        assert_eq!(config_result_from_rc(709), ConfigResult::Busy);
        assert_eq!(config_result_from_rc(707), ConfigResult::Conflict);
        assert_eq!(config_result_from_rc(-1), ConfigResult::InternalError);
    }

    #[test]
    fn request_completion_does_not_invent_native_errno() {
        assert_eq!(
            request_error_from_result(RequestResult::Rejected).native_errno(),
            0
        );
        assert_eq!(
            request_error_from_result(RequestResult::Conflict).native_errno(),
            0
        );
        assert_eq!(
            request_error_from_result(RequestResult::InvalidState).native_errno(),
            0
        );
    }

    #[test]
    fn core_results_override_errno_categories() {
        // Set an unrelated errno before projecting Core result values.
        unsafe { ffi::zlink_bind(std::ptr::null_mut(), std::ptr::null()) };
        assert_ne!(last_errno(), libc::EBUSY);
        assert_eq!(check_recv_rc(202).unwrap_err().code(), RecvResult::Busy);
        assert_eq!(
            check_recv_rc(207).unwrap_err().code(),
            RecvResult::BufferTooSmall
        );
        assert_eq!(check_close_rc(401).unwrap_err().code(), CloseResult::Busy);
        assert_eq!(
            check_bind_rc(502).unwrap_err().code(),
            BindResult::AddrInUse
        );
        assert_eq!(
            check_connect_rc(608).unwrap_err().code(),
            ConnectResult::AuthFailed
        );
        assert_eq!(config_result_from_rc(709), ConfigResult::Busy);
    }
}
