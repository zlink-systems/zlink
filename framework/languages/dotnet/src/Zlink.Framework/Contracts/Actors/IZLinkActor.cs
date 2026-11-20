namespace Zlink.Framework.Contracts.Actors;

public interface IZLinkActor
{
    IZLinkActorContext Context { get; }

    ValueTask<string?> OnDuplicateBindAsync(
        ZLinkSessionBindingIdentity current,
        ZLinkSessionBindingIdentity incoming,
        CancellationToken cancellationToken
    ) => ValueTask.FromResult<string?>(null);

    /// <summary>Notifies the Actor after a Session binding commits, in Actor handler order.</summary>
    /// <remarks>
    /// The default implementation does nothing. Failure does not change the committed bind.
    /// The runtime cancels the callback on SessionReplacementCallbackTimeout or shutdown.
    /// Exceptions and timeout are reported once as runtime task exceptions; cancellation caused
    /// by Actor or runtime shutdown is not reported.
    /// </remarks>
    ValueTask OnSessionBoundAsync(
        ZLinkSessionBindingIdentity incoming,
        CancellationToken cancellationToken
    ) => ValueTask.CompletedTask;

    void Configure() { }

    ValueTask OnJoinCompletedAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken
    )
    {
        return ValueTask.CompletedTask;
    }
}
