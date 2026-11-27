use std::mem::MaybeUninit;
use std::slice;

use crate::error::ConfigError;
use crate::ffi;

use crate::internal::{MessageParts, MessageStorage};
use crate::message::Message;
use crate::native_errors::check_config_rc;

impl MessageParts {
    pub(crate) fn close_parts(&mut self) {
        if !self.is_empty() {
            unsafe { ffi::zlink_multipart_close(self.as_mut_ptr(), self.len()) };
        }
        self.clear();
    }
}

impl Drop for MessageParts {
    fn drop(&mut self) {
        self.close_parts();
    }
}
fn raw_ref(storage: &MessageStorage) -> &ffi::zlink_msg_t {
    &storage.raw
}

fn raw_mut(storage: &mut MessageStorage) -> &mut ffi::zlink_msg_t {
    &mut storage.raw
}

fn from_raw_storage(raw: ffi::zlink_msg_t) -> Message {
    Message {
        inner: crate::internal::MessageStorage::new(raw),
    }
}

impl MessageStorage {
    pub(crate) fn new_message() -> Result<Message, ConfigError> {
        unsafe {
            let mut msg = MaybeUninit::<ffi::zlink_msg_t>::uninit();
            check_config_rc(ffi::zlink_msg_init(msg.as_mut_ptr()))?;
            Ok(from_raw_storage(msg.assume_init()))
        }
    }

    pub(crate) fn with_size_message(size: usize) -> Result<Message, ConfigError> {
        unsafe {
            let mut msg = MaybeUninit::<ffi::zlink_msg_t>::uninit();
            check_config_rc(ffi::zlink_msg_init_size(msg.as_mut_ptr(), size))?;
            Ok(from_raw_storage(msg.assume_init()))
        }
    }

    pub(crate) fn from_slice_message(data: &[u8]) -> Result<Message, ConfigError> {
        let mut msg = Self::with_size_message(data.len())?;
        unsafe {
            let dst = ffi::zlink_msg_data(raw_mut(&mut msg.inner)) as *mut u8;
            std::ptr::copy_nonoverlapping(data.as_ptr(), dst, data.len());
        }
        Ok(msg)
    }

    pub(crate) fn as_bytes(&self) -> &[u8] {
        unsafe {
            let raw = raw_ref(self);
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

    pub(crate) fn data_mut(&mut self) -> &mut [u8] {
        unsafe {
            let raw = raw_mut(self);
            let ptr = ffi::zlink_msg_data(raw) as *mut u8;
            let len = ffi::zlink_msg_size(raw);
            if ptr.is_null() || len == 0 {
                &mut []
            } else {
                slice::from_raw_parts_mut(ptr, len)
            }
        }
    }

    pub(crate) fn size(&self) -> usize {
        unsafe { ffi::zlink_msg_size(raw_ref(self)) }
    }

    pub(crate) fn ref_count(&self) -> i32 {
        let mut err = ffi::zlink_config_result_t::ZLINK_CONFIG_OK;
        unsafe { ffi::zlink_msg_refcnt(raw_ref(self), &mut err) }
    }

    pub(crate) fn try_clone_message(&self) -> Result<Message, ConfigError> {
        unsafe {
            let mut msg = MaybeUninit::<ffi::zlink_msg_t>::uninit();
            check_config_rc(ffi::zlink_msg_init(msg.as_mut_ptr()))?;
            let mut inner = msg.assume_init();
            match check_config_rc(ffi::zlink_msg_copy(
                &mut inner,
                raw_ref(self) as *const ffi::zlink_msg_t as *mut ffi::zlink_msg_t,
            )) {
                Ok(()) => Ok(from_raw_storage(inner)),
                Err(error) => {
                    let _ = ffi::zlink_msg_close(&mut inner);
                    Err(error)
                }
            }
        }
    }

    pub(crate) fn close(&mut self) {
        unsafe {
            let _ = ffi::zlink_msg_close(raw_mut(self));
        }
    }
}

impl Message {
    pub(crate) fn into_raw(self) -> ffi::zlink_msg_t {
        let message = std::mem::ManuallyDrop::new(self);
        unsafe { std::ptr::read(&message.inner.raw) }
    }

    pub(crate) fn raw_mut(&mut self) -> &mut ffi::zlink_msg_t {
        raw_mut(&mut self.inner)
    }

    /// Construct from a raw `zlink_msg_t` whose ownership is being transferred
    /// to Rust. The caller must not close the original.
    pub(crate) unsafe fn from_raw(raw: ffi::zlink_msg_t) -> Self {
        from_raw_storage(raw)
    }

    pub(crate) fn close_now(&mut self) {
        unsafe {
            let raw = raw_mut(&mut self.inner);
            let _ = ffi::zlink_msg_close(raw);
            let _ = ffi::zlink_msg_init(raw);
        }
    }
}
