use std::mem::MaybeUninit;
use std::slice;

use crate::error::ConfigError;
use crate::ffi;
use crate::message::Message;
use crate::native_errors::check_config_rc;

fn raw_ref(message: &Message) -> &ffi::zlink_msg_t {
    &message.inner.raw
}

fn raw_mut(message: &mut Message) -> &mut ffi::zlink_msg_t {
    &mut message.inner.raw
}

fn from_raw_storage(raw: ffi::zlink_msg_t) -> Message {
    Message {
        inner: crate::internal::MessageStorage::new(raw),
    }
}

pub(crate) fn message_new() -> Result<Message, ConfigError> {
    unsafe {
        let mut msg = MaybeUninit::<ffi::zlink_msg_t>::uninit();
        check_config_rc(ffi::zlink_msg_init(msg.as_mut_ptr()))?;
        Ok(from_raw_storage(msg.assume_init()))
    }
}

pub(crate) fn message_with_size(size: usize) -> Result<Message, ConfigError> {
    unsafe {
        let mut msg = MaybeUninit::<ffi::zlink_msg_t>::uninit();
        check_config_rc(ffi::zlink_msg_init_size(msg.as_mut_ptr(), size))?;
        Ok(from_raw_storage(msg.assume_init()))
    }
}

pub(crate) fn message_from_slice(data: &[u8]) -> Result<Message, ConfigError> {
    let mut msg = message_with_size(data.len())?;
    unsafe {
        let dst = ffi::zlink_msg_data(raw_mut(&mut msg)) as *mut u8;
        std::ptr::copy_nonoverlapping(data.as_ptr(), dst, data.len());
    }
    Ok(msg)
}

pub(crate) fn message_as_bytes(message: &Message) -> &[u8] {
    unsafe {
        let raw = raw_ref(message);
        let ptr = ffi::zlink_msg_data(raw as *const ffi::zlink_msg_t as *mut ffi::zlink_msg_t)
            as *const u8;
        let len = ffi::zlink_msg_size(raw);
        if ptr.is_null() || len == 0 {
            &[]
        } else {
            slice::from_raw_parts(ptr, len)
        }
    }
}

pub(crate) fn message_data_mut(message: &mut Message) -> &mut [u8] {
    unsafe {
        let raw = raw_mut(message);
        let ptr = ffi::zlink_msg_data(raw) as *mut u8;
        let len = ffi::zlink_msg_size(raw);
        if ptr.is_null() || len == 0 {
            &mut []
        } else {
            slice::from_raw_parts_mut(ptr, len)
        }
    }
}

pub(crate) fn message_size(message: &Message) -> usize {
    unsafe { ffi::zlink_msg_size(raw_ref(message)) }
}

pub(crate) fn message_ref_count(message: &Message) -> i32 {
    let mut err = ffi::zlink_config_result_t::ZLINK_CONFIG_OK;
    unsafe { ffi::zlink_msg_refcnt(raw_ref(message), &mut err) }
}

pub(crate) fn message_try_clone(message: &Message) -> Result<Message, ConfigError> {
    unsafe {
        let mut msg = MaybeUninit::<ffi::zlink_msg_t>::uninit();
        check_config_rc(ffi::zlink_msg_init(msg.as_mut_ptr()))?;
        let mut inner = msg.assume_init();
        match check_config_rc(ffi::zlink_msg_copy(
            &mut inner,
            raw_ref(message) as *const ffi::zlink_msg_t as *mut ffi::zlink_msg_t,
        )) {
            Ok(()) => Ok(from_raw_storage(inner)),
            Err(error) => {
                let _ = ffi::zlink_msg_close(&mut inner);
                Err(error)
            }
        }
    }
}

pub(crate) fn message_drop(message: &mut Message) {
    unsafe {
        let _ = ffi::zlink_msg_close(raw_mut(message));
    }
}

impl Message {
    pub(crate) fn raw_mut(&mut self) -> &mut ffi::zlink_msg_t {
        raw_mut(self)
    }

    /// Construct from a raw `zlink_msg_t` whose ownership is being transferred
    /// to Rust. The caller must not close the original.
    pub(crate) unsafe fn from_raw(raw: ffi::zlink_msg_t) -> Self {
        from_raw_storage(raw)
    }

    pub(crate) fn close_now(&mut self) {
        unsafe {
            let raw = raw_mut(self);
            let _ = ffi::zlink_msg_close(raw);
            let _ = ffi::zlink_msg_init(raw);
        }
    }
}
