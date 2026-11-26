using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class MessageFlowFailureTests
{
    [Fact]
    public async Task Actor_send_reports_the_serializer_failure_without_sent()
    {
        var delivered = new TaskCompletionSource<string>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var actorId = $"encode-failure-{Guid.NewGuid():N}";
        using var flow = new FlowObservation("actor_id", actorId, nameof(Snapshot));
        using var stderr = new StderrObservation();
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddSingleton(stderr.Factory);
        builder.Services.AddSingleton(delivered);
        builder.Services.AddSingleton<SpotCloseProbeState>();
        builder.Services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.AddLocationStore(new ZLinkInMemoryProviderLocationStore());
            options
                .AddRouteMesh("snapshot-mesh")
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server()
                .AddEntrySpot<SnapshotEntrySpot>()
                .AddActorFactory<SpotCloseActor, SpotCloseActorFactory>(
                    SpotCloseHost.ActorType,
                    factory => factory.DisableRelocation()
                );
        });
        using var host = builder.Build();
        await host.StartAsync();
        try
        {
            _ = Assert.IsType<ZLinkActorCreateResult.Created>(
                await host
                    .Services.GetRequiredService<IZLinkActorManager>()
                    .GetOrCreate(actorId, SpotCloseHost.ActorType)
                    .InMesh("snapshot-mesh")
                    .Request(ZLinkMessage.Empty)
                    .Async()
            );

            var client = host.Services.GetRequiredService<IZLinkActorClient>();
            var error = await Assert.ThrowsAsync<JsonException>(() =>
                client.SendToActor(actorId, new Snapshot(null!)).Async().AsTask()
            );
            Assert.Contains(nameof(Snapshot), error.Message);
            Assert.Contains("winner", error.Message);
            Assert.Contains("nullable", error.Message);
            MessageFlowDeliveryTests.WaitForDelivery();
            Assert.DoesNotContain(flow.Events, activity => Tag(activity, "phase") == "sent");
            var failed = Assert.Single(
                flow.Events,
                activity => Tag(activity, "outcome") == "failed"
            );
            AssertFailure(failed, "encode_error", "fail_caller");
            Assert.Equal(error.Message, Tag(failed, "error_message"));

            await client.SendToActor(actorId, new Snapshot("valid")).Async();
            Assert.Equal("valid", await delivered.Task.WaitAsync(TimeSpan.FromSeconds(10)));
            MessageFlowDeliveryTests.WaitForDelivery();
            Assert.Single(flow.Events, activity => Tag(activity, "phase") == "sent");
            Assert.Single(flow.Events, activity => Tag(activity, "outcome") == "failed");
        }
        finally
        {
            await host.StopAsync();
        }
        stderr.AssertFailure("encode_error", "fail_caller", "winner");
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Spot_send_traces_start_and_async_failure_on_the_received_flow(bool remote)
    {
        var probe = new HandlerProbe();
        var spotId = $"handler-failure-{Guid.NewGuid():N}";
        using var flow = new FlowObservation("spot_id", spotId, nameof(ProbePacket));
        const string spotType = "message-flow-failure";
        using var stderr = new StderrObservation();
        {
            await using var owner = await SpotCloseHost.StartAsync(
                registerInstanceFactory: false,
                registerUserSpotFactory: false,
                configureServer: (services, server) =>
                {
                    services.AddSingleton(probe);
                    services.AddSingleton(stderr.Factory);
                    server.AddSpotFactory<ProbeSpot>(
                        spotType,
                        factory => factory.DisableRelocation()
                    );
                }
            );
            using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            await owner
                .Manager.GetOrCreate(spotId, spotType)
                .InMesh(SpotCloseHost.MeshName)
                .Async();
            await using var peer = remote
                ? await SpotCloseHost.StartAsync(owner.Store, clientOnly: true)
                : null;
            if (peer is not null)
                await peer.WaitForPeerAsync(owner, cancellation.Token);
            var client = (peer ?? owner).Runtime.Services.GetRequiredService<IZLinkSpotClient>();

            await client.SendToSpot(spotId, new ProbePacket(true)).Async();
            try
            {
                await probe.Entered.Task.WaitAsync(cancellation.Token);
                MessageFlowDeliveryTests.WaitForDelivery();
                var received = Assert.Single(
                    flow.Events,
                    activity => Tag(activity, "phase") == "received"
                );
                var flowId = Assert.IsType<string>(received.GetTagItem("flow_id"));
                Assert.NotEmpty(flowId);
                var dispatched = Assert.Single(
                    flow.Events,
                    activity => Tag(activity, "phase") == "dispatched"
                );
                Assert.Equal(flowId, Tag(dispatched, "flow_id"));
                Assert.DoesNotContain(
                    flow.Events,
                    activity => Tag(activity, "outcome") == "failed"
                );

                probe.Release.SetResult();
                var failed = await flow.Failure.Task.WaitAsync(cancellation.Token);
                AssertFailure(failed, "handler_exception", "drop");
                Assert.Equal(flowId, Tag(failed, "flow_id"));
                Assert.Equal(Tag(received, "flow_origin"), Tag(failed, "flow_origin"));
                Assert.Equal(Tag(received, "correlation_id"), Tag(failed, "correlation_id"));
                Assert.Equal("async handler failure", Tag(failed, "error_message"));

                await client.SendToSpot(spotId, new ProbePacket(false)).Async();
                await probe.Succeeded.Task.WaitAsync(cancellation.Token);
                MessageFlowDeliveryTests.WaitForDelivery();
                Assert.Equal(
                    2,
                    flow.Events.Count(activity => Tag(activity, "phase") == "dispatched")
                );
                Assert.Single(flow.Events, activity => Tag(activity, "outcome") == "failed");
            }
            finally
            {
                probe.Release.TrySetResult();
            }
        }
        stderr.AssertFailure("handler_exception", "drop", "async handler failure");
    }

    private sealed class StderrObservation : IDisposable
    {
        private readonly TextWriter _original = Console.Error;
        private readonly StringWriter _output = new();

        public StderrObservation()
        {
            Console.SetError(_output);
            Factory = LoggerFactory.Create(logging =>
                logging.AddConsole(options => options.LogToStandardErrorThreshold = LogLevel.Trace)
            );
        }

        public ILoggerFactory Factory { get; }

        public void AssertFailure(string reason, string action, string cause)
        {
            Factory.Dispose();
            var failure = Assert.Single(
                _output.ToString().Split('\n'),
                line => line.Contains("zlink.dispatch_error") && line.Contains($"reason={reason}")
            );
            Assert.Contains("outcome=failed", failure);
            Assert.Contains($"action={action}", failure);
            Assert.Contains(cause, failure);
        }

        public void Dispose()
        {
            Factory.Dispose();
            Console.SetError(_original);
            _output.Dispose();
        }
    }

    private static string? Tag(Activity activity, string name) =>
        activity.GetTagItem(name) as string;

    private static void AssertFailure(Activity activity, string reason, string action)
    {
        Assert.Equal("zlink.dispatch_error", activity.OperationName);
        Assert.Equal("failed", Tag(activity, "outcome"));
        Assert.Equal(reason, Tag(activity, "reason"));
        Assert.Equal(action, Tag(activity, "action"));
    }

    private sealed class FlowObservation : IDisposable
    {
        private readonly ActivityListener _listener;

        public FlowObservation(string idTag, string id, string packet)
        {
            _listener = new ActivityListener
            {
                ShouldListenTo = source => source.Name == "Zlink.Framework",
                Sample = (ref ActivityCreationOptions<ActivityContext> _) =>
                    ActivitySamplingResult.AllData,
                ActivityStopped = activity =>
                {
                    if (Tag(activity, idTag) != id || Tag(activity, "packet_name") != packet)
                        return;
                    Events.Enqueue(activity);
                    if (activity.OperationName == "zlink.dispatch_error")
                        Failure.TrySetResult(activity);
                },
            };
            ActivitySource.AddActivityListener(_listener);
        }

        public ConcurrentQueue<Activity> Events { get; } = new();
        public TaskCompletionSource<Activity> Failure { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public void Dispose() => _listener.Dispose();
    }

    public sealed record Snapshot(string Winner);

    internal sealed class SnapshotEntrySpot(IZLinkEntrySpotContext context)
        : IZLinkEntrySpot<SpotCloseActor>
    {
        public IZLinkEntrySpotContext Context { get; } = context;

        public void Configure() =>
            Context.Handlers.AddActorPacket<SnapshotHandler, SpotCloseActor>(nameof(Snapshot));

        public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
            SpotCloseActor actor,
            ZLinkMessage request,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(ZLinkActorCreateResponse.Accept());

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());

        public ValueTask OnJoinedActorAsync(
            SpotCloseActor actor,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(
            SpotCloseActor actor,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;
    }

    internal sealed class SnapshotHandler(TaskCompletionSource<string> delivered)
        : IZLinkEntrySpotActorSendHandler<SnapshotEntrySpot, SpotCloseActor, Snapshot>
    {
        public ValueTask HandleAsync(
            SnapshotEntrySpot spot,
            SpotCloseActor actor,
            IZLinkMessageContext context,
            Snapshot message,
            CancellationToken cancellationToken
        )
        {
            delivered.SetResult(message.Winner);
            return ValueTask.CompletedTask;
        }
    }

    public sealed record ProbePacket(bool Fail);

    public sealed class HandlerProbe
    {
        public TaskCompletionSource Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Succeeded { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    public sealed class ProbeSpot(IZLinkSpotContext context) : IZLinkSpot<IZLinkActor>
    {
        public IZLinkSpotContext Context { get; } = context;

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());

        public ValueTask OnJoinedActorAsync(
            IZLinkActor actor,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(
            IZLinkActor actor,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;
    }

    public sealed class ProbeHandler(HandlerProbe probe)
        : IZLinkSpotPacketHandler<ProbeSpot, ProbePacket>
    {
        public async ValueTask HandleAsync(
            ProbeSpot spot,
            IZLinkMessageContext context,
            ProbePacket message,
            CancellationToken cancellationToken
        )
        {
            if (message.Fail)
            {
                probe.Entered.SetResult();
                await probe.Release.Task.WaitAsync(cancellationToken);
                throw new InvalidOperationException("async handler failure");
            }

            probe.Succeeded.SetResult();
        }
    }
}
