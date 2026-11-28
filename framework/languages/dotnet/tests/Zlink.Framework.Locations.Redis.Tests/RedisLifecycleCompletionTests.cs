using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using StackExchange.Redis;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Locations.Redis.Tests;

[Collection(RedisTestCollection.Name)]
public sealed class RedisLifecycleCompletionTests(RedisTestFixture redis)
{
    private static readonly TimeSpan Guard = TimeSpan.FromSeconds(5);
    private const string Mesh = "lifecycle";
    private const string ActorType = "probe-actor";
    private const string SpotType = "probe-spot";

    public enum Operation
    {
        ActorFind,
        SpotFind,
        ActorCreate,
        ActorDestroy,
        SpotManagerClose,
        ActorCreateYield,
        SpotContextClose,
    }

    [SkippableTheory]
    [InlineData(Operation.ActorFind)]
    [InlineData(Operation.SpotFind)]
    [InlineData(Operation.ActorCreate)]
    [InlineData(Operation.ActorDestroy)]
    [InlineData(Operation.SpotManagerClose)]
    [InlineData(Operation.ActorCreateYield)]
    public Task BlockedLifecycleContinuationLeavesSameStoreFree(Operation operation) =>
        VerifyCompletionAsync(operation);

    [SkippableFact]
    public Task SpotContextCloseContinuationLeavesSameStoreFree() =>
        VerifyCompletionAsync(Operation.SpotContextClose);

    private async Task VerifyCompletionAsync(Operation operation)
    {
        Skip.IfNot(redis.RedisAvailable, redis.SkipReason);
        await using var store = redis.CreateStore();
        using var control = await ConnectionMultiplexer.ConnectAsync(redis.ConnectionString);
        var database = control.GetDatabase();
        using var probe = new CompletionProbe();
        var services = new ServiceCollection();
        services.AddSingleton(probe);
        services.AddTransient<ProbeHandler>();
        services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.AddLocationStore(store);
            options
                .AddRouteMesh(Mesh)
                .Listen("tcp://127.0.0.1:0")
                .Objects()
                .Server()
                .AddEntrySpot<ProbeEntrySpot>()
                .AddActorFactory<ProbeActor, ProbeActorFactory>(
                    ActorType,
                    factory => factory.DisableRelocation()
                )
                .AddSpotFactory<ProbeSpot>(
                    SpotType,
                    factory =>
                        factory
                            .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                            .DisableRelocation()
                );
        });
        await using var provider = services.BuildServiceProvider();
        var hosted = provider.GetServices<IHostedService>().ToArray();
        foreach (var service in hosted)
            await service.StartAsync(CancellationToken.None);

        var actors = provider.GetRequiredService<IZLinkActorManager>();
        var spots = provider.GetRequiredService<IZLinkSpotManager>();
        var actorId = $"actor-{Guid.NewGuid():N}";
        Task<string>? handler = null;
        Task? dependent = null;
        try
        {
            if (operation == Operation.SpotFind)
                Assert.Null(await spots.FindAsync("warmup").AsTask().WaitAsync(Guard));
            else
                Assert.Null(await actors.FindAsync("warmup").AsTask().WaitAsync(Guard));

            ActorRef actor = default;
            if (operation == Operation.ActorDestroy)
                actor = Assert
                    .IsType<ZLinkActorCreateResult.Created>(
                        await actors
                            .Create(actorId, ActorType)
                            .InMesh(Mesh)
                            .Async()
                            .AsTask()
                            .WaitAsync(Guard)
                    )
                    .Actor;

            SpotRef spot = default;
            if (
                operation
                is Operation.SpotManagerClose
                    or Operation.SpotContextClose
                    or Operation.ActorCreateYield
            )
                spot = (
                    await spots.Create(SpotType).InMesh(Mesh).Async().AsTask().WaitAsync(Guard)
                ).Spot;

            if (operation is Operation.SpotContextClose or Operation.ActorCreateYield)
            {
                handler = provider
                    .GetRequiredService<IZLinkSpotClient>()
                    .RequestToSpot(spot.SpotId, "lifecycle-probe")
                    .Async<string>()
                    .AsTask();
                await WaitForHandlerSignalAsync(probe.HandlerEntered.Task, handler);
            }

            // Hold the Redis response until the inline continuation is registered.
            Assert.Equal("OK", (string?)await database.ExecuteAsync("CLIENT", "PAUSE", 3000));
            try
            {
                dependent = operation switch
                {
                    Operation.ActorFind => probe.Attach(
                        actors.FindAsync("missing"),
                        result => Assert.Null(result)
                    ),
                    Operation.SpotFind => probe.Attach(
                        spots.FindAsync("missing"),
                        result => Assert.Null(result)
                    ),
                    Operation.ActorCreate => probe.Attach(
                        actors.Create(actorId, ActorType).InMesh(Mesh).Async(),
                        result => Assert.IsType<ZLinkActorCreateResult.Created>(result)
                    ),
                    Operation.ActorDestroy => probe.Attach(
                        actors.DestroyAsync(actor),
                        result => Assert.True(result)
                    ),
                    Operation.SpotManagerClose => probe.Attach(
                        spots.CloseAsync(spot),
                        result => Assert.True(result)
                    ),
                    _ => null,
                };

                if (handler is not null)
                {
                    probe.StartHandler.SetResult(
                        operation == Operation.SpotContextClose
                            ? (context, ct) =>
                            {
                                // The close drains the originating handler before completing.
                                _ = probe.Attach(
                                    context.CloseAsync(ct),
                                    result => Assert.True(result)
                                );
                                return ValueTask.CompletedTask;
                            }
                            : async (context, ct) =>
                            {
                                var turn = ZLinkSerialTurn.Current;
                                Assert.NotNull(turn);
                                var scope = ZLinkApplicationExecutionContext.Current;
                                Assert.NotNull(scope);
                                var pending = actors
                                    .Create(actorId, ActorType)
                                    .InMesh(Mesh)
                                    .Yield(ct)
                                    .AsTask();
                                _ = probe.Attach(
                                    new ValueTask<ZLinkActorCreateResult>(pending),
                                    result => Assert.IsType<ZLinkActorCreateResult.Created>(result)
                                );
                                _ = await pending.ConfigureAwait(false);
                                Assert.Same(turn, ZLinkSerialTurn.Current);
                                Assert.Equal(scope, ZLinkApplicationExecutionContext.Current);
                                Assert.False(turn.Suspended.IsCompleted);
                            }
                    );
                    await WaitForHandlerSignalAsync(probe.Registered.Task, handler);
                    dependent = await probe.Registered.Task;
                }
            }
            finally
            {
                await database.ExecuteAsync("CLIENT", "UNPAUSE");
            }

            var completion = await probe.Entered.Task.WaitAsync(Guard);
            Assert.IsType<ZLinkStoreReadResult.Missing>(
                await store.ReadAsync(new ZLinkStoreKey("other-read")).AsTask().WaitAsync(Guard)
            );
            Assert.False(
                dependent!.IsCompleted,
                "The other read must finish while the continuation waits."
            );
            Assert.True(
                completion.Dispatcher,
                $"{operation} continuation ran outside the Framework completion turn on {completion.Thread}."
            );
        }
        finally
        {
            probe.Release.Set();
            probe.StartHandler.TrySetCanceled();
            try
            {
                if (dependent is not null)
                    await dependent.WaitAsync(Guard);
                if (handler is not null && !probe.StartHandler.Task.IsCanceled)
                    Assert.Equal("lifecycle-probe", await handler.WaitAsync(Guard));
            }
            finally
            {
                foreach (var service in hosted.Reverse())
                    await service.StopAsync(CancellationToken.None);
            }
        }
    }

    private static async Task WaitForHandlerSignalAsync(Task signal, Task handler)
    {
        if (await Task.WhenAny(signal, handler).WaitAsync(Guard) == handler)
            await handler;
        await signal.WaitAsync(Guard);
    }

    private sealed class CompletionProbe : IDisposable
    {
        public TaskCompletionSource HandlerEntered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource<
            Func<IZLinkSpotContext, CancellationToken, ValueTask>
        > StartHandler { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource<Task> Registered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource<(bool Dispatcher, string Thread)> Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public ManualResetEventSlim Release { get; } = new();

        public Task Attach<T>(ValueTask<T> operation, Action<T> assertResult)
        {
            var task = operation.AsTask();
            Assert.False(task.IsCompleted);
            var dependent = task.ContinueWith(
                result =>
                {
                    Entered.SetResult(
                        (
                            ZLinkCompletionDispatcher.IsCurrentExecution,
                            Thread.CurrentThread.Name ?? "<unnamed>"
                        )
                    );
                    Assert.True(Release.Wait(Guard + Guard), "Continuation cleanup timed out.");
                    assertResult(result.GetAwaiter().GetResult());
                },
                CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default
            );
            Registered.SetResult(dependent);
            return dependent;
        }

        public void Dispose() => Release.Dispose();
    }

    private sealed class ProbeActor(IZLinkActorContext context) : IZLinkActor
    {
        public string ActorId => context.ActorId;
        public IZLinkActorContext Context => context;
    }

    private sealed class ProbeActorFactory : IZLinkActorFactory<ProbeActor>
    {
        public ValueTask<ProbeActor> CreateAsync(
            IZLinkActorContext context,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(new ProbeActor(context));
    }

    private sealed class ProbeEntrySpot(IZLinkEntrySpotContext context)
        : IZLinkEntrySpot<ProbeActor>
    {
        public IZLinkEntrySpotContext Context => context;

        public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
            ProbeActor actor,
            ZLinkMessage request,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(ZLinkActorCreateResponse.Accept());

        public ValueTask OnJoinedActorAsync(
            ProbeActor actor,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(ProbeActor actor, CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;
    }

    private sealed class ProbeSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context => context;

        public void Configure() => Context.Handlers.AddPacket<ProbeHandler>();
    }

    private sealed class ProbeHandler(CompletionProbe probe)
        : IZLinkSpotRequestHandler<ProbeSpot, string, string>
    {
        public async ValueTask<string> HandleAsync(
            ProbeSpot spot,
            IZLinkMessageContext context,
            string request,
            CancellationToken cancellationToken
        )
        {
            probe.HandlerEntered.SetResult();
            var start = await probe
                .StartHandler.Task.WaitAsync(cancellationToken)
                .ConfigureAwait(false);
            await start(spot.Context, cancellationToken).ConfigureAwait(false);
            return request;
        }
    }
}
