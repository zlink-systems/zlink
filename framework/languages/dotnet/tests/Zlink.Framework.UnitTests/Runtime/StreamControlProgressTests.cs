using System.Diagnostics;
using System.Net.Sockets;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Tests.Common;

namespace Zlink.Framework.UnitTests;

public sealed class StreamControlProgressTests(Xunit.Abstractions.ITestOutputHelper output)
{
    [Fact]
    public async Task Four_hundred_sessions_with_three_UserSpots_resume_after_control_processing()
    {
        if (
            Environment.ProcessorCount != 2
            && await IsolatedTestProcess.RunIsolatedAsync(
                typeof(StreamControlProgressTests),
                nameof(Four_hundred_sessions_with_three_UserSpots_resume_after_control_processing),
                processorCount: 2
            )
        )
            return;
        await VerifyFourHundredConnectionsAsync(actors: true);
    }

    [Fact]
    public async Task Four_hundred_sessions_without_UserSpots_resume_after_control_processing()
    {
        if (
            Environment.ProcessorCount != 2
            && await IsolatedTestProcess.RunIsolatedAsync(
                typeof(StreamControlProgressTests),
                nameof(Four_hundred_sessions_without_UserSpots_resume_after_control_processing),
                processorCount: 2
            )
        )
            return;
        await VerifyFourHundredConnectionsAsync(actors: false);
    }

    private async Task VerifyFourHundredConnectionsAsync(bool actors)
    {
        var state = new ProgressState(400, actors);
        var name = actors ? "actors-three-userspots" : "session-handler";
        using var logs = CreateFlowLog(name);
        using var host = CreateHost(state);
        var connectors = new List<IZlinkStreamConnector>();
        var requests = Array.Empty<Task<Hold>>();
        try
        {
            await host.StartAsync();
            var runtime = host.Services.GetRequiredService<IZLinkFrameworkRuntime>();
            var endpoint = runtime
                .GetListenerStatus(ZLinkListenerKind.Stream, "control-progress")
                .Endpoint;
            Assert.Equal(2UL, runtime.Status.Capacity.ApplicationJobQueue.EffectiveProcessorCount);
            Assert.Equal(
                256UL,
                runtime.Status.Capacity.ApplicationJobQueue.EffectiveMaxQueuedApplicationJobs
            );
            var connected = await StreamConnectionHarness.ConnectAsync(
                connectors,
                400,
                new ZlinkStreamConnectorOptions
                {
                    Endpoint = new Uri(endpoint),
                    DispatchMode = ZlinkStreamDispatchMode.Immediate,
                    Reconnect = new() { Enabled = false },
                }
            );
            await state.AllConnected.Task.WaitAsync(TimeSpan.FromSeconds(30));
            if (actors)
            {
                foreach (var connector in connected)
                {
                    var reply = await connector.Request(new Login()).Async<Ready>();
                    Assert.Equal(3, reply.Spots);
                }
                Assert.Equal(1200, state.SpotsTouched);
            }
            requests = connected
                .Select(
                    (connector, index) => connector.Request(new Hold(index)).Async<Hold>().AsTask()
                )
                .ToArray();
            await state.AllEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            // Observe default periodic heartbeats while every application turn is held.
            // This is a fixture observation window, not a transport timeout or retry.
            var observation = Stopwatch.StartNew();
            while (observation.Elapsed < TimeSpan.FromSeconds(2))
            {
                Record(name, "held", runtime, state);
                await Task.Delay(20);
            }
            Record(name, "release", runtime, state);
            state.Release.TrySetResult();
            await Task.WhenAll(requests).WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(400, state.Completed);
            for (var index = 0; index < requests.Length; index++)
                Assert.Equal(index, requests[index].Result.Value);
            await WaitUntilAsync(() =>
                runtime.Status.Capacity.ApplicationJobQueue.PressureState
                != ZLinkApplicationJobQueuePressureState.Paused
            );
            Record(name, "completed", runtime, state);
            Assert.NotEqual(
                ZLinkApplicationJobQueuePressureState.Paused,
                runtime.Status.Capacity.ApplicationJobQueue.PressureState
            );
        }
        finally
        {
            state.Release.TrySetResult();
            foreach (var connector in connectors)
                await connector.DisposeAsync();
            // Observe request terminals during cleanup, including the expected fail-before.
            if (requests.Length != 0)
                await Task.WhenAll(
                    requests.Select(async request =>
                    {
                        try
                        {
                            await request;
                        }
                        catch (ZlinkStreamException) { }
                    })
                );
            await host.StopAsync();
        }
    }

    [Fact]
    public async Task Heartbeat_control_replies_and_returns_permit_before_application_handler_finishes()
    {
        var state = new ProgressState(1, false);
        using var logs = CreateFlowLog("control-contract");
        using var host = CreateHost(state);
        await host.StartAsync();
        var runtime = host.Services.GetRequiredService<IZLinkFrameworkRuntime>();
        var endpoint = new Uri(
            runtime.GetListenerStatus(ZLinkListenerKind.Stream, "control-progress").Endpoint
        );
        using var peer = new TcpClient();
        try
        {
            await peer.ConnectAsync(endpoint.Host, endpoint.Port);
            await state.AllConnected.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var application = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.None,
                null,
                nameof(Hold),
                ZlinkStreamMetadata.Empty
            );
            await WriteFrameAsync(
                peer,
                application,
                ZLinkStreamPacketPayloadCodec.EncodeJson(new Hold(1), typeof(Hold))
            );
            await state.AllEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(state.Release.Task.IsCompleted);
            await WaitUntilAsync(() =>
                runtime.Status.Capacity.ApplicationJobQueue.ReservedSupplyPermits == 0
            );
            var ping = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Control,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                null,
                "$zlink.heartbeat.ping",
                ZlinkStreamMetadata.Empty
            );
            await WriteFrameAsync(peer, ping, []);
            using var readDeadline = new CancellationTokenSource(TimeSpan.FromSeconds(2));
            ZlinkStreamHeader received;
            do
            {
                var prefix = new byte[6];
                await peer.GetStream().ReadExactlyAsync(prefix, readDeadline.Token);
                var headerLength = System.Buffers.Binary.BinaryPrimitives.ReadUInt16BigEndian(
                    prefix
                );
                var payloadLength = System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(
                    prefix.AsSpan(2)
                );
                var bytes = new byte[checked(headerLength + (int)payloadLength)];
                await peer.GetStream().ReadExactlyAsync(bytes, readDeadline.Token);
                received = ZLinkStreamProtocolDefaults.DecodeHeader(
                    bytes.AsMemory(0, headerLength)
                );
            } while (received.Name == "$zlink.heartbeat.ping");
            Assert.Equal("$zlink.heartbeat.pong", received.Name);
            await WaitUntilAsync(() =>
                runtime.Status.Capacity.ApplicationJobQueue.ReservedSupplyPermits == 0
            );
            Assert.Equal(0, state.Completed);
            Assert.False(state.Release.Task.IsCompleted);
            Record("control-contract", "control-completed-before-handler", runtime, state);
        }
        finally
        {
            Record("control-contract", "handler-still-held", runtime, state);
            state.Release.TrySetResult();
            peer.Close();
            await host.StopAsync();
        }
    }

    private static async Task WriteFrameAsync(
        TcpClient peer,
        ZlinkStreamHeader header,
        byte[] payload
    )
    {
        var frame = ZLinkStreamFrameCodec.Encode(
            ZLinkStreamProtocolDefaults.EncodeHeader(header).Span,
            payload
        );
        await peer.GetStream().WriteAsync(frame);
    }

    private static async Task WaitUntilAsync(Func<bool> condition)
    {
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        while (!condition())
            await Task.Delay(10, deadline.Token);
    }

    private static TestHostMessageFlowListener CreateFlowLog(string name)
    {
        var directory =
            Environment.GetEnvironmentVariable("ZLINK_STREAM_CONTROL_TEST_LOG_DIR")
            ?? FrameworkTestEnvironment.CreateTestLogDirectory("stream-control-progress");
        return new TestHostMessageFlowListener(Path.Combine(directory, name + ".flow"));
    }

    private void Record(
        string name,
        string phase,
        IZLinkFrameworkRuntime runtime,
        ProgressState state
    )
    {
        var line = JsonSerializer.Serialize(
            new
            {
                timestamp = DateTimeOffset.UtcNow,
                phase,
                capacity = runtime.Status.Capacity.ApplicationJobQueue,
                entered = state.Entered,
                completed = state.Completed,
                spotsTouched = state.SpotsTouched,
            }
        );
        output.WriteLine(line);
        if (
            Environment.GetEnvironmentVariable("ZLINK_STREAM_CONTROL_TEST_LOG_DIR") is { } directory
        )
            File.AppendAllText(
                Path.Combine(directory, name + ".jsonl"),
                line + Environment.NewLine
            );
    }

    private static IHost CreateHost(ProgressState state)
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Logging.SetMinimumLevel(LogLevel.Warning);
        builder.Services.AddSingleton(state);
        builder.Services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Detailed);
            var echo = options.AddClientServerChannel("control-echo");
            echo.Client();
            echo.Server().Listen().AddRequestHandler<EchoHandler, Hold, Hold>();
            var stream = options.AddStreamNode("control-progress").Bind("tcp://127.0.0.1:0");
            if (state.Actors)
            {
                options.AddLocationStore(new ZLinkInMemoryProviderLocationStore());
                var objects = options
                    .AddRouteMesh("control-objects")
                    .Listen("tcp://127.0.0.1:0")
                    .Objects()
                    .Server();
                objects.AddEntrySpot<ProgressEntry>();
                objects.AddActorFactory<ProgressActor, ProgressActorFactory>(
                    "control-actor",
                    factory => factory.DisableRelocation()
                );
                objects.AddSpotFactory<ProgressUserSpot>(
                    "control-user",
                    factory =>
                        factory
                            .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                            .DisableRelocation()
                );
                stream.EnableActorDispatch().AddSession<ProgressSession>();
            }
            else
                stream.AddSession<HoldSession>();
        });
        return builder.Build();
    }

    private sealed record Login;

    private sealed record Ready(int Spots);

    private sealed record Hold(int Value);

    private sealed record OpenUser;

    private sealed record TouchUser;

    private sealed record TouchedUser(int Value);

    private sealed class ProgressState(int count, bool actors)
    {
        public bool Actors => actors;
        public int Connected,
            Entered,
            Completed,
            SpotsTouched;
        public TaskCompletionSource AllConnected { get; } = NewSignal();
        public TaskCompletionSource AllEntered { get; } = NewSignal();
        public TaskCompletionSource Release { get; } = NewSignal();

        public void OnConnected()
        {
            if (Interlocked.Increment(ref Connected) == count)
                AllConnected.TrySetResult();
        }

        public async Task<Hold> HoldAsync(
            IZLinkRouteClient route,
            Hold request,
            CancellationToken cancellationToken
        )
        {
            if (Interlocked.Increment(ref Entered) == count)
                AllEntered.TrySetResult();
            await Release.Task.WaitAsync(cancellationToken);
            var reply = await route
                .RequestToChannel("control-echo", request)
                .Async<Hold>(cancellationToken);
            Interlocked.Increment(ref Completed);
            return reply;
        }

        private static TaskCompletionSource NewSignal() =>
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed class EchoHandler : IZLinkRequestHandler<Hold, Hold>
    {
        public ValueTask<Hold> HandleAsync(
            Hold request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(request);
    }

    private sealed class ProgressSession(
        IZLinkSessionContext context,
        ProgressState state,
        IZLinkActorManager actors,
        IZLinkSpotManager spots,
        IZLinkSpotClient spotClient
    ) : IZLinkSession
    {
        public IZLinkSessionContext Context => context;

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
        {
            state.OnConnected();
            return ValueTask.CompletedTask;
        }

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public async ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken
        )
        {
            if (dispatch.PacketName == nameof(Login))
            {
                var result = await actors
                    .GetOrCreate(Guid.NewGuid().ToString("N"), "control-actor")
                    .InMesh("control-objects")
                    .Async(cancellationToken);
                var actor = result switch
                {
                    ZLinkActorCreateResult.Created created => created.Actor,
                    ZLinkActorCreateResult.Existing existing => existing.Actor,
                    _ => throw new InvalidOperationException("Actor creation rejected."),
                };
                await Context.Actors.BindAsync(actor, cancellationToken);
                for (var index = 0; index < 3; index++)
                {
                    var id = $"control-user:{index}:{actor.ActorId}";
                    var opened = await spots
                        .GetOrCreate(id, "control-user")
                        .InMesh("control-objects")
                        .Request(new OpenUser())
                        .Async(cancellationToken);
                    Assert.NotEqual(ZLinkSpotCreateState.Rejected, opened.State);
                    var touched = await spotClient
                        .RequestToSpot(id, new TouchUser())
                        .Async<TouchedUser>(cancellationToken);
                    Assert.Equal(1, touched.Value);
                    Interlocked.Increment(ref state.SpotsTouched);
                }
                await Context.Client.Reply(new Ready(3)).Async();
            }
            else
                await Context.Actors.Bound.Single().RelayAsync(payload);
        }
    }

    private sealed class HoldSession(
        IZLinkSessionContext context,
        ProgressState state,
        IZLinkRouteClient route
    ) : IZLinkSession
    {
        public IZLinkSessionContext Context => context;

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
        {
            state.OnConnected();
            return ValueTask.CompletedTask;
        }

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public async ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken
        )
        {
            var reply = await state.HoldAsync(route, payload.Decode<Hold>(), cancellationToken);
            if (dispatch.CanReply)
                await Context.Client.Reply(reply).Async();
        }
    }

    private sealed class ProgressActor(IZLinkActorContext context) : IZLinkActor
    {
        public IZLinkActorContext Context => context;
    }

    private sealed class ProgressActorFactory : IZLinkActorFactory<ProgressActor>
    {
        public ValueTask<ProgressActor> CreateAsync(
            IZLinkActorContext context,
            CancellationToken cancellationToken = default
        ) => ValueTask.FromResult(new ProgressActor(context));

        async ValueTask<IZLinkActor> IZLinkActorFactory.CreateAsync(
            IZLinkActorContext context,
            CancellationToken cancellationToken
        ) => await CreateAsync(context, cancellationToken);
    }

    private sealed class ProgressEntry(IZLinkEntrySpotContext context)
        : IZLinkEntrySpot<ProgressActor>
    {
        public IZLinkEntrySpotContext Context => context;

        public void Configure() => Context.Handlers.AddActorPacket<ActorHold, ProgressActor>();

        public ValueTask OnJoinedActorAsync(
            ProgressActor actor,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(
            ProgressActor actor,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;
    }

    private sealed class ActorHold(IZLinkRouteClient route, ProgressState state)
        : IZLinkEntrySpotActorRequestHandler<ProgressEntry, ProgressActor, Hold, Hold>
    {
        public async ValueTask<Hold> HandleAsync(
            ProgressEntry spot,
            ProgressActor actor,
            IZLinkMessageContext context,
            Hold message,
            CancellationToken cancellationToken
        ) => await state.HoldAsync(route, message, cancellationToken);
    }

    private sealed class ProgressUserSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context => context;
        public int Touches;

        public void Configure() => Context.Handlers.AddPacket<TouchHandler>();
    }

    private sealed class TouchHandler
        : IZLinkSpotRequestHandler<ProgressUserSpot, TouchUser, TouchedUser>
    {
        public ValueTask<TouchedUser> HandleAsync(
            ProgressUserSpot spot,
            IZLinkMessageContext context,
            TouchUser request,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(new TouchedUser(++spot.Touches));
    }
}
