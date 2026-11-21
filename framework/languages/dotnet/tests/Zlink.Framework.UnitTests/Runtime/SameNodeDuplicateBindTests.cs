using System.Text.Json;
using System.Threading.Channels;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink.Stream.Connector.Contracts;
using Xunit;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.Runtime.Locations;
using ZLinkSessionBindingIdentity = Zlink.Framework.Contracts.Actors.ZLinkSessionBindingIdentity;

namespace Zlink.Framework.UnitTests.SameNodeDuplicateBind;

public sealed class SameNodeDuplicateBindTests
{
    public static IEnumerable<object[]> Cases()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null)
        {
            var path = Path.Combine(
                directory.FullName,
                "framework/runtime/conformance/bound-session-relocation-v1.json"
            );
            if (File.Exists(path))
            {
                using var fixture = JsonDocument.Parse(File.ReadAllText(path));
                foreach (
                    var scenario in fixture
                        .RootElement.GetProperty("sameNodeDuplicateBind")
                        .EnumerateArray()
                )
                    yield return
                    [
                        scenario.GetProperty("rejectionMessage").GetString()!,
                        scenario.GetProperty("accepted").GetBoolean(),
                        scenario.GetProperty("callbackCount").GetInt32(),
                        scenario.GetProperty("replacementCount").GetInt32(),
                    ];
                yield break;
            }
            directory = directory.Parent;
        }
        throw new FileNotFoundException("Shared binding fixture was not found.");
    }

    [Theory]
    [MemberData(nameof(Cases))]
    public async Task Same_node_different_session_uses_the_Actor_owner_decision(
        string rejection,
        bool accepted,
        int callbacks,
        int replacements
    )
    {
        var state = new BindingState { Rejection = rejection };
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddSingleton(state);
        builder.Services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.AddLocationStore(new ZLinkInMemoryProviderLocationStore());
            var objects = options
                .AddRouteMesh("binding")
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server();
            objects.AddEntrySpot<BindingEntry>();
            objects.AddActorFactory<BindingActor, BindingFactory>(
                "player",
                factory => factory.DisableRelocation()
            );
            options
                .AddStreamNode("clients")
                .Bind("ws://127.0.0.1:0")
                .EnableActorDispatch()
                .AddSession<BindingSession>();
        });
        using var host = builder.Build();
        await host.StartAsync();
        try
        {
            var manager = host.Services.GetRequiredService<IZLinkActorManager>();
            var created = Assert.IsType<ZLinkActorCreateResult.Created>(
                await manager.Create("same-node-player", "player").InMesh("binding").Async()
            );
            var endpoint = host
                .Services.GetRequiredService<IZLinkFrameworkRuntime>()
                .GetListenerStatus(ZLinkListenerKind.Stream, "clients")
                .Endpoint;
            await using var first = ZlinkStreamConnectorFactory.Create(
                new()
                {
                    Endpoint = new(endpoint),
                    DispatchMode = ZlinkStreamDispatchMode.Immediate,
                    Reconnect = new() { Enabled = false },
                }
            );
            var firstBound = new TaskCompletionSource<IZlinkStreamActor>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            using var firstBinding = first.OnActorBound(
                (actor, _) =>
                {
                    firstBound.TrySetResult(actor);
                    return ValueTask.CompletedTask;
                }
            );
            var disconnected = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            using var observation = first.OnDisconnected(
                (_, _) =>
                {
                    disconnected.TrySetResult();
                    return ValueTask.CompletedTask;
                }
            );
            await first.Connect.Async();
            var current = await state.Sessions.Reader.ReadAsync();
            await current.Context.Actors.BindAsync(created.Actor);
            Assert.Equal(0, state.Callbacks);
            var oldActor = await firstBound.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(
                "before",
                await oldActor.Request(new BindingProbe("before")).Async<string>()
            );
            await using var second = ZlinkStreamConnectorFactory.Create(
                new()
                {
                    Endpoint = new(endpoint),
                    DispatchMode = ZlinkStreamDispatchMode.Immediate,
                    Reconnect = new() { Enabled = false },
                }
            );
            var secondBound = new TaskCompletionSource<IZlinkStreamActor>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            using var secondBinding = second.OnActorBound(
                (actor, _) =>
                {
                    secondBound.TrySetResult(actor);
                    return ValueTask.CompletedTask;
                }
            );
            await second.Connect.Async();
            var incoming = await state.Sessions.Reader.ReadAsync();
            if (accepted)
            {
                await incoming.Context.Actors.BindAsync(created.Actor);
                var currentActor = await secondBound.Task.WaitAsync(TimeSpan.FromSeconds(5));
                Assert.Equal(
                    "current",
                    await currentActor.Request(new BindingProbe("current")).Async<string>()
                );
                await disconnected.Task.WaitAsync(TimeSpan.FromSeconds(5));
                await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
                    await oldActor.Request(new BindingProbe("retired")).Async<string>()
                );
                Assert.Equal(2, state.Ingress);
                Assert.False(first.IsConnected);
                Assert.NotNull(incoming.Context.Actors.Find(created.Actor.ActorId));
            }
            else
            {
                var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                    await incoming.Context.Actors.BindAsync(created.Actor)
                );
                Assert.Equal(ZLinkFrameworkErrorKind.Rejected, failure.Kind);
                Assert.NotNull(current.Context.Actors.Find(created.Actor.ActorId));
                Assert.Null(incoming.Context.Actors.Find(created.Actor.ActorId));
                Assert.True(first.IsConnected);
                Assert.Equal(
                    "current",
                    await oldActor.Request(new BindingProbe("current")).Async<string>()
                );
                Assert.Equal(2, state.Ingress);
            }
            Assert.Equal(callbacks, state.Callbacks);
            Assert.Equal(replacements, state.Replacements);
            Assert.Equal(accepted ? 2 : 1, state.BoundCallbacks);
        }
        finally
        {
            await host.StopAsync();
        }
    }
}

public sealed class BindingState
{
    public string Rejection = "";
    public int Callbacks;
    public int BoundCallbacks;
    public int Replacements;
    public int Ingress;
    public Channel<BindingSession> Sessions { get; } = Channel.CreateUnbounded<BindingSession>();
}

public sealed class BindingActor(IZLinkActorContext context, BindingState state) : IZLinkActor
{
    public IZLinkActorContext Context { get; } = context;

    public ValueTask OnSessionBoundAsync(
        ZLinkSessionBindingIdentity incoming,
        CancellationToken cancellationToken
    )
    {
        Interlocked.Increment(ref state.BoundCallbacks);
        return ValueTask.CompletedTask;
    }

    public ValueTask<string?> OnDuplicateBindAsync(
        ZLinkSessionBindingIdentity current,
        ZLinkSessionBindingIdentity incoming,
        CancellationToken cancellationToken
    )
    {
        Interlocked.Increment(ref state.Callbacks);
        return ValueTask.FromResult<string?>(state.Rejection);
    }
}

public sealed class BindingFactory(BindingState state) : IZLinkActorFactory<BindingActor>
{
    public ValueTask<BindingActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default
    ) => ValueTask.FromResult(new BindingActor(context, state));

    async ValueTask<IZLinkActor> IZLinkActorFactory.CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken
    ) => await CreateAsync(context, cancellationToken);
}

public sealed class BindingEntry(IZLinkEntrySpotContext context) : IZLinkEntrySpot<BindingActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public void Configure() => Context.Handlers.AddActorPacket<BindingProbeHandler, BindingActor>();

    public ValueTask OnJoinedActorAsync(BindingActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectActorAsync(
        BindingActor actor,
        CancellationToken cancellationToken
    ) => ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(BindingActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;
}

public sealed class BindingSession(IZLinkSessionContext context, BindingState state) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage message,
        CancellationToken cancellationToken
    )
    {
        await dispatch.Actor!.RelayAsync(message);
    }

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
    {
        state.Sessions.Writer.TryWrite(this);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnActorBindingReplacedAsync(
        string actorId,
        CancellationToken cancellationToken
    )
    {
        Interlocked.Increment(ref state.Replacements);
        return ValueTask.CompletedTask;
    }
}

public sealed record BindingProbe(string Value);

public sealed class BindingProbeHandler(BindingState state)
    : IZLinkEntrySpotActorRequestHandler<BindingEntry, BindingActor, BindingProbe, string>
{
    public ValueTask<string> HandleAsync(
        BindingEntry spot,
        BindingActor actor,
        IZLinkMessageContext context,
        BindingProbe message,
        CancellationToken cancellationToken
    )
    {
        Interlocked.Increment(ref state.Ingress);
        return ValueTask.FromResult(message.Value);
    }
}
