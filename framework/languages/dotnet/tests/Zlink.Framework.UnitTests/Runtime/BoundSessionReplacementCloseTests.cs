using System.Collections.Concurrent;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink.Stream.Connector.Contracts;
using Xunit;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.BoundSessionReplacementClose;

// Spec 04-session/02-session-actor-binding §6: after the replaced session's callback
// terminal, the fixed grace timer closes the previous connection. The host runs on the real
// TimeProvider.System, whose timers restore the ExecutionContext captured at creation.
public sealed class BoundSessionReplacementCloseTests
{
    [Fact]
    public async Task Rebinding_an_actor_closes_the_previous_connection_after_the_grace_timer()
    {
        var unhandled = new ConcurrentQueue<object>();
        UnhandledExceptionEventHandler recordUnhandled = (_, e) =>
            unhandled.Enqueue(e.ExceptionObject);
        AppDomain.CurrentDomain.UnhandledException += recordUnhandled;
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddZLinkFramework(o =>
        {
            o.DisableImplicitHandlerAutoRegistration();
            o.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            o.AddLocationStore(new ZLinkInMemoryProviderLocationStore());
            var objects = o.AddRouteMesh("replacement")
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server();
            objects.AddEntrySpot<REntry>();
            objects.AddActorFactory<RActor, RFactory>("actor", f => f.DisableRelocation());
            o.AddStreamNode("clients")
                .Bind("ws://127.0.0.1:0")
                .EnableActorDispatch()
                .AddSession<RSession>();
        });
        using var app = builder.Build();
        await app.StartAsync();
        try
        {
            var actor = "actor:" + Guid.NewGuid();
            var endpoint = app
                .Services.GetRequiredService<IZLinkFrameworkRuntime>()
                .GetListenerStatus(ZLinkListenerKind.Stream, "clients")
                .Endpoint;
            await using var previous = await ConnectAsync(endpoint);
            await previous.Connector.Request(new RLogin(actor)).Async<RPong>();
            await using var replacement = await ConnectAsync(endpoint);
            await replacement.Connector.Request(new RLogin(actor)).Async<RPong>();

            await previous.Replaced.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await previous.Disconnected.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(previous.Connector.IsConnected);
            Assert.Equal(1, RSession.ReplacedCallbacks(actor));
            await replacement.Connector.Request(new RPing()).Async<RPong>();
            Assert.True(replacement.Connector.IsConnected);
            Assert.False(replacement.Disconnected.Task.IsCompleted);
            Assert.Equal(0, Volatile.Read(ref replacement.ReplacedCount));
            Assert.Empty(unhandled);
        }
        finally
        {
            await app.StopAsync();
            AppDomain.CurrentDomain.UnhandledException -= recordUnhandled;
        }
    }

    private static async Task<RClient> ConnectAsync(string endpoint)
    {
        var client = new RClient(
            ZlinkStreamConnectorFactory.Create(
                new()
                {
                    Endpoint = new(endpoint),
                    DispatchMode = ZlinkStreamDispatchMode.Immediate,
                    // The previous connection must stay closed so the assertion observes the
                    // server's grace-timer close, not a later client reconnect.
                    Reconnect = new() { Enabled = false },
                }
            )
        );
        await client.Connector.Connect.Async();
        return client;
    }

    private sealed class RClient : IAsyncDisposable
    {
        private readonly IDisposable _replaced;
        private readonly IDisposable _disconnected;
        public int ReplacedCount;

        public RClient(IZlinkStreamConnector connector)
        {
            Connector = connector;
            _replaced = connector.On<RReplaced>(
                (_, _) =>
                {
                    Interlocked.Increment(ref ReplacedCount);
                    Replaced.TrySetResult();
                    return ValueTask.CompletedTask;
                }
            );
            _disconnected = connector.OnDisconnected(
                (_, _) =>
                {
                    Disconnected.TrySetResult();
                    return ValueTask.CompletedTask;
                }
            );
        }

        public IZlinkStreamConnector Connector { get; }
        public TaskCompletionSource Replaced { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Disconnected { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public async ValueTask DisposeAsync()
        {
            _replaced.Dispose();
            _disconnected.Dispose();
            await Connector.DisposeAsync();
        }
    }
}

public sealed record RLogin(string Actor);

public sealed record RPing;

public sealed record RPong;

public sealed record RReplaced;

public sealed class RActor(IZLinkActorContext context) : IZLinkActor
{
    public IZLinkActorContext Context { get; } = context;
}

public sealed class RFactory : IZLinkActorFactory<RActor>
{
    public ValueTask<RActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken ct = default
    ) => ValueTask.FromResult(new RActor(context));

    async ValueTask<IZLinkActor> IZLinkActorFactory.CreateAsync(
        IZLinkActorContext context,
        CancellationToken ct
    ) => await CreateAsync(context, ct);
}

public sealed class RSession(IZLinkSessionContext context, IZLinkActorManager actors)
    : IZLinkSession
{
    private static readonly ConcurrentDictionary<string, int> Replacements = new();

    public IZLinkSessionContext Context { get; } = context;

    public static int ReplacedCallbacks(string actor) => Replacements.GetValueOrDefault(actor);

    public ValueTask OnConnectedAsync(CancellationToken ct) => ValueTask.CompletedTask;

    public ValueTask OnDisconnectedAsync(CancellationToken ct) => ValueTask.CompletedTask;

    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public async ValueTask OnActorBindingReplacedAsync(string actorId, CancellationToken ct)
    {
        Replacements.AddOrUpdate(actorId, 1, static (_, count) => count + 1);
        await Context.Client.Send(new RReplaced()).Async();
    }

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage message,
        CancellationToken ct
    )
    {
        if (dispatch.PacketName == nameof(RLogin))
        {
            var result = await actors
                .GetOrCreate(message.Decode<RLogin>().Actor, "actor")
                .InMesh("replacement")
                .Async(ct);
            var actor = result switch
            {
                ZLinkActorCreateResult.Created created => created.Actor,
                ZLinkActorCreateResult.Existing existing => existing.Actor,
                _ => throw new InvalidOperationException("Actor creation failed"),
            };
            await Context.Actors.BindAsync(actor, ct);
        }
        await Context.Client.Reply(new RPong()).Async();
    }
}

public sealed class REntry(IZLinkEntrySpotContext context) : IZLinkEntrySpot<RActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public ValueTask OnJoinedActorAsync(RActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectActorAsync(RActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(RActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;
}
