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
///     Builds a send whose terminal operation waits asynchronously for local
///     Core admission.
/// </summary>
public interface AsyncSendOperation
{
    /// <summary>
    ///     Adds the first message part. Ownership transfers when
    ///     <see cref="AsyncSendSubmitOperation.Async" /> is called.
    /// </summary>
    AsyncSendSubmitOperation Message(Message message);
}

/// <summary>
///     Accepts further parts and asynchronously waits until Core admits the
///     complete record.
/// </summary>
public interface AsyncSendSubmitOperation
{
    /// <summary>Adds another message part.</summary>
    AsyncSendSubmitOperation Message(Message message);

    /// <summary>
    ///     Transfers the accumulated parts to the operation and returns without
    ///     waiting for HWM credit. The task completes when Core admits the
    ///     complete record.
    /// </summary>
    Task Async(CancellationToken ct = default);
}

/// <summary>
///     Builds an exact-target DEALER or ROUTER send whose terminal operation is
///     language-native asynchronous admission.
/// </summary>
public interface RoutedSendOperation
{
    /// <summary>
    ///     Adds the first message part. Ownership transfers to the asynchronous
    ///     operation when <see cref="RoutedSendSubmitOperation.Async" /> is
    ///     called.
    /// </summary>
    RoutedSendSubmitOperation Message(Message message);
}

/// <summary>
///     Accepts further parts and asynchronously waits until Core admits the
///     complete routed record.
/// </summary>
public interface RoutedSendSubmitOperation
{
    /// <summary>Adds another message part.</summary>
    RoutedSendSubmitOperation Message(Message message);

    /// <summary>
    ///     Transfers the accumulated parts to the operation and returns without
    ///     waiting for target HWM credit. The task completes when the complete
    ///     record is admitted to the selected exact target.
    /// </summary>
    Task Async(CancellationToken ct = default);
}

/// <summary>
///     Builds a request: add the request parts, then call the asynchronous
///     terminal and await a reply.
/// </summary>
public interface RequestOperation
{
    /// <summary>
    ///     Adds the first request part. Ownership transfers to the asynchronous
    ///     operation when <see cref="RequestSubmitOperation.Async" /> is called.
    /// </summary>
    RequestSubmitOperation Message(Message message);
}

/// <summary>
///     Accepts further parts, timeout, and the asynchronous terminal operation of
///     a request.
/// </summary>
public interface RequestSubmitOperation
{
    /// <summary>
    ///     Adds another request part. Ownership transfers to the asynchronous
    ///     operation when <see cref="Async" /> is called.
    /// </summary>
    RequestSubmitOperation Message(Message message);

    /// <summary>
    ///     Sets how long the submit awaits a reply before the result reports
    ///     <see cref="RequestResult.TimedOut" />, replacing any previous value.
    /// </summary>
    RequestSubmitOperation Timeout(TimeSpan timeout);

    /// <summary>
    ///     Transfers the request parts to the operation, asynchronously waits for
    ///     exact-target admission, and returns the reply parts.
    /// </summary>
    /// <remarks>
    ///     The caller owns the returned messages and must dispose them. Calling
    ///     this method transfers all accumulated request parts to the pending
    ///     operation, including while it waits for exact-target admission.
    /// </remarks>
    Task<IReadOnlyList<Message>> Async(CancellationToken ct = default);
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
///     The outcome of a request.
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
