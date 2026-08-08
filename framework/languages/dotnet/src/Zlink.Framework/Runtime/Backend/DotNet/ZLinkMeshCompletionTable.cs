using Zlink.Framework.Runtime.Backend.Contracts;

namespace Zlink.Framework.Runtime.Backend.DotNet;

// Framework allocates the reply correlation and registers its waiter before it
// submits the request. The dispatch pump therefore only resolves known pending
// correlations; it never retains a reply that arrived before registration.
internal sealed class ZLinkMeshCompletionTable
{
    internal delegate void CompletionHandler(
        MeshReceiveRecord record, IReadOnlyList<Message> parts);

    private readonly object _gate = new();
    private readonly Dictionary<MeshOperationId, CompletionHandler> _pending = new();

    public bool Register(MeshOperationId correlationId, CompletionHandler handler)
    {
        if (correlationId == default) return false;
        ArgumentNullException.ThrowIfNull(handler);
        lock (_gate)
        {
            if (!_pending.TryAdd(correlationId, handler))
                throw new InvalidOperationException(
                    "The reply correlation already has a waiter.");
        }
        return true;
    }

    public bool RegisterRequest(
        MeshOperationId correlationId,
        RequestCallback callback)
    {
        ArgumentNullException.ThrowIfNull(callback);
        return Register(correlationId, (record, parts) =>
            callback(MapResult(record.TerminalResult, record.FailureErrno), parts));
    }

    // This is the only request submission entry point for the pull-dispatch
    // bridge. A synchronous rejection cannot produce a completion, so its
    // waiter is removed before the result is returned.
    public SubmitResult RegisterBeforeSubmit(
        MeshOperationId correlationId,
        CompletionHandler handler,
        Func<MeshOperationId, SubmitResult> submit)
    {
        ArgumentNullException.ThrowIfNull(handler);
        ArgumentNullException.ThrowIfNull(submit);
        if (!Register(correlationId, handler))
            throw new ArgumentException(
                "A non-default reply correlation is required.",
                nameof(correlationId));

        try
        {
            var result = submit(correlationId);
            if (result != SubmitResult.Ok)
                Unregister(correlationId);
            return result;
        }
        catch
        {
            Unregister(correlationId);
            throw;
        }
    }

    public SubmitResult RegisterRequestBeforeSubmit(
        MeshOperationId correlationId,
        RequestCallback callback,
        Func<MeshOperationId, SubmitResult> submit)
    {
        ArgumentNullException.ThrowIfNull(callback);
        return RegisterBeforeSubmit(
            correlationId,
            (record, parts) => callback(
                MapResult(record.TerminalResult, record.FailureErrno),
                parts),
            submit);
    }

    private void Unregister(MeshOperationId correlationId)
    {
        lock (_gate)
            _pending.Remove(correlationId);
    }

    public void Complete(
        MeshReceiveRecord record,
        IReadOnlyList<Message> parts)
    {
        CompletionHandler? handler;
        lock (_gate)
            _pending.Remove(record.OperationId, out handler);
        if (handler is null)
        {
            // A terminal after cancellation, timeout, or shutdown no longer has
            // an owner. It must not be attached to a later request.
            ZLinkMessageParts.DisposeAll(parts);
            return;
        }
        handler(record, parts);
    }

    public void FailAll(RequestResult result)
    {
        KeyValuePair<MeshOperationId, CompletionHandler>[] pending;
        lock (_gate)
        {
            pending = _pending.ToArray();
            _pending.Clear();
        }
        foreach (var entry in pending)
            entry.Value(
                MeshReceiveRecord.CompletionFailure(entry.Key, result),
                Array.Empty<Message>());
    }

    public static RequestResult MapResult(int terminalResult, int failureErrno)
    {
        _ = failureErrno;
        if (terminalResult == 0) return RequestResult.Ok;
        var result = (RequestResult)terminalResult;
        return Enum.IsDefined(result) ? result : RequestResult.InternalError;
    }
}
