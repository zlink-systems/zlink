use crate::error::{ConfigError, ConfigResult};

pub use crate::routing_id::RoutingId;

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

impl Message {
    /// Create an empty (zero-length) message.
    pub fn new() -> Result<Self, ConfigError> {
        crate::message_factory::message_new()
    }

    /// Create a message of the given size filled with uninitialized bytes.
    pub fn with_size(size: usize) -> Result<Self, ConfigError> {
        crate::message_factory::message_with_size(size)
    }

    /// Allocates a message with writable payload storage of `size` bytes; an
    /// alias for [`Message::with_size`].
    pub fn allocate(size: usize) -> Result<Self, ConfigError> {
        Self::with_size(size)
    }

    /// Create a message by copying the given byte source.
    pub fn try_from<T: AsRef<[u8]>>(data: T) -> Result<Self, ConfigError> {
        crate::message_factory::message_from_slice(data.as_ref())
    }

    /// View the message payload as a byte slice.
    pub fn as_bytes(&self) -> &[u8] {
        crate::message_factory::message_as_bytes(self)
    }

    pub(crate) fn data(&self) -> &[u8] {
        self.as_bytes()
    }

    /// View the message payload as a mutable byte slice.
    pub fn data_mut(&mut self) -> &mut [u8] {
        crate::message_factory::message_data_mut(self)
    }

    /// Message size in bytes.
    pub fn size(&self) -> usize {
        crate::message_factory::message_size(self)
    }

    /// Returns `true` if the message has zero-length payload.
    pub fn is_empty(&self) -> bool {
        self.size() == 0
    }

    /// Interpret the payload as a UTF-8 string.
    pub fn as_str(&self) -> Result<&str, std::str::Utf8Error> {
        std::str::from_utf8(self.data())
    }

    /// Returns a new `Vec<u8>` holding a copy of the payload.
    pub fn to_vec(&self) -> Vec<u8> {
        self.as_bytes().to_vec()
    }

    /// Copies the payload into `destination`, returning the number of bytes
    /// written. Returns an error when `destination` is smaller than the payload.
    pub fn copy_to(&self, destination: &mut [u8]) -> Result<usize, ConfigError> {
        let bytes = self.as_bytes();
        if destination.len() < bytes.len() {
            return Err(ConfigError::new(
                ConfigResult::InvalidArgument,
                libc::EINVAL,
            ));
        }
        destination[..bytes.len()].copy_from_slice(bytes);
        Ok(bytes.len())
    }

    /// Return the native storage reference count for the message.
    ///
    /// This is a diagnostic helper only. It does not affect ownership or
    /// message lifetime.
    pub fn ref_count(&self) -> i32 {
        crate::message_factory::message_ref_count(self)
    }

    /// Returns an independent copy of this message that owns its own payload.
    pub fn try_clone(&self) -> Result<Self, ConfigError> {
        crate::message_factory::message_try_clone(self)
    }
}

impl Drop for Message {
    fn drop(&mut self) {
        crate::message_factory::message_drop(self);
    }
}

impl TryFrom<&[u8]> for Message {
    type Error = ConfigError;

    /// Creates a message holding an independent copy of the bytes.
    fn try_from(data: &[u8]) -> Result<Self, ConfigError> {
        crate::message_factory::message_from_slice(data)
    }
}

impl TryFrom<Vec<u8>> for Message {
    type Error = ConfigError;

    /// Creates a message holding an independent copy of the bytes.
    fn try_from(v: Vec<u8>) -> Result<Self, ConfigError> {
        crate::message_factory::message_from_slice(&v)
    }
}

impl TryFrom<&str> for Message {
    type Error = ConfigError;

    /// Creates a message holding the UTF-8 bytes of the string.
    fn try_from(value: &str) -> Result<Self, ConfigError> {
        crate::message_factory::message_from_slice(value.as_bytes())
    }
}
