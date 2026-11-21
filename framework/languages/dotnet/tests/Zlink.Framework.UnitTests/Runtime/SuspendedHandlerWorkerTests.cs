using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.UnitTests.SuspendedHandlerWorker;

public sealed class SuspendedHandlerWorkerTests
{
    [Fact]
    public async Task Two_suspended_spot_handlers_return_workers_for_another_spot()
    {
        var previousProcessorCount = Environment.GetEnvironmentVariable("DOTNET_PROCESSOR_COUNT");
        Environment.SetEnvironmentVariable("DOTNET_PROCESSOR_COUNT", "2");
        try
        {
            if (
                await IsolatedTestProcess.RunIsolatedAsync(
                    typeof(SuspendedHandlerWorkerTests),
                    nameof(Two_suspended_spot_handlers_return_workers_for_another_spot)
                )
            )
                return;
        }
        finally
        {
            Environment.SetEnvironmentVariable("DOTNET_PROCESSOR_COUNT", previousProcessorCount);
        }

        Assert.Equal(2, ZLinkProcessExecutionPool.ApplicationWorkerCount);
        var probe = new Probe();
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddSingleton(probe);
        builder.Services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.UseTestLocationStore();
            options
                .AddRouteMesh("suspended-workers")
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server()
                .AddSpotFactory<Room>("room", factory => factory.DisableRelocation());
        });
        using var host = builder.Build();
        await host.StartAsync();
        try
        {
            var spots = host.Services.GetRequiredService<IZLinkSpotManager>();
            foreach (var id in new[] { "b", "a0", "a1" })
                await spots.GetOrCreate(id, "room").InMesh("suspended-workers").Async();
            var client = host.Services.GetRequiredService<IZLinkSpotClient>();
            var replies = await Task.WhenAll(
                    Enumerable
                        .Range(0, 2)
                        .Select(id =>
                            client
                                .RequestToSpot($"a{id}", new Outer(id))
                                .Timeout(TimeSpan.FromSeconds(2))
                                .Async<Reply>()
                                .AsTask()
                        )
                )
                .WaitAsync(TimeSpan.FromSeconds(2));
            Assert.Equal(new[] { new Reply(0, true), new Reply(1, true) }, replies);
            Assert.Equal(2, probe.Arrived);
        }
        finally
        {
            await host.StopAsync();
        }
    }
}

public sealed class Probe
{
    public int Arrived;
    public TaskCompletionSource Both { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
}

public sealed record Outer(int Id);

public sealed record Inner(int Id);

public sealed record Reply(int Id, bool Ok);

public sealed class Room(IZLinkSpotContext context) : IZLinkSpot<IZLinkActor>
{
    public IZLinkSpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddPacket<OuterHandler>();
        Context.Handlers.AddPacket<InnerHandler>();
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken ct
    ) => ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());

    public ValueTask OnJoinedActorAsync(IZLinkActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(IZLinkActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;
}

public sealed class OuterHandler(Probe probe) : IZLinkSpotRequestHandler<Room, Outer, Reply>
{
    public async ValueTask<Reply> HandleAsync(
        Room spot,
        IZLinkMessageContext context,
        Outer request,
        CancellationToken ct
    )
    {
        if (Interlocked.Increment(ref probe.Arrived) == 2)
            probe.Both.TrySetResult();
        await probe.Both.Task;
        return await spot
            .Context.Outbound.RequestToSpot("b", new Inner(request.Id))
            .Timeout(TimeSpan.FromSeconds(2))
            .Async<Reply>();
    }
}

public sealed class InnerHandler : IZLinkSpotRequestHandler<Room, Inner, Reply>
{
    public ValueTask<Reply> HandleAsync(
        Room spot,
        IZLinkMessageContext context,
        Inner request,
        CancellationToken ct
    ) => ValueTask.FromResult(new Reply(request.Id, true));
}
