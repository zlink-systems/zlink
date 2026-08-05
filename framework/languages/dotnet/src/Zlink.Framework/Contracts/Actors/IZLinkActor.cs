namespace Zlink.Framework.Contracts.Actors;

public interface IZLinkActor
{
    IZLinkActorContext Context { get; }

    void Configure()
    {
    }

    ValueTask OnJoinCompletedAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken)
    {
        return ValueTask.CompletedTask;
    }
}
