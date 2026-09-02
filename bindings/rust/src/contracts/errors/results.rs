#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
/// The outcome of submitting a send or publish.
pub enum SubmitResult {
    /// The operation succeeded.
    Ok = 0,
    /// Refused because the outbound queue was full.
    Backpressured = 1,
    /// No connected peer was available for the operation.
    NotConnected = 2,
    /// The target was not found.
    NotFound = 3,
    /// The context was terminated while the operation was in flight.
    Terminated = 4,
    /// The target handle was invalid or already closed.
    InvalidHandle = 5,
    /// An argument was invalid.
    InvalidArgument = 6,
    /// The operation is not supported.
    NotSupported = 7,
    /// The target was in a state that does not allow the operation.
    InvalidState = 8,
    /// The handle was used from a thread that does not own it.
    ThreadViolation = 9,
    /// Memory could not be allocated for the operation.
    OutOfMemory = 10,
    /// The request sequence space was exhausted.
    SeqExhausted = 11,
    /// An unexpected internal error occurred.
    InternalError = 12,
    /// Rejected by an admission policy before sending.
    NotAdmitted = 13,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
/// The outcome of a request.
pub enum RequestResult {
    /// The operation succeeded.
    Ok = 0,
    /// No reply arrived within the request timeout.
    TimedOut = 101,
    /// The target was not found.
    NotFound = 102,
    /// The context was terminated while the operation was in flight.
    Terminated = 103,
    /// The reply violated the request/reply protocol.
    ProtocolError = 104,
    /// An unexpected internal error occurred.
    InternalError = 105,
    /// The responder rejected the request.
    Rejected = 106,
    /// The operation conflicted with existing state.
    Conflict = 107,
    /// The resource was busy and could not service the request.
    Busy = 108,
    /// No connected peer was available for the operation.
    NotConnected = 109,
    /// An argument was invalid.
    InvalidArgument = 110,
    /// The target was in a state that does not allow the operation.
    InvalidState = 111,
    /// The operation is not supported.
    NotSupported = 112,
    /// The request could not be admitted because the outbound queue was full.
    Backpressured = 113,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
/// The outcome of a receive.
pub enum RecvResult {
    /// The operation succeeded.
    Ok = 0,
    /// No message was available on a non-blocking receive.
    NoData = 201,
    /// The resource was busy and could not service the request.
    Busy = 202,
    /// The context was terminated while the operation was in flight.
    Terminated = 203,
    /// The target handle was invalid or already closed.
    InvalidHandle = 204,
    /// The operation is not supported.
    NotSupported = 205,
    /// An unexpected internal error occurred.
    InternalError = 206,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
/// The outcome of registering or running a callback handler.
pub enum HandlerResult {
    /// The operation succeeded.
    Ok = 0,
    /// An argument was invalid.
    InvalidArgument = 301,
    /// The resource was busy and could not service the request.
    Busy = 302,
    /// The operation is not supported.
    NotSupported = 303,
    /// The call would deadlock, such as invoking it from its own callback.
    Deadlock = 304,
    /// The target handle was invalid or already closed.
    InvalidHandle = 305,
    /// An unexpected internal error occurred.
    InternalError = 306,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
/// The outcome of closing a socket or resource.
pub enum CloseResult {
    /// The operation succeeded.
    Ok = 0,
    /// The resource was busy and could not service the request.
    Busy = 401,
    /// The context was already shutting down.
    Shutdown = 402,
    /// The target handle was invalid or already closed.
    InvalidHandle = 403,
    /// An unexpected internal error occurred.
    InternalError = 404,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
/// The outcome of binding to an endpoint.
pub enum BindResult {
    /// The operation succeeded.
    Ok = 0,
    /// An argument was invalid.
    InvalidArgument = 501,
    /// The endpoint address was already in use.
    AddrInUse = 502,
    /// The operation is not supported.
    NotSupported = 503,
    /// The target handle was invalid or already closed.
    InvalidHandle = 504,
    /// An unexpected internal error occurred.
    InternalError = 505,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
/// The outcome of connecting to an endpoint.
pub enum ConnectResult {
    /// The operation succeeded.
    Ok = 0,
    /// An argument was invalid.
    InvalidArgument = 601,
    /// The operation is not supported.
    NotSupported = 602,
    /// The target handle was invalid or already closed.
    InvalidHandle = 603,
    /// An unexpected internal error occurred.
    InternalError = 604,
    /// The target was not found.
    NotFound = 605,
    /// The operation conflicted with existing state.
    Conflict = 606,
    /// The resource was busy and could not service the request.
    Busy = 607,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
/// The outcome of reading or applying a configuration option.
pub enum ConfigResult {
    /// The operation succeeded.
    Ok = 0,
    /// The target handle was invalid or already closed.
    InvalidHandle = 701,
    /// An argument was invalid.
    InvalidArgument = 702,
    /// The operation is not supported.
    NotSupported = 703,
    /// An unexpected internal error occurred.
    InternalError = 704,
    /// The target was in a state that does not allow the operation.
    InvalidState = 705,
    /// The target was not found.
    NotFound = 706,
}
