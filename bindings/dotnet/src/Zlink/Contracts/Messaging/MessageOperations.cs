// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Extension methods that add several message parts to a send, request, reply,
///     or actor-join builder in one call.
/// </summary>
public static class MessageOperations
{
    /// <summary>
    ///     Sends one ROUTER message without allocating a multipart builder on
    ///     the built-in socket implementation. External implementations fall
    ///     back to the equivalent builder contract.
    /// </summary>
    public static Task SendAsync(this IRouterSocket socket,
        RoutingId routingId, Message message,
        CancellationToken cancellationToken = default)
    {
        if (socket == null)
            throw new ArgumentNullException(nameof(socket));
        if (message == null)
            throw new ArgumentNullException(nameof(message));
        return socket is RouterSocket builtIn
            ? builtIn.SendSingleAsyncCore(routingId, message,
                cancellationToken)
            : socket.Send(routingId).Message(message).Async(
                cancellationToken);
    }

    /// <summary>
    ///     Adds <paramref name="messages" /> as parts, in order. The parts are
    ///     consumed on a successful submit; see <see cref="SendOperation" /> for the
    ///     ownership contract.
    /// </summary>
    /// <returns>The same builder, for chaining further parts, flags, or submit.</returns>
    public static SendSubmitOperation Messages(
        this SendOperation operation,
        IReadOnlyList<Message> messages)
    {
        EnsureNotEmpty(messages);
        return operation.Message(messages[0]).Messages(messages, 1);
    }

    /// <summary>
    ///     Adds <paramref name="messages" /> as parts, in order. The parts are
    ///     consumed on a successful submit; see <see cref="SendOperation" /> for the
    ///     ownership contract.
    /// </summary>
    /// <returns>The same builder, for chaining further parts, flags, or submit.</returns>
    public static SendSubmitOperation Messages(
        this SendSubmitOperation operation,
        IReadOnlyList<Message> messages)
    {
        EnsureNotEmpty(messages);
        return operation.Messages(messages, 0);
    }

    /// <summary>
    ///     Adds publish parts in order. The parts are consumed on a successful
    ///     submit.
    /// </summary>
    public static PublishSubmitOperation Messages(
        this PublishOperation operation,
        IReadOnlyList<Message> messages)
    {
        EnsureNotEmpty(messages);
        return operation.Message(messages[0]).Messages(messages, 1);
    }

    /// <summary>
    ///     Adds publish parts in order. The parts are consumed on a successful
    ///     submit.
    /// </summary>
    public static PublishSubmitOperation Messages(
        this PublishSubmitOperation operation,
        IReadOnlyList<Message> messages)
    {
        EnsureNotEmpty(messages);
        return operation.Messages(messages, 0);
    }

    /// <summary>
    ///     Adds routed-send parts in order. Ownership transfers when the builder's
    ///     asynchronous terminal is called.
    /// </summary>
    public static RoutedSendSubmitOperation Messages(
        this RoutedSendOperation operation,
        IReadOnlyList<Message> messages)
    {
        EnsureNotEmpty(messages);
        return operation.Message(messages[0]).Messages(messages, 1);
    }

    /// <summary>
    ///     Adds routed-send parts in order. Ownership transfers when the builder's
    ///     asynchronous terminal is called.
    /// </summary>
    public static RoutedSendSubmitOperation Messages(
        this RoutedSendSubmitOperation operation,
        IReadOnlyList<Message> messages)
    {
        EnsureNotEmpty(messages);
        return operation.Messages(messages, 0);
    }

    /// <summary>
    ///     Adds <paramref name="messages" /> as request parts, in order. Ownership
    ///     transfers when the builder's asynchronous terminal is called.
    /// </summary>
    /// <returns>
    ///     The same builder, for chaining further parts, timeout, or the
    ///     asynchronous terminal.
    /// </returns>
    public static RequestSubmitOperation Messages(
        this RequestOperation operation,
        IReadOnlyList<Message> messages)
    {
        EnsureNotEmpty(messages);
        return operation.Message(messages[0]).Messages(messages, 1);
    }

    /// <summary>
    ///     Adds <paramref name="messages" /> as request parts, in order. Ownership
    ///     transfers when the builder's asynchronous terminal is called.
    /// </summary>
    /// <returns>
    ///     The same builder, for chaining further parts, timeout, or the
    ///     asynchronous terminal.
    /// </returns>
    public static RequestSubmitOperation Messages(
        this RequestSubmitOperation operation,
        IReadOnlyList<Message> messages)
    {
        EnsureNotEmpty(messages);
        return operation.Messages(messages, 0);
    }

    /// <summary>
    ///     Adds <paramref name="messages" /> as parts, in order. The parts are
    ///     consumed on a successful submit; see <see cref="SendOperation" /> for the
    ///     ownership contract.
    /// </summary>
    /// <returns>The same builder, for chaining further parts, flags, or submit.</returns>
    public static ReplySubmitOperation Messages(
        this ReplyOperation operation,
        IReadOnlyList<Message> messages)
    {
        EnsureNotEmpty(messages);
        return operation.Message(messages[0]).Messages(messages, 1);
    }

    /// <summary>
    ///     Adds <paramref name="messages" /> as parts, in order. The parts are
    ///     consumed on a successful submit; see <see cref="SendOperation" /> for the
    ///     ownership contract.
    /// </summary>
    /// <returns>The same builder, for chaining further parts, flags, or submit.</returns>
    public static ReplySubmitOperation Messages(
        this ReplySubmitOperation operation,
        IReadOnlyList<Message> messages)
    {
        EnsureNotEmpty(messages);
        return operation.Messages(messages, 0);
    }

    // ActorJoin operation-builder extensions removed in RouteMesh 10.0.0
    // (the actor join fluent builders no longer exist).

    /// <summary>
    ///     Adds <paramref name="messages" /> as parts, in order. The parts are
    ///     consumed on a successful submit; see <see cref="SendOperation" /> for the
    ///     ownership contract.
    /// </summary>
    /// <returns>The same builder, for chaining further parts, flags, or submit.</returns>

    /// <summary>
    ///     Adds <paramref name="messages" /> as parts, in order. The parts are
    ///     consumed on a successful submit; see <see cref="SendOperation" /> for the
    ///     ownership contract.
    /// </summary>
    /// <returns>The same builder, for chaining further parts, flags, or submit.</returns>

    /// <summary>
    ///     Adds <paramref name="messages" /> as parts, in order. The parts are
    ///     consumed on a successful submit; see <see cref="SendOperation" /> for the
    ///     ownership contract.
    /// </summary>
    /// <returns>The same builder, for chaining further parts, flags, or submit.</returns>

    private static SendSubmitOperation Messages(
        this SendSubmitOperation operation,
        IReadOnlyList<Message> messages,
        int startIndex)
    {
        for (var index = startIndex; index < messages.Count; index++) operation = operation.Message(messages[index]);

        return operation;
    }

    private static PublishSubmitOperation Messages(
        this PublishSubmitOperation operation,
        IReadOnlyList<Message> messages,
        int startIndex)
    {
        for (var index = startIndex; index < messages.Count; index++)
            operation = operation.Message(messages[index]);

        return operation;
    }

    private static RoutedSendSubmitOperation Messages(
        this RoutedSendSubmitOperation operation,
        IReadOnlyList<Message> messages,
        int startIndex)
    {
        for (var index = startIndex; index < messages.Count; index++)
            operation = operation.Message(messages[index]);

        return operation;
    }

    private static RequestSubmitOperation Messages(
        this RequestSubmitOperation operation,
        IReadOnlyList<Message> messages,
        int startIndex)
    {
        for (var index = startIndex; index < messages.Count; index++) operation = operation.Message(messages[index]);

        return operation;
    }

    private static ReplySubmitOperation Messages(
        this ReplySubmitOperation operation,
        IReadOnlyList<Message> messages,
        int startIndex)
    {
        for (var index = startIndex; index < messages.Count; index++) operation = operation.Message(messages[index]);

        return operation;
    }



    private static void EnsureNotEmpty(IReadOnlyList<Message> messages)
    {
        ArgumentNullException.ThrowIfNull(messages);
        if (messages.Count == 0) throw new ArgumentException("At least one message is required.", nameof(messages));
    }
}
