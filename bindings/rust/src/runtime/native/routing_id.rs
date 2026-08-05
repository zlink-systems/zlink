use crate::ffi;
use crate::routing_id::RoutingId;

impl RoutingId {
    pub(crate) fn as_raw(&self) -> &ffi::zlink_routing_id_t {
        unsafe { &*(self as *const Self).cast::<ffi::zlink_routing_id_t>() }
    }

    pub(crate) fn from_raw(raw: ffi::zlink_routing_id_t) -> Self {
        Self {
            size: raw.size,
            data: raw.data,
        }
    }

    pub(crate) fn from_raw_optional(raw: ffi::zlink_routing_id_t) -> Option<Self> {
        if raw.size == 0 {
            None
        } else {
            Some(Self::from_raw(raw))
        }
    }
}
