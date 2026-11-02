namespace Zlink.Framework.Contracts.Actors;

public interface IZLinkActor
{
    IZLinkActorContext Context { get; }

    ValueTask<string?> OnDuplicateBindAsync(
        ZLinkSessionBindingIdentity current,
        ZLinkSessionBindingIdentity incoming,
        CancellationToken cancellationToken
    ) => ValueTask.FromResult<string?>(null);

    void Configure() { }

    ValueTask OnJoinCompletedAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken
    )
    {
        return ValueTask.CompletedTask;
    }
}
