using System.Collections.Concurrent;
using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class SpotActorDispatchErrorReasonTests
{
    [Theory]
    [InlineData(true, "handler_exception")]
    [InlineData(false, "no_handler")]
    public async Task Spot_actor_request_reason_identifies_the_failure_stage(
        bool registered,
        string expectedReason
    )
    {
        var trace = new RelocationBehaviorTrace
        {
            ConfigureTarget = context =>
                context.Handlers.AddActorPacket<ThrowingHandler, BehaviorActor>(
                    nameof(ThrowNotFound)
                ),
        };
        trace.ReleaseJoinHandler.TrySetResult();
        trace.ReleaseSourceLeave.TrySetResult();
        trace.ReleaseTargetLifecycle.TrySetResult();
        await using var host = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            new ZLinkInMemoryProviderLocationStore(),
            new SynchronizedRelocationStore(),
            true,
            configureServices: services => services.AddTransient<ThrowingHandler>()
        );
        host.Runtime.Registration.DispatchOptions.Diagnostics.SetLevel(
            ZLinkDiagnosticsLevel.Normal
        );

        var actorId = $"dispatch-reason-{Guid.NewGuid():N}";
        var spotId = $"dispatch-spot-{Guid.NewGuid():N}";
        _ = await host
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(spotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Async();
        _ = await host
            .Services.GetRequiredService<IZLinkActorManager>()
            .Create(actorId, RelocationBehaviorHost.ActorType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Request(new BehaviorCreate(7))
            .Async();
        var client = host.Services.GetRequiredService<IZLinkActorClient>();
        _ = await client
            .RequestToActor(actorId, new BeginBehaviorJoin(spotId))
            .Async<BehaviorAck>();
        await trace.WaitAsync("publicJoinCompleted");

        var packet = registered ? nameof(ThrowNotFound) : nameof(MissingPacket);
        var reasons = new ConcurrentQueue<string?>();
        using var listener = new ActivityListener
        {
            ShouldListenTo = static source => source.Name == "Zlink.Framework",
            Sample = static (ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllData,
            ActivityStopped = activity =>
            {
                if (
                    activity.OperationName == "zlink.dispatch_error"
                    && Equals(activity.GetTagItem("packet_name"), packet)
                    && Equals(activity.GetTagItem("surface"), "actor")
                    && Equals(activity.GetTagItem("actor_id"), actorId)
                )
                    reasons.Enqueue(activity.GetTagItem("reason") as string);
            },
        };
        ActivitySource.AddActivityListener(listener);

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
        {
            if (registered)
                await client.RequestToActor(actorId, new ThrowNotFound()).Async<BehaviorAck>();
            else
                await client.RequestToActor(actorId, new MissingPacket()).Async<BehaviorAck>();
        });
        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
        TelemetryReceipt.Wait(() => !reasons.IsEmpty);
        Assert.Equal(expectedReason, Assert.Single(reasons));
    }

    private sealed record ThrowNotFound;

    private sealed record MissingPacket;

    private sealed class ThrowingHandler
        : IZLinkSpotActorRequestHandler<
            BehaviorTargetSpot,
            BehaviorActor,
            ThrowNotFound,
            BehaviorAck
        >
    {
        public ValueTask<BehaviorAck> HandleAsync(
            BehaviorTargetSpot spot,
            BehaviorActor actor,
            IZLinkMessageContext context,
            ThrowNotFound request,
            CancellationToken cancellationToken
        ) =>
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                "Application lookup failed."
            );
    }
}
