use std::fmt;

pub use crate::results::{
    BindResult, CloseResult, ConfigResult, ConnectResult, HandlerResult, RecvResult, RequestResult,
    SubmitResult,
};

macro_rules! define_error_type {
    ($name:ident, $result:ident, $variant:ident, $doc:literal) => {
        #[doc = $doc]
        #[derive(Debug, Clone, Copy, PartialEq, Eq)]
        pub struct $name {
            /// The typed result code that classifies this failure.
            pub code: $result,
            /// The underlying native errno, or 0 when none.
            pub native_errno: i32,
        }

        impl $name {
            /// Creates an error from a typed result `code` and native errno.
            pub const fn new(code: $result, native_errno: i32) -> Self {
                Self { code, native_errno }
            }

            /// Returns the typed result code that classifies this failure.
            pub const fn code(&self) -> $result {
                self.code
            }

            /// Returns the underlying native errno, or 0 when none.
            pub const fn native_errno(&self) -> i32 {
                self.native_errno
            }
        }

        impl fmt::Display for $name {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                write!(
                    f,
                    "{}(code={:?}, native_errno={})",
                    stringify!($name),
                    self.code,
                    self.native_errno
                )
            }
        }

        impl std::error::Error for $name {}

        impl From<$name> for ZlinkError {
            fn from(value: $name) -> Self {
                ZlinkError::$variant(value)
            }
        }
    };
}

define_error_type!(
    SubmitError,
    SubmitResult,
    Submit,
    "Error from submitting a send or publish."
);
define_error_type!(
    RequestError,
    RequestResult,
    Request,
    "Error from a request, or a reply that reported failure."
);
define_error_type!(
    RecvError,
    RecvResult,
    Recv,
    "Error from receiving a message."
);
define_error_type!(
    HandlerError,
    HandlerResult,
    Handler,
    "Error from registering or running a callback handler."
);
define_error_type!(
    CloseError,
    CloseResult,
    Close,
    "Error from closing a socket or resource."
);
define_error_type!(
    BindError,
    BindResult,
    Bind,
    "Error from binding to an endpoint."
);
define_error_type!(
    ConnectError,
    ConnectResult,
    Connect,
    "Error from connecting to an endpoint."
);
define_error_type!(
    ConfigError,
    ConfigResult,
    Config,
    "Error from reading or applying a configuration option."
);

/// Any error raised by the binding, tagged by the operation that produced it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ZlinkError {
    /// A send or publish submit failed.
    Submit(SubmitError),
    /// A request failed.
    Request(RequestError),
    /// A receive failed.
    Recv(RecvError),
    /// A callback handler operation failed.
    Handler(HandlerError),
    /// Closing a resource failed.
    Close(CloseError),
    /// Binding to an endpoint failed.
    Bind(BindError),
    /// Connecting to an endpoint failed.
    Connect(ConnectError),
    /// Reading or applying a configuration option failed.
    Config(ConfigError),
}

impl ZlinkError {
    /// Returns the typed result code as its raw integer value.
    pub fn code(&self) -> i32 {
        match self {
            Self::Submit(err) => err.code as i32,
            Self::Request(err) => err.code as i32,
            Self::Recv(err) => err.code as i32,
            Self::Handler(err) => err.code as i32,
            Self::Close(err) => err.code as i32,
            Self::Bind(err) => err.code as i32,
            Self::Connect(err) => err.code as i32,
            Self::Config(err) => err.code as i32,
        }
    }

    /// Returns the underlying native errno, or 0 when none.
    pub fn native_errno(&self) -> i32 {
        match self {
            Self::Submit(err) => err.native_errno,
            Self::Request(err) => err.native_errno,
            Self::Recv(err) => err.native_errno,
            Self::Handler(err) => err.native_errno,
            Self::Close(err) => err.native_errno,
            Self::Bind(err) => err.native_errno,
            Self::Connect(err) => err.native_errno,
            Self::Config(err) => err.native_errno,
        }
    }
}

impl fmt::Display for ZlinkError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Submit(err) => err.fmt(f),
            Self::Request(err) => err.fmt(f),
            Self::Recv(err) => err.fmt(f),
            Self::Handler(err) => err.fmt(f),
            Self::Close(err) => err.fmt(f),
            Self::Bind(err) => err.fmt(f),
            Self::Connect(err) => err.fmt(f),
            Self::Config(err) => err.fmt(f),
        }
    }
}

impl std::error::Error for ZlinkError {}
