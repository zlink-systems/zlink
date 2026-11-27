#if NETWORK_SYNC
using Microsoft.Extensions.Logging;
using MyGame.Match.Domain;
using MyGame.Shared;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.NetworkSync.Server;

namespace MyGame.Match;

public sealed class NetworkSyncRoom
    : NetworkSyncSpot<NetworkSyncRoom, PlayerActor, Vec3, Vec3, RoomView>
{
    private const int TickRate = 30;
    private readonly RoomState state;
    private readonly string nodeId;
    private long elapsedTicks;

    public NetworkSyncRoom(
        IZLinkSpotContext context, NodeSettings settings, ILogger<NetworkSyncRoom> logger
    ) : base(context, logger, tickRate: TickRate, wakeupBudget: TimeSpan.FromSeconds(0.5 / TickRate))
    {
        nodeId = settings.Id;
        state = new(() => elapsedTicks * 1000.0 / TickRate);
    }

    protected override ValueTask<ZLinkSpotActorJoinResult> AdmitPlayerAsync(
        string actorId, ZLinkMessage request, CancellationToken ct
    ) => ValueTask.FromResult(
        state.Admit(actorId, Context.SpotId, request.Decode<RoomAdmission>())
            ? ZLinkSpotActorJoinResult.Accept()
            : ZLinkSpotActorJoinResult.Reject()
    );

    protected override ValueTask OnPlayerJoinedAsync(PlayerActor actor, CancellationToken ct) =>
        actor.Context.BoundSession.Send(new MatchFound(Context.SpotId)).Async();

    protected override Vec3 GetMissingInput(string actorId) => default;

    protected override void Step(long epoch, long tick, NetworkSyncInputs<Vec3, Vec3> inputs)
    {
        elapsedTicks++;
        if (state.Phase != MatchPhase.Playing)
            return;

        for (var index = 0; index < inputs.Count; index++)
        {
            var input = inputs[index];
            var player = state.Player(input.ActorId);
            if (input.Input.Y == 0 && input.Input.TryNormalize(out var direction))
            {
                // Limit each held input to one tick of movement, allowing for rounding.
                state.Move(input.ActorId, new MoveRequest(direction * 0.25, player.Sequence + 1));
            }
            for (var action = 0; action < input.ActionCount; action++)
                state.Shoot(input.ActorId, new ShootRequest(input.GetAction(action).Payload, player.Sequence + 1));
        }
    }

    protected override RoomView CaptureSnapshot() => state.View(Context.SpotId, nodeId);
}
#endif
