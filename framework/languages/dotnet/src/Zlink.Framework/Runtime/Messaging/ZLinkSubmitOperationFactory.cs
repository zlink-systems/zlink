namespace Zlink.Framework.Runtime.Messaging;

internal sealed class ZLinkSubmitOperationFactory(
    TimeSpan? sendTimeout,
    object admissionGate,
    Action<PendingSubmit> wake)
{
    public DateTimeOffset? ResolveDeadline()
    {
        return sendTimeout is null
            ? null
            : DateTimeOffset.UtcNow.Add(sendTimeout.Value);
    }

    public PendingSubmit CreateCommand(
        IReadOnlyList<Message> parts,
        Func<IReadOnlyList<Message>, bool> trySubmit,
        string operationId)
    {
        return PendingSubmit.CreateCommand(
            parts,
            trySubmit,
            ResolveDeadlineOrThrow(parts),
            admissionGate,
            wake,
            operationId);
    }

    public PendingSubmit CreateRequest<T>(
        IReadOnlyList<Message> parts,
        Func<IReadOnlyList<Message>, bool> trySubmit,
        ZLinkRequestCompletion<T> completion,
        string operationId,
        DateTimeOffset? deadline = null)
    {
        return PendingSubmit.CreateRequest(
            parts,
            trySubmit,
            ResolveDeadlineOrThrow(parts, deadline),
            admissionGate,
            wake,
            completion,
            operationId);
    }

    private DateTimeOffset? ResolveDeadlineOrThrow(
        IReadOnlyList<Message> parts,
        DateTimeOffset? deadline = null)
    {
        var resolvedDeadline = deadline ?? ResolveDeadline();
        if (resolvedDeadline is DateTimeOffset value && value <= DateTimeOffset.UtcNow)
        {
            ZLinkMessageParts.DisposeAll(parts);
            throw new TimeoutException("ZLink async submit timed out before the socket became writable.");
        }

        return resolvedDeadline;
    }
}
