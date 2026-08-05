// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Thrown when submitting a send or publish fails.
/// </summary>
public sealed partial class ZlinkSubmitException : ZlinkException
{
    /// <summary>
    ///     The result codes this operation can fail with.
    /// </summary>
    public enum ErrorCode
    {
        /// <summary>
        ///     The operation succeeded.
        /// </summary>
        Ok = 0,

        /// <summary>
        ///     The send was refused because the outbound queue was full.
        /// </summary>
        Backpressured = 1,

        /// <summary>
        ///     No connected peer was available for the operation.
        /// </summary>
        NotConnected = 2,

        /// <summary>
        ///     The target was not found.
        /// </summary>
        NotFound = 3,

        /// <summary>
        ///     The context was terminated while the operation was in flight.
        /// </summary>
        Terminated = 4,

        /// <summary>
        ///     The target handle was invalid or already closed.
        /// </summary>
        InvalidHandle = 5,

        /// <summary>
        ///     An argument was invalid.
        /// </summary>
        InvalidArgument = 6,

        /// <summary>
        ///     The operation is not supported.
        /// </summary>
        NotSupported = 7,

        /// <summary>
        ///     The target was in a state that does not allow the operation.
        /// </summary>
        InvalidState = 8,

        /// <summary>
        ///     The handle was used from a thread that does not own it.
        /// </summary>
        ThreadViolation = 9,

        /// <summary>
        ///     Memory could not be allocated for the operation.
        /// </summary>
        OutOfMemory = 10,

        /// <summary>
        ///     The request sequence space was exhausted.
        /// </summary>
        SeqExhausted = 11,

        /// <summary>
        ///     An unexpected internal error occurred.
        /// </summary>
        InternalError = 12,

        /// <summary>
        ///     The message was rejected by an admission policy before sending.
        /// </summary>
        NotAdmitted = 13
    }

    /// <summary>
    ///     Creates a zlink submit exception instance.
    /// </summary>
    public ZlinkSubmitException(ErrorCode result)
        : this(ValidatePublicErrorCode(result), 0)
    {
    }

    /// <summary>
    ///     Creates a zlink submit exception instance.
    /// </summary>
    internal ZlinkSubmitException(ErrorCode result, int nativeErrno)
        : base((int)result, nativeErrno)
    {
        Result = result;
    }

    /// <summary>
    ///     Gets the result code that classifies this failure.
    /// </summary>
    public ErrorCode Result { get; }
}

/// <summary>
///     Thrown when a request fails or its reply reports an error.
/// </summary>
public sealed partial class ZlinkRequestException : ZlinkException
{
    /// <summary>
    ///     The result codes this operation can fail with.
    /// </summary>
    public enum ErrorCode
    {
        /// <summary>
        ///     The operation succeeded.
        /// </summary>
        Ok = 0,

        /// <summary>
        ///     No reply arrived within the request timeout.
        /// </summary>
        TimedOut = 101,

        /// <summary>
        ///     The target was not found.
        /// </summary>
        NotFound = 102,

        /// <summary>
        ///     The context was terminated while the operation was in flight.
        /// </summary>
        Terminated = 103,

        /// <summary>
        ///     The reply violated the request/reply protocol.
        /// </summary>
        ProtocolError = 104,

        /// <summary>
        ///     An unexpected internal error occurred.
        /// </summary>
        InternalError = 105,

        /// <summary>
        ///     The responder rejected the request.
        /// </summary>
        Rejected = 106,

        /// <summary>
        ///     The operation conflicted with existing state.
        /// </summary>
        Conflict = 107,

        /// <summary>
        ///     The resource was busy and could not service the request.
        /// </summary>
        Busy = 108,

        /// <summary>
        ///     No connected peer was available for the operation.
        /// </summary>
        NotConnected = 109,

        /// <summary>
        ///     An argument was invalid.
        /// </summary>
        InvalidArgument = 110,

        /// <summary>
        ///     The target was in a state that does not allow the operation.
        /// </summary>
        InvalidState = 111,

        /// <summary>
        ///     The operation is not supported.
        /// </summary>
        NotSupported = 112
    }

    /// <summary>
    ///     Creates a zlink request exception instance.
    /// </summary>
    public ZlinkRequestException(ErrorCode result)
        : this(ValidatePublicErrorCode(result), 0)
    {
    }

    /// <summary>
    ///     Creates a zlink request exception instance.
    /// </summary>
    internal ZlinkRequestException(ErrorCode result, int nativeErrno)
        : base((int)result, nativeErrno)
    {
        Result = result;
    }

    /// <summary>
    ///     Gets the result code that classifies this failure.
    /// </summary>
    public ErrorCode Result { get; }
}

/// <summary>
///     Thrown when receiving a message fails.
/// </summary>
public sealed partial class ZlinkRecvException : ZlinkException
{
    /// <summary>
    ///     The result codes this operation can fail with.
    /// </summary>
    public enum ErrorCode
    {
        /// <summary>
        ///     The operation succeeded.
        /// </summary>
        Ok = 0,

        /// <summary>
        ///     No message was available on a non-blocking receive.
        /// </summary>
        NoData = 201,

        /// <summary>
        ///     The resource was busy and could not service the request.
        /// </summary>
        Busy = 202,

        /// <summary>
        ///     The context was terminated while the operation was in flight.
        /// </summary>
        Terminated = 203,

        /// <summary>
        ///     The target handle was invalid or already closed.
        /// </summary>
        InvalidHandle = 204,

        /// <summary>
        ///     The operation is not supported.
        /// </summary>
        NotSupported = 205,

        /// <summary>
        ///     An unexpected internal error occurred.
        /// </summary>
        InternalError = 206
    }

    /// <summary>
    ///     Creates a zlink receive exception instance.
    /// </summary>
    public ZlinkRecvException(ErrorCode result)
        : this(ValidatePublicErrorCode(result), 0)
    {
    }

    /// <summary>
    ///     Creates a zlink receive exception instance.
    /// </summary>
    internal ZlinkRecvException(ErrorCode result, int nativeErrno)
        : base((int)result, nativeErrno)
    {
        Result = result;
    }

    /// <summary>
    ///     Gets the result code that classifies this failure.
    /// </summary>
    public ErrorCode Result { get; }
}

/// <summary>
///     Thrown when registering or running a callback handler fails.
/// </summary>
public sealed partial class ZlinkHandlerException : ZlinkException
{
    /// <summary>
    ///     The result codes this operation can fail with.
    /// </summary>
    public enum ErrorCode
    {
        /// <summary>
        ///     The operation succeeded.
        /// </summary>
        Ok = 0,

        /// <summary>
        ///     An argument was invalid.
        /// </summary>
        InvalidArgument = 301,

        /// <summary>
        ///     The resource was busy and could not service the request.
        /// </summary>
        Busy = 302,

        /// <summary>
        ///     The operation is not supported.
        /// </summary>
        NotSupported = 303,

        /// <summary>
        ///     The call would deadlock, such as invoking it from its own callback.
        /// </summary>
        Deadlock = 304,

        /// <summary>
        ///     The target handle was invalid or already closed.
        /// </summary>
        InvalidHandle = 305,

        /// <summary>
        ///     An unexpected internal error occurred.
        /// </summary>
        InternalError = 306
    }

    /// <summary>
    ///     Creates a zlink handler exception instance.
    /// </summary>
    public ZlinkHandlerException(ErrorCode result)
        : this(ValidatePublicErrorCode(result), 0)
    {
    }

    /// <summary>
    ///     Creates a zlink handler exception instance.
    /// </summary>
    internal ZlinkHandlerException(ErrorCode result, int nativeErrno)
        : base((int)result, nativeErrno)
    {
        Result = result;
    }

    /// <summary>
    ///     Gets the result code that classifies this failure.
    /// </summary>
    public ErrorCode Result { get; }
}

/// <summary>
///     Thrown when closing a socket or resource fails.
/// </summary>
public sealed partial class ZlinkCloseException : ZlinkException
{
    /// <summary>
    ///     The result codes this operation can fail with.
    /// </summary>
    public enum ErrorCode
    {
        /// <summary>
        ///     The operation succeeded.
        /// </summary>
        Ok = 0,

        /// <summary>
        ///     The resource was busy and could not service the request.
        /// </summary>
        Busy = 401,

        /// <summary>
        ///     The context was already shutting down.
        /// </summary>
        Shutdown = 402,

        /// <summary>
        ///     The target handle was invalid or already closed.
        /// </summary>
        InvalidHandle = 403,

        /// <summary>
        ///     An unexpected internal error occurred.
        /// </summary>
        InternalError = 404
    }

    /// <summary>
    ///     Creates a zlink close exception instance.
    /// </summary>
    public ZlinkCloseException(ErrorCode result)
        : this(ValidatePublicErrorCode(result), 0)
    {
    }

    /// <summary>
    ///     Creates a zlink close exception instance.
    /// </summary>
    internal ZlinkCloseException(ErrorCode result, int nativeErrno)
        : base((int)result, nativeErrno)
    {
        Result = result;
    }

    /// <summary>
    ///     Gets the result code that classifies this failure.
    /// </summary>
    public ErrorCode Result { get; }
}

/// <summary>
///     Thrown when binding a socket to an endpoint fails.
/// </summary>
public sealed partial class ZlinkBindException : ZlinkException
{
    /// <summary>
    ///     The result codes this operation can fail with.
    /// </summary>
    public enum ErrorCode
    {
        /// <summary>
        ///     The operation succeeded.
        /// </summary>
        Ok = 0,

        /// <summary>
        ///     An argument was invalid.
        /// </summary>
        InvalidArgument = 501,

        /// <summary>
        ///     The endpoint address was already in use.
        /// </summary>
        AddrInUse = 502,

        /// <summary>
        ///     The operation is not supported.
        /// </summary>
        NotSupported = 503,

        /// <summary>
        ///     The target handle was invalid or already closed.
        /// </summary>
        InvalidHandle = 504,

        /// <summary>
        ///     An unexpected internal error occurred.
        /// </summary>
        InternalError = 505
    }

    /// <summary>
    ///     Creates a zlink bind exception instance.
    /// </summary>
    public ZlinkBindException(ErrorCode result)
        : this(ValidatePublicErrorCode(result), 0)
    {
    }

    /// <summary>
    ///     Creates a zlink bind exception instance.
    /// </summary>
    internal ZlinkBindException(ErrorCode result, int nativeErrno)
        : base((int)result, nativeErrno)
    {
        Result = result;
    }

    /// <summary>
    ///     Gets the result code that classifies this failure.
    /// </summary>
    public ErrorCode Result { get; }
}

/// <summary>
///     Thrown when connecting a socket to an endpoint fails.
/// </summary>
public sealed partial class ZlinkConnectException : ZlinkException
{
    /// <summary>
    ///     The result codes this operation can fail with.
    /// </summary>
    public enum ErrorCode
    {
        /// <summary>
        ///     The operation succeeded.
        /// </summary>
        Ok = 0,

        /// <summary>
        ///     An argument was invalid.
        /// </summary>
        InvalidArgument = 601,

        /// <summary>
        ///     The operation is not supported.
        /// </summary>
        NotSupported = 602,

        /// <summary>
        ///     The target handle was invalid or already closed.
        /// </summary>
        InvalidHandle = 603,

        /// <summary>
        ///     An unexpected internal error occurred.
        /// </summary>
        InternalError = 604,

        /// <summary>
        ///     The target was not found.
        /// </summary>
        NotFound = 605,

        /// <summary>
        ///     The operation conflicted with existing state.
        /// </summary>
        Conflict = 606,

        /// <summary>
        ///     The resource was busy and could not service the request.
        /// </summary>
        Busy = 607
    }

    /// <summary>
    ///     Creates a zlink connect exception instance.
    /// </summary>
    public ZlinkConnectException(ErrorCode result)
        : this(ValidatePublicErrorCode(result), 0)
    {
    }

    /// <summary>
    ///     Creates a zlink connect exception instance.
    /// </summary>
    internal ZlinkConnectException(ErrorCode result, int nativeErrno)
        : base((int)result, nativeErrno)
    {
        Result = result;
    }

    /// <summary>
    ///     Gets the result code that classifies this failure.
    /// </summary>
    public ErrorCode Result { get; }
}

/// <summary>
///     Thrown when reading or applying a configuration option fails.
/// </summary>
public sealed partial class ZlinkConfigException : ZlinkException
{
    /// <summary>
    ///     The result codes this operation can fail with.
    /// </summary>
    public enum ErrorCode
    {
        /// <summary>
        ///     The operation succeeded.
        /// </summary>
        Ok = 0,

        /// <summary>
        ///     The target handle was invalid or already closed.
        /// </summary>
        InvalidHandle = 701,

        /// <summary>
        ///     An argument was invalid.
        /// </summary>
        InvalidArgument = 702,

        /// <summary>
        ///     The operation is not supported.
        /// </summary>
        NotSupported = 703,

        /// <summary>
        ///     An unexpected internal error occurred.
        /// </summary>
        InternalError = 704,

        /// <summary>
        ///     The target was in a state that does not allow the operation.
        /// </summary>
        InvalidState = 705,

        /// <summary>
        ///     The target was not found.
        /// </summary>
        NotFound = 706,

        /// <summary>
        ///     The requested identity, name, or binding already exists.
        /// </summary>
        Conflict = 707,

        /// <summary>
        ///     The caller-provided output capacity is too small.
        /// </summary>
        BufferTooSmall = 708,

        /// <summary>
        ///     A mutable object is already in concurrent use.
        /// </summary>
        Busy = 709
    }

    /// <summary>
    ///     Creates a zlink config exception instance.
    /// </summary>
    public ZlinkConfigException(ErrorCode result)
        : this(ValidatePublicErrorCode(result), 0)
    {
    }

    /// <summary>
    ///     Creates a zlink config exception instance.
    /// </summary>
    internal ZlinkConfigException(ErrorCode result, int nativeErrno)
        : base((int)result, nativeErrno)
    {
        Result = result;
    }

    /// <summary>
    ///     Gets the result code that classifies this failure.
    /// </summary>
    public ErrorCode Result { get; }
}
