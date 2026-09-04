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
///     caller for retry or disposal. <see cref="SendSubmitOperation.TrySubmit" />
///     also leaves the parts with the caller when it returns <c>false</c>.
///     <see cref="SendSubmitOperation.Async" /> transfers the parts to the
///     operation once its initial attempt succeeds or obtains a WRITABLE wait
///     token. The request, reply, and actor-join builders in this file share the
///     successful-submit ownership model.
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
///     Accepts further parts and the blocking or awaitable terminal of a send
///     builder.
/// </summary>
public interface SendSubmitOperation
{
    /// <summary>
    ///     Adds another message part. The part is consumed on a successful submit;
    ///     see <see cref="SendOperation" /> for the ownership contract.
    /// </summary>
    SendSubmitOperation Message(Message message);

    /// <summary>
    ///     Blocks until Core admits the accumulated record locally.
    /// </summary>
    void Submit();

    /// <summary>
    ///     Makes one non-blocking admission attempt. Returns <c>false</c> only
    ///     when Core reports <see cref="SubmitResult.Backpressured" /> with
    ///     <c>EAGAIN</c>; in that case Core retained no payload and every message
    ///     remains owned by the caller.
    /// </summary>
    bool TrySubmit();

    /// <summary>
    ///     Makes non-blocking admission attempts. If Core reports backpressure,
    ///     the operation retains an exact packet snapshot, waits for the matching
    ///     <see cref="CompletionKind.Writable" /> token on <c>POLLOUT</c>, and
    ///     retries that packet. Ordinary successful SEND admission completes
    ///     immediately and produces no native completion.
    /// </summary>
    /// <remarks>
    ///     Once the initial attempt is admitted or returns a WRITABLE wait token,
    ///     this method consumes the caller's message wrappers and the operation
    ///     owns its retained packet until success, cancellation, or failure.
    /// </remarks>
    Task Async(CancellationToken cancellationToken = default);
}

/// <summary>
///     Builds a topic publish. Publish is a synchronous one-shot: the default
///     PUB contract is lossy, so a subscriber at its high-water mark has its
///     copy dropped and the publisher proceeds immediately.
/// </summary>
public interface PublishOperation
{
    /// <summary>
    ///     Adds the first message part. The part is consumed on a successful
    ///     submit; see <see cref="SendOperation" /> for the ownership contract.
    /// </summary>
    PublishSubmitOperation Message(Message message);
}

/// <summary>
///     Accepts further parts, flags, and the synchronous terminal submit of a
///     publish builder.
/// </summary>
public interface PublishSubmitOperation
{
    /// <summary>Adds another message part.</summary>
    PublishSubmitOperation Message(Message message);

    /// <summary>
    ///     Sets the flags applied at submit time, replacing any previously set
    ///     flags.
    /// </summary>
    PublishSubmitOperation Flags(SendFlags flags);

    /// <summary>
    ///     Publishes the accumulated parts on the calling thread. The publisher
    ///     never waits at the high-water mark. Failures — including the
    ///     immediate back-pressure a <c>NODROP</c> publisher surfaces — throw
    ///     <see cref="ZlinkSubmitException" />.
    /// </summary>
    void Submit();
}

/// <summary>Builds a non-blocking publish that reports immediate refusal.</summary>
public interface TryPublishOperation
{
    /// <summary>Adds the first message part.</summary>
    TryPublishSubmitOperation Message(Message message);
}

/// <summary>Completes a non-blocking publish attempt.</summary>
public interface TryPublishSubmitOperation
{
    /// <summary>Adds another message part.</summary>
    TryPublishSubmitOperation Message(Message message);

    /// <summary>Sets publish-specific flags.</summary>
    TryPublishSubmitOperation Flags(SendFlags flags);

    /// <summary>Returns false only for immediate publish backpressure.</summary>
    bool Submit();
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
    ///     Blocks until Core completes the request and returns the reply parts.
    /// </summary>
    IReadOnlyList<Message> Submit();

    /// <summary>
    ///     Transfers the request parts to the operation, asynchronously waits for
    ///     exact-target admission, and returns the reply parts.
    /// </summary>
    /// <remarks>
    ///     The caller owns the returned messages and must dispose them. Calling
    ///     this method transfers all accumulated request parts to the pending
    ///     operation, including while it waits for exact-target admission.
    /// </remarks>
    Task<IReadOnlyList<Message>> Async(
        CancellationToken cancellationToken = default);
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
