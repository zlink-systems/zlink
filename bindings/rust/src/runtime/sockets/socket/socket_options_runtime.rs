use super::*;

pub(super) fn set_int_opt(
    handle: *mut c_void,
    opt: ffi::zlink_option_t,
    value: i32,
) -> Result<(), ConfigError> {
    check_config_rc(unsafe {
        ffi::zlink_set_option(
            handle,
            opt,
            &value as *const i32 as *const c_void,
            std::mem::size_of::<i32>(),
        )
    })
}

pub(super) fn set_u64_opt(
    handle: *mut c_void,
    opt: ffi::zlink_option_t,
    value: u64,
) -> Result<(), ConfigError> {
    check_config_rc(unsafe {
        ffi::zlink_set_option(
            handle,
            opt,
            &value as *const u64 as *const c_void,
            std::mem::size_of::<u64>(),
        )
    })
}

pub(super) fn set_bool_opt(
    handle: *mut c_void,
    opt: ffi::zlink_option_t,
    enabled: bool,
) -> Result<(), ConfigError> {
    set_int_opt(handle, opt, if enabled { 1 } else { 0 })
}

pub(super) fn set_duration_opt(
    handle: *mut c_void,
    opt: ffi::zlink_option_t,
    duration: Duration,
) -> Result<(), ConfigError> {
    set_int_opt(handle, opt, duration_to_millis(duration)?)
}

pub(super) fn get_duration_opt(
    handle: *mut c_void,
    opt: ffi::zlink_option_t,
) -> Result<Duration, ConfigError> {
    let millis = get_int_opt(handle, opt)?;
    Ok(Duration::from_millis(millis as u64))
}

pub(super) fn set_string_opt(
    handle: *mut c_void,
    opt: ffi::zlink_option_t,
    value: &str,
) -> Result<(), ConfigError> {
    let c_value = CString::new(value).map_err(|_| config_validation_error())?;
    check_config_rc(unsafe {
        ffi::zlink_set_option(
            handle,
            opt,
            c_value.as_ptr() as *const c_void,
            value.len() + 1,
        )
    })
}

pub(super) fn get_int_opt(
    handle: *mut c_void,
    opt: ffi::zlink_option_t,
) -> Result<i32, ConfigError> {
    let mut value: i32 = 0;
    let mut len = std::mem::size_of::<i32>();
    check_config_rc(unsafe {
        ffi::zlink_get_option(handle, opt, &mut value as *mut i32 as *mut c_void, &mut len)
    })?;
    Ok(value)
}

pub(super) fn get_u64_opt(
    handle: *mut c_void,
    opt: ffi::zlink_option_t,
) -> Result<u64, ConfigError> {
    let mut value: u64 = 0;
    let mut len = std::mem::size_of::<u64>();
    check_config_rc(unsafe {
        ffi::zlink_get_option(handle, opt, &mut value as *mut u64 as *mut c_void, &mut len)
    })?;
    if len != std::mem::size_of::<u64>() {
        return Err(config_validation_error());
    }
    Ok(value)
}

pub(super) fn get_bool_opt(
    handle: *mut c_void,
    opt: ffi::zlink_option_t,
) -> Result<bool, ConfigError> {
    Ok(get_int_opt(handle, opt)? != 0)
}

pub(super) fn get_i64_opt(
    handle: *mut c_void,
    opt: ffi::zlink_option_t,
) -> Result<i64, ConfigError> {
    let mut value: i64 = 0;
    let mut len = std::mem::size_of::<i64>();
    check_config_rc(unsafe {
        ffi::zlink_get_option(handle, opt, &mut value as *mut i64 as *mut c_void, &mut len)
    })?;
    Ok(value)
}
