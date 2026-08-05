namespace Zlink.Framework.Contracts.Streams;

public interface IZLinkSessionActor
{
    string ActorId => Ref.ActorId;

    ActorRef Ref { get; }

    ValueTask RelayAsync(
        ZLinkMessage payload,
        CancellationToken cancellationToken = default);

    ValueTask NotifyDisconnectedAsync(
        CancellationToken cancellationToken = default);
}
