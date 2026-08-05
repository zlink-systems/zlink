namespace Zlink.Framework.Runtime.Messaging;

/// <summary>
/// Owns the terminal race between a native reply callback, caller cancellation,
/// and an optional caller-side timeout. A successful callback transfers reply
/// ownership to the awaiter; every callback that loses the race disposes its reply.
/// </summary>
internal sealed class ZLinkNativeReplyCompletion<TResult> : IDisposable
{
    private readonly ZLinkRequestCompletion<(TResult Result, IReadOnlyList<Message> Reply)> _completion;

    internal ZLinkNativeReplyCompletion(CancellationToken cancellationToken)
    {
        _completion = new ZLinkRequestCompletion<(TResult, IReadOnlyList<Message>)>(
            cancellationToken,
            discardResult: static result => ZLinkMessageParts.DisposeAll(result.Item2));
    }

    internal ZLinkNativeReplyCompletion(
        CancellationToken cancellationToken,
        TimeSpan timeout,
        string timeoutMessage)
    {
        _completion = new ZLinkRequestCompletion<(TResult, IReadOnlyList<Message>)>(
            cancellationToken,
            timeout,
            timeoutMessage,
            static result => ZLinkMessageParts.DisposeAll(result.Item2));
    }

    internal Task<(TResult Result, IReadOnlyList<Message> Reply)> Task => _completion.Task;

    internal void Complete(TResult result, IReadOnlyList<Message> reply)
    {
        _completion.Complete((result, reply));
    }

    internal void Fail(Exception exception, IReadOnlyList<Message> reply)
    {
        ZLinkMessageParts.DisposeAll(reply);
        _completion.Fail(exception);
    }

    public void Dispose()
    {
        _completion.Dispose();
    }
}
