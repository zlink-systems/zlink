using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.StreamDisconnectProgress;

public sealed class StreamDisconnectProgressTests
{
    [Fact]
    public async Task Physical_disconnect_notifies_actor_while_session_handler_awaits()
    {
        var state = new ProgressState();
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddSingleton(state);
        // This backend integration test uses the existing friend assembly and
        // DI port. Resource creation remains in the default native backend.
        builder.Services.AddSingleton<IZLinkBackendAdapterFactory>(
            services => new ObservingBackendFactory(
                new ZLinkDotNetBackendAdapterFactory(services.GetService<ILoggerFactory>()),
                state
            )
        );
        builder.Services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.AddLocationStore(new ZLinkInMemoryProviderLocationStore());
            var objects = options
                .AddRouteMesh("disconnect-progress")
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server();
            objects.AddEntrySpot<ProgressEntry>();
            objects.AddActorFactory<ProgressActor, ProgressFactory>(
                "progress-actor",
                factory => factory.DisableRelocation()
            );
            options
                .AddStreamNode("progress-stream")
                .Bind("tcp://127.0.0.1:0")
                .EnableActorDispatch()
                .AddSession<ProgressSession>();
        });
        using var host = builder.Build();
        await host.StartAsync();
        var endpoint = host
            .Services.GetRequiredService<IZLinkFrameworkRuntime>()
            .GetListenerStatus(ZLinkListenerKind.Stream, "progress-stream")
            .Endpoint;
        await using var client = ZlinkStreamConnectorFactory.Create(
            new()
            {
                Endpoint = new(endpoint),
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                Reconnect = new() { Enabled = false },
                Heartbeat = new() { Enabled = false },
            }
        );
        var actorBeforeRelease = false;
        try
        {
            await client.Connect.Async();
            await client
                .Request(new ProgressLogin("progress:" + Guid.NewGuid()))
                .Async<ProgressReply>();
            await client.Send(new ProgressHold()).Async();
            await state.HandlerEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await client.Close.Async();

            await state.PhysicalDisconnected.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(state.HandlerExited.Task.IsCompleted);
            Assert.False(state.SessionDisconnected.Task.IsCompleted);
            var winner = await Task.WhenAny(
                state.ActorDisconnected.Task,
                Task.Delay(TimeSpan.FromSeconds(1))
            );
            actorBeforeRelease = ReferenceEquals(winner, state.ActorDisconnected.Task);
            Assert.False(state.HandlerExited.Task.IsCompleted);
            Assert.False(state.SessionDisconnected.Task.IsCompleted);
        }
        finally
        {
            state.ReleaseHandler.TrySetResult();
            try
            {
                if (state.HandlerEntered.Task.IsCompletedSuccessfully)
                {
                    await state.HandlerExited.Task.WaitAsync(TimeSpan.FromSeconds(5));
                    await state.ActorDisconnected.Task.WaitAsync(TimeSpan.FromSeconds(5));
                    await state.SessionDisconnected.Task.WaitAsync(TimeSpan.FromSeconds(5));
                }
            }
            finally
            {
                await host.StopAsync();
            }
        }
        Assert.Equal(1, Volatile.Read(ref state.ActorDisconnectCount));
        Assert.Equal(1, Volatile.Read(ref state.StreamMonitorOpenCount));
        Assert.Equal(1, Volatile.Read(ref state.PhysicalDisconnectCount));
        Assert.Equal(1, Volatile.Read(ref state.SessionDisconnectCount));
        Assert.True(
            actorBeforeRelease,
            "Automatic Actor disconnect must progress while the Session handler awaits."
        );
    }
}

public sealed class ProgressState
{
    public TaskCompletionSource PhysicalDisconnected { get; } = NewSignal();
    public TaskCompletionSource HandlerEntered { get; } = NewSignal();
    public TaskCompletionSource ReleaseHandler { get; } = NewSignal();
    public TaskCompletionSource HandlerExited { get; } = NewSignal();
    public TaskCompletionSource SessionDisconnected { get; } = NewSignal();
    public TaskCompletionSource ActorDisconnected { get; } = NewSignal();
    public int ActorDisconnectCount;
    public int StreamMonitorOpenCount;
    public int PhysicalDisconnectCount;
    public int SessionDisconnectCount;

    private static TaskCompletionSource NewSignal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);
}

internal sealed class ObservingBackendFactory(
    IZLinkBackendAdapterFactory original,
    ProgressState state
) : IZLinkBackendAdapterFactory
{
    public IZLinkBackendRuntimeContext CreateRuntimeContext() => original.CreateRuntimeContext();

    public IZLinkMonitoringBackendAdapter CreateMonitoringAdapter() =>
        new ObservingMonitoringAdapter(original.CreateMonitoringAdapter(), state);
}

internal sealed class ObservingMonitoringAdapter(
    IZLinkMonitoringBackendAdapter original,
    ProgressState state
) : IZLinkMonitoringBackendAdapter
{
    public IZLinkBackendSocketMonitor OpenSocketMonitor(IAsyncDisposable socket)
    {
        var monitor = original.OpenSocketMonitor(socket);
        if (socket is not IZLinkBackendStreamSocket)
            return monitor;
        Interlocked.Increment(ref state.StreamMonitorOpenCount);
        return new ObservingStreamMonitor(monitor, state);
    }
}

internal sealed class ObservingStreamMonitor(
    IZLinkBackendSocketMonitor original,
    ProgressState state
) : IZLinkBackendSocketMonitor
{
    public IZLinkBackendSocketPoller CreateReceivePoller() => original.CreateReceivePoller();

    public bool TryRecv(out ZLinkBackendSocketMonitorEvent monitorEvent)
    {
        // The default wrapper calls its existing native monitor's public Recv
        // exactly once. Preserve the received value and exception unchanged.
        var received = original.TryRecv(out monitorEvent);
        if (received && monitorEvent.NativeEvent == ZLinkSocketNativeEventType.Disconnected)
        {
            Interlocked.Increment(ref state.PhysicalDisconnectCount);
            state.PhysicalDisconnected.TrySetResult();
        }
        return received;
    }

    public ValueTask DisposeAsync() => original.DisposeAsync();
}

public sealed record ProgressLogin(string ActorId);

public sealed record ProgressHold;

public sealed record ProgressReply;

public sealed class ProgressSession(
    IZLinkSessionContext context,
    IZLinkActorManager actors,
    ProgressState state
) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
    {
        Assert.True(
            state.HandlerExited.Task.IsCompletedSuccessfully,
            "Session disconnect callback must run after the accepted Session handler exits."
        );
        Interlocked.Increment(ref state.SessionDisconnectCount);
        state.SessionDisconnected.TrySetResult();
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage message,
        CancellationToken cancellationToken
    )
    {
        if (dispatch.PacketName == nameof(ProgressLogin))
        {
            var result = await actors
                .GetOrCreate(message.Decode<ProgressLogin>().ActorId, "progress-actor")
                .InMesh("disconnect-progress")
                .Async(cancellationToken);
            var actor = result switch
            {
                ZLinkActorCreateResult.Created created => created.Actor,
                ZLinkActorCreateResult.Existing existing => existing.Actor,
                _ => throw new InvalidOperationException("Actor creation failed."),
            };
            await Context.Actors.BindOrGetAsync(actor, cancellationToken);
            await Context.Client.Reply(new ProgressReply()).Async();
            return;
        }
        if (dispatch.PacketName != nameof(ProgressHold))
            throw new InvalidOperationException("Unexpected Session packet.");
        state.HandlerEntered.TrySetResult();
        try
        {
            await state.ReleaseHandler.Task.WaitAsync(cancellationToken);
        }
        finally
        {
            state.HandlerExited.TrySetResult();
        }
    }
}

public sealed class ProgressActor(IZLinkActorContext context) : IZLinkActor
{
    public IZLinkActorContext Context { get; } = context;
}

public sealed class ProgressFactory : IZLinkActorFactory<ProgressActor>
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

public sealed class ProgressEntry(IZLinkEntrySpotContext context, ProgressState state)
    : IZLinkEntrySpot<ProgressActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public void Configure() { }

    public ValueTask OnJoinedActorAsync(ProgressActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(ProgressActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectActorAsync(
        ProgressActor actor,
        CancellationToken cancellationToken
    )
    {
        Interlocked.Increment(ref state.ActorDisconnectCount);
        state.ActorDisconnected.TrySetResult();
        return ValueTask.CompletedTask;
    }
}
