use crate::contracts::errors::errors::ConfigError;
use crate::contracts::eventing::poller::Pollable;
use crate::contracts::messaging::message::Message;
use crate::runtime::errors::native_errors::check_config_rc;
use crate::runtime::eventing::poller::pollable_handle;
use crate::runtime::native::ffi;

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
