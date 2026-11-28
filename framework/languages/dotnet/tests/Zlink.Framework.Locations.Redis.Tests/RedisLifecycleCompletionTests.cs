using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using StackExchange.Redis;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Runtime.Backend.DotNet;

namespace Zlink.Framework.Locations.Redis.Tests;

[Collection(RedisTestCollection.Name)]
public sealed class RedisLifecycleCompletionTests(RedisTestFixture redis)
{
    private static readonly TimeSpan Guard = TimeSpan.FromSeconds(5);

    [SkippableTheory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task BlockedFindContinuationLeavesSameStoreFree(bool actor)
    {
        Skip.IfNot(redis.RedisAvailable, redis.SkipReason);
        await using var store = redis.CreateStore();
        using var control = await ConnectionMultiplexer.ConnectAsync(redis.ConnectionString);
        var database = control.GetDatabase();
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.AddLocationStore(store);
            options.AddRouteMesh("lifecycle").Listen("tcp://127.0.0.1:0").Objects().Server();
        });
        await using var provider = services.BuildServiceProvider();
        var hosted = provider.GetServices<IHostedService>().ToArray();
        foreach (var service in hosted)
            await service.StartAsync(CancellationToken.None);

        Task<bool> FindMissingAsync(string id) =>
            actor
                ? IsMissingAsync(provider.GetRequiredService<IZLinkActorManager>().FindAsync(id))
                : IsMissingAsync(provider.GetRequiredService<IZLinkSpotManager>().FindAsync(id));

        try
        {
            Assert.True(await FindMissingAsync("warmup").WaitAsync(Guard));

            var entered = new TaskCompletionSource<(bool Dispatcher, string Thread)>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            using var release = new ManualResetEventSlim();
            Task? dependent = null;
            try
            {
                // Hold the Redis response until the inline continuation is registered.
                Assert.Equal("OK", (string?)await database.ExecuteAsync("CLIENT", "PAUSE", 3000));
                try
                {
                    var find = actor
                        ? Attach(
                            provider.GetRequiredService<IZLinkActorManager>().FindAsync("missing")
                        )
                        : Attach(
                            provider.GetRequiredService<IZLinkSpotManager>().FindAsync("missing")
                        );
                    dependent = find;
                }
                finally
                {
                    await database.ExecuteAsync("CLIENT", "UNPAUSE");
                }

                var turn = await entered.Task.WaitAsync(Guard);
                Assert.IsType<ZLinkStoreReadResult.Missing>(
                    await store.ReadAsync(new ZLinkStoreKey("other-read")).AsTask().WaitAsync(Guard)
                );
                Assert.False(
                    dependent.IsCompleted,
                    "The other read must finish while the continuation waits."
                );
                Assert.True(
                    turn.Dispatcher,
                    $"Find continuation ran outside the Framework completion turn on {turn.Thread}."
                );
            }
            finally
            {
                release.Set();
                if (dependent is not null)
                    await dependent.WaitAsync(Guard);
            }

            Task Attach<T>(ValueTask<T?> operation)
                where T : struct
            {
                var task = operation.AsTask();
                Assert.False(task.IsCompleted);
                return task.ContinueWith(
                    result =>
                    {
                        entered.SetResult(
                            (
                                ZLinkCompletionDispatcher.IsCurrentExecution,
                                Thread.CurrentThread.Name ?? "<unnamed>"
                            )
                        );
                        Assert.True(release.Wait(Guard + Guard), "Continuation cleanup timed out.");
                        Assert.Null(result.GetAwaiter().GetResult());
                    },
                    CancellationToken.None,
                    TaskContinuationOptions.ExecuteSynchronously,
                    TaskScheduler.Default
                );
            }
        }
        finally
        {
            foreach (var service in hosted.Reverse())
                await service.StopAsync(CancellationToken.None);
        }
    }

    private static async Task<bool> IsMissingAsync<T>(ValueTask<T?> operation)
        where T : struct => await operation is null;
}
