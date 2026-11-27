use crate::native::ffi;

pub(crate) struct MessageStorage {
    pub(crate) raw: ffi::zlink_msg_t,
}

impl MessageStorage {
    pub(crate) fn new(raw: ffi::zlink_msg_t) -> Self {
        Self { raw }
    }
}

#[derive(Default)]
pub(crate) struct NativeReceiveScratch {
    pub(crate) parts: Vec<ffi::zlink_msg_t>,
}
