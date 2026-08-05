use crate::error::ConfigError;
use crate::results::ConfigResult;

/// A validated routing identifier.
///
/// The 255 byte bound maps to the C routing-id contract, but this public value
/// does not expose the native mirror type.
#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
pub struct RoutingId {
    pub(crate) size: u8,
    pub(crate) data: [u8; 255],
}

impl RoutingId {
    /// The maximum routing id length, in bytes.
    pub const MAX_LEN: usize = 255;

    fn from_bytes(data: &[u8]) -> Self {
        if data.is_empty() {
            panic!("routing id must not be empty");
        }
        if data.len() > Self::MAX_LEN {
            panic!(
                "routing id length {} exceeds maximum {}",
                data.len(),
                Self::MAX_LEN
            );
        }
        let mut raw = Self {
            size: data.len() as u8,
            data: [0u8; 255],
        };
        raw.data[..data.len()].copy_from_slice(data);
        raw
    }

    /// Creates a routing id by decoding `value` as a hex string.
    ///
    /// # Panics
    /// Panics when `value` is not valid hex of 1 to 255 bytes; use
    /// [`try_from_hex`](Self::try_from_hex) to handle errors instead.
    pub fn from_hex(value: &str) -> Self {
        Self::try_from_hex(value).expect("invalid routing id hex string")
    }

    /// Creates a routing id by decoding `value` as a hex string (even length,
    /// up to 510 digits for 255 bytes), returning an error on invalid input.
    pub fn try_from_hex(value: &str) -> Result<Self, ConfigError> {
        if value.is_empty() || value.len() % 2 != 0 || value.len() > Self::MAX_LEN * 2 {
            return Err(validation_error());
        }
        let mut data = Vec::with_capacity(value.len() / 2);
        let bytes = value.as_bytes();
        for pair in bytes.chunks_exact(2) {
            let high = hex_nibble(pair[0]).ok_or_else(validation_error)?;
            let low = hex_nibble(pair[1]).ok_or_else(validation_error)?;
            data.push((high << 4) | low);
        }
        Self::new(&data)
    }

    pub(crate) fn new(data: &[u8]) -> Result<Self, ConfigError> {
        if data.is_empty() || data.len() > Self::MAX_LEN {
            return Err(validation_error());
        }
        Ok(Self::from_bytes(data))
    }

    /// Returns the routing id bytes.
    pub fn as_bytes(&self) -> &[u8] {
        &self.data[..self.size as usize]
    }

    pub(crate) fn data(&self) -> &[u8] {
        self.as_bytes()
    }

    /// Returns the length of the routing id in bytes.
    pub fn size(&self) -> usize {
        self.size as usize
    }

    pub(crate) fn len(&self) -> usize {
        self.size()
    }

    /// Returns `true` when the routing id has no bytes.
    pub fn is_empty(&self) -> bool {
        self.size == 0
    }

    /// Returns the routing id as a lowercase hex string.
    pub fn to_hex(&self) -> String {
        let mut out = String::with_capacity(self.size() * 2);
        for byte in self.as_bytes() {
            use std::fmt::Write as _;
            let _ = write!(&mut out, "{byte:02x}");
        }
        out
    }
}

fn validation_error() -> ConfigError {
    ConfigError::new(ConfigResult::InvalidArgument, libc::EINVAL)
}

fn hex_nibble(value: u8) -> Option<u8> {
    match value {
        b'0'..=b'9' => Some(value - b'0'),
        b'a'..=b'f' => Some(value - b'a' + 10),
        b'A'..=b'F' => Some(value - b'A' + 10),
        _ => None,
    }
}

impl std::fmt::Display for RoutingId {
    /// Formats as printable UTF-8 text when possible, otherwise as a `u32`, a
    /// UUID, or a `hex:` fallback.
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if let Ok(text) = std::str::from_utf8(self.as_bytes()) {
            if !text.chars().any(char::is_control) {
                return f.write_str(text);
            }
        }
        if self.size() == 4 {
            let mut bytes = [0u8; 4];
            bytes.copy_from_slice(self.as_bytes());
            return write!(f, "{}", u32::from_be_bytes(bytes));
        }
        if self.size() == 16 {
            let bytes = self.as_bytes();
            return write!(
                f,
                "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                bytes[0],
                bytes[1],
                bytes[2],
                bytes[3],
                bytes[4],
                bytes[5],
                bytes[6],
                bytes[7],
                bytes[8],
                bytes[9],
                bytes[10],
                bytes[11],
                bytes[12],
                bytes[13],
                bytes[14],
                bytes[15]
            );
        }
        write!(f, "hex:{}", self.to_hex())
    }
}

impl From<&[u8]> for RoutingId {
    /// Creates a routing id from a copy of the bytes.
    ///
    /// # Panics
    /// Panics when the input is empty or longer than [`RoutingId::MAX_LEN`].
    fn from(data: &[u8]) -> Self {
        Self::from_bytes(data)
    }
}

impl<const N: usize> From<&[u8; N]> for RoutingId {
    /// Creates a routing id from a copy of the bytes.
    ///
    /// # Panics
    /// Panics when the input is empty or longer than [`RoutingId::MAX_LEN`].
    fn from(data: &[u8; N]) -> Self {
        Self::from_bytes(data)
    }
}

impl From<&str> for RoutingId {
    /// Creates a routing id from the UTF-8 bytes of `value`.
    ///
    /// # Panics
    /// Panics when the encoded input is empty or longer than
    /// [`RoutingId::MAX_LEN`].
    fn from(value: &str) -> Self {
        Self::from_bytes(value.as_bytes())
    }
}

impl From<u32> for RoutingId {
    /// Creates a 4-byte routing id from `value` in big-endian order.
    fn from(value: u32) -> Self {
        Self::from_bytes(&value.to_be_bytes())
    }
}

impl From<[u8; 16]> for RoutingId {
    /// Creates a 16-byte routing id from the given UUID bytes.
    fn from(value: [u8; 16]) -> Self {
        Self::from_bytes(&value)
    }
}
