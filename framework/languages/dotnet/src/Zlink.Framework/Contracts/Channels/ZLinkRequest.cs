using System.Runtime.CompilerServices;

namespace Zlink.Framework.Contracts.Channels;

/// <summary>Observes source-local admission and the application reply of one request.</summary>
public readonly struct ZLinkRequest<TReply>
{
    internal ZLinkRequest(ValueTask admitted, ValueTask<TReply> reply)
    {
        Admitted = admitted;
        Reply = reply;
    }

    public ValueTask Admitted { get; }
    public ValueTask<TReply> Reply { get; }

    public ValueTaskAwaiter<TReply> GetAwaiter() => Reply.GetAwaiter();

    public ConfiguredValueTaskAwaitable<TReply> ConfigureAwait(bool continueOnCapturedContext) =>
        Reply.ConfigureAwait(continueOnCapturedContext);

    public Task<TReply> AsTask() => Reply.AsTask();

    public static implicit operator ValueTask<TReply>(ZLinkRequest<TReply> request) =>
        request.Reply;
}
