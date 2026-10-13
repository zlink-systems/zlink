using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;

namespace MyGame.Session;

public sealed class PlayerSession(IZLinkSessionContext context, IZLinkActorManager actors)
    : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken)
    {
        Console.Error.WriteLine($"Session error: {error.Error}: {error.Message}");
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken
    )
    {
        if (dispatch.PacketName == nameof(SessionInfoRequest))
        {
            await Context.Client.Reply(new SessionInfo(Context.SessionId)).Async();
            return;
        }

        if (Context.Actors.Bound.Count == 0)
        {
            if (dispatch.PacketName != nameof(JoinWorldRequest))
                throw new InvalidOperationException(
                    "JoinWorldRequest must be the first player packet."
                );

            var join = payload.Decode<JoinWorldRequest>();
            var result = await actors
                .GetOrCreate(join.PlayerId, GameNames.PlayerType)
                .InMesh(GameNames.WorldMesh)
                .Request(ZLinkMessage.Empty)
                .Async(cancellationToken);
            var actor = result switch
            {
                ZLinkActorCreateResult.Created created => created.Actor,
                ZLinkActorCreateResult.Existing existing => existing.Actor,
                _ => throw new InvalidOperationException("Player creation was rejected."),
            };
            await Context.Actors.BindOrGetAsync(actor, cancellationToken);
        }

        // Relaying the original packet establishes the authoritative actor's session route.
        await Context.Actors.Bound.Single().RelayAsync(payload);
    }
}
