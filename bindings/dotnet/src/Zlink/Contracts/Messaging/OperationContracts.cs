// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Builds a multipart send: add one or more parts, then
///     <see cref="SendSubmitOperation.Submit" />.
/// </summary>
/// <remarks>
///     Submitting consumes the added <see cref="Message" /> parts. On a successful
///     submit each part's payload is moved into the transport and the managed
///     instance is left empty; reading a consumed part's payload afterward throws,
///     though disposing it stays safe and is still required to return pooled
///     instances. If the submit fails, ownership of every part is restored to the
///     caller for retry or disposal. The request, reply, and actor-join builders in
///     this file share this same ownership model.
/// </remarks>
public interface SendOperation
{
    /// <summary>
    ///     Adds the first message part. The part is consumed on a successful
    ///     submit; see <see cref="SendOperation" /> for the ownership contract.
    /// </summary>
    SendSubmitOperation Message(Message message);
}

/// <summary>
///     Accepts further parts, flags, and the terminal submit of a send builder.
/// </summary>
public interface SendSubmitOperation
{
    /// <summary>
    ///     Adds another message part. The part is consumed on a successful submit;
    ///     see <see cref="SendOperation" /> for the ownership contract.
    /// </summary>
    SendSubmitOperation Message(Message message);

    /// <summary>
    ///     Sets the flags applied at submit time, replacing any previously set
    ///     flags.
    /// </summary>
    SendSubmitOperation Flags(SendFlags flags);

    /// <summary>
    ///     Submits the accumulated parts.
    /// </summary>
    /// <returns>
    ///     true when the parts were queued for sending; false only when
    ///     <see cref="SendFlags.DontWait" /> is set and the send would have blocked
    ///     (back-pressure). Other failures throw <see cref="ZlinkException" />.
    /// </returns>
    bool Submit();
}

/// <summary>
///     Builds a request: add the request parts, then submit and await a reply.
/// </summary>
public interface RequestOperation
{
    /// <summary>
    ///     Adds the first request part. The part is consumed on a successful
    ///     submit; see <see cref="SendOperation" /> for the ownership contract.
    /// </summary>
    RequestSubmitOperation Message(Message message);
}

/// <summary>
///     Accepts further parts, timeout, flags, and the terminal submit of a request.
/// </summary>
public interface RequestSubmitOperation
{
    /// <summary>
    ///     Adds another request part. The part is consumed on a successful submit;
    ///     see <see cref="SendOperation" /> for the ownership contract.
    /// </summary>
    RequestSubmitOperation Message(Message message);

    /// <summary>
    ///     Sets how long the submit awaits a reply before the result reports
    ///     <see cref="RequestResult.TimedOut" />, replacing any previous value.
    /// </summary>
    RequestSubmitOperation Timeout(TimeSpan timeout);

    /// <summary>
    ///     Sets the send flags and narrows the builder to callback submission:
    ///     once flags are set the awaitable <see cref="Async" /> is no longer
    ///     reachable, only <see cref="RequestCallbackSubmitOperation.Submit" />.
    /// </summary>
    RequestCallbackSubmitOperation Flags(SendFlags flags);

    /// <summary>
    ///     Submits the request and asynchronously returns the reply parts.
    /// </summary>
    /// <remarks>
    ///     The caller owns the returned messages and must dispose them. Request
    ///     parts follow the consume-on-submit contract of <see cref="SendOperation" />.
    /// </remarks>
    Task<IReadOnlyList<Message>> Async(CancellationToken ct = default);

    /// <summary>
    ///     Submits the request; the result and reply parts are delivered later to
    ///     <paramref name="callback" /> (see <see cref="RequestCallback" /> for reply
    ///     ownership).
    /// </summary>
    /// <returns>
    ///     true when the request was dispatched; false only when
    ///     <see cref="SendFlags.DontWait" /> is set and the send would have blocked
    ///     (back-pressure). Other failures throw <see cref="ZlinkException" />.
    /// </returns>
    bool Submit(RequestCallback callback);
}

/// <summary>
///     Callback-submission stage of a request builder (reached after
///     <see cref="RequestSubmitOperation.Flags" />).
/// </summary>
public interface RequestCallbackSubmitOperation
{
    /// <summary>
    ///     Adds another request part. The part is consumed on a successful submit;
    ///     see <see cref="SendOperation" /> for the ownership contract.
    /// </summary>
    RequestCallbackSubmitOperation Message(Message message);

    /// <summary>
    ///     Sets how long the submit awaits a reply before the result reports
    ///     <see cref="RequestResult.TimedOut" />, replacing any previous value.
    /// </summary>
    RequestCallbackSubmitOperation Timeout(TimeSpan timeout);

    /// <summary>
    ///     Sets the send flags applied at submit time, replacing any previous flags.
    /// </summary>
    RequestCallbackSubmitOperation Flags(SendFlags flags);

    /// <summary>
    ///     Submits the request; the result and reply parts are delivered later to
    ///     <paramref name="callback" /> (see <see cref="RequestCallback" /> for reply
    ///     ownership).
    /// </summary>
    /// <returns>
    ///     true when the request was dispatched; false only when
    ///     <see cref="SendFlags.DontWait" /> is set and the send would have blocked
    ///     (back-pressure). Other failures throw <see cref="ZlinkException" />.
    /// </returns>
    bool Submit(RequestCallback callback);
}

/// <summary>
///     Builds a reply to a received request: add the reply parts, then submit.
/// </summary>
public interface ReplyOperation
{
    /// <summary>
    ///     Adds the first reply part. The part is consumed on a successful submit;
    ///     see <see cref="SendOperation" /> for the ownership contract.
    /// </summary>
    ReplySubmitOperation Message(Message message);
}

/// <summary>
///     Accepts further parts and the terminal submit of a reply builder.
/// </summary>
public interface ReplySubmitOperation
{
    /// <summary>
    ///     Adds another reply part. The part is consumed on a successful submit;
    ///     see <see cref="SendOperation" /> for the ownership contract.
    /// </summary>
    ReplySubmitOperation Message(Message message);

    /// <summary>
    ///     Submits the reply. Failures throw <see cref="ZlinkException" />.
    /// </summary>
    void Submit();
}

/// <summary>
///     The outcome of a request, as delivered to a <see cref="RequestCallback" />.
/// </summary>
public enum RequestResult
{
    /// <summary>
    ///     The request succeeded and a reply was returned.
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
    ///     The context was terminated while the request was in flight.
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
    ///     The request conflicted with existing state.
    /// </summary>
    Conflict = 107,

    /// <summary>
    ///     The responder was busy and could not service the request.
    /// </summary>
    Busy = 108,

    /// <summary>
    ///     No connected peer was available for the request.
    /// </summary>
    NotConnected = 109,

    /// <summary>
    ///     An argument was invalid.
    /// </summary>
    InvalidArgument = 110,

    /// <summary>
    ///     The target was in a state that does not allow the request.
    /// </summary>
    InvalidState = 111,

    /// <summary>
    ///     The request is not supported.
    /// </summary>
    NotSupported = 112,

    /// <summary>
    ///     The target could not admit the request because its bounded pending
    ///     budget was exhausted.
    /// </summary>
    Backpressured = 113
}

/// <summary>
///     Receives request completion and reply payloads.
/// </summary>
/// <remarks>
///     When <paramref name="result" /> is <see cref="RequestResult.Ok" />, the
///     callback owns the messages in <paramref name="parts" /> and must dispose them.
///     Non-success results provide an empty payload list.
/// </remarks>
public delegate void RequestCallback(RequestResult result,
    IReadOnlyList<Message> parts);
