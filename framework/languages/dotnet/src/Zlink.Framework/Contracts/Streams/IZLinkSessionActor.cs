namespace Zlink.Framework.Contracts.Streams;

public interface IZLinkSessionActor
{
    string ActorId => Ref.ActorId;

    ActorRef Ref { get; }

    ValueTask RelayAsync(ZLinkMessage payload);

    ValueTask NotifyDisconnectedAsync(CancellationToken cancellationToken = default);
}
