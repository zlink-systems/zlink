use crate::error::ConfigError;
use crate::ffi;
use crate::message::Message;
use crate::native_errors::check_config_rc;
use crate::poller::pollable_handle;
use crate::poller_contracts::Pollable;

pub fn proxy(
    frontend: &dyn Pollable,
    backend: &dyn Pollable,
    capture: Option<&dyn Pollable>,
) -> Result<(), ConfigError> {
    let frontend = pollable_handle(frontend)?;
    let backend = pollable_handle(backend)?;
    let capture = capture
        .map(pollable_handle)
        .transpose()?
        .unwrap_or(std::ptr::null_mut());
    check_config_rc(unsafe { ffi::zlink_proxy(frontend, backend, capture) })
}

pub fn proxy_steerable(
    frontend: &dyn Pollable,
    backend: &dyn Pollable,
    capture: Option<&dyn Pollable>,
    control: &dyn Pollable,
) -> Result<(), ConfigError> {
    let frontend = pollable_handle(frontend)?;
    let backend = pollable_handle(backend)?;
    let capture = capture
        .map(pollable_handle)
        .transpose()?
        .unwrap_or(std::ptr::null_mut());
    let control = pollable_handle(control)?;
    check_config_rc(unsafe { ffi::zlink_proxy_steerable(frontend, backend, capture, control) })
}

pub fn sleep(seconds: i32) {
    unsafe {
        ffi::zlink_sleep(seconds);
    }
}

pub fn multipart_close(parts: &mut [Message]) {
    for part in parts.iter_mut() {
        part.close_now();
    }
}
