pub use crate::contracts::core::routing_id::RoutingId;

/// Owned message frame.
///
/// `Message` is a safe public contract type. Native storage and FFI ownership
/// rules are handled by the private runtime implementation.
pub struct Message {
    pub(crate) inner: crate::internal::MessageStorage,
}

// A Message owns one inline native frame. Moving that ownership between threads
// is safe; sharing one mutable frame concurrently is intentionally not exposed
// by the API.
unsafe impl Send for Message {}
