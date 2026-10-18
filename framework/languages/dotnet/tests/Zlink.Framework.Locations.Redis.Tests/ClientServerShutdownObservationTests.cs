using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Handlers;

namespace Zlink.Framework.Locations.Redis.Tests;

[CollectionDefinition(Name, DisableParallelization = true)]
public sealed class ClientServerShutdownCollection : ICollectionFixture<RedisTestFixture>
{
    public const string Name = "ClientServer shutdown observation";
}

[Collection(ClientServerShutdownCollection.Name)]
public sealed class ClientServerShutdownObservationTests(RedisTestFixture redis)
{
    private const int Iterations = 10;

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Awaited_host_shutdown_observes_internal_submit_failures(bool serverFirst)
    {
        Assert.True(redis.RedisAvailable, redis.SkipReason);
        CollectUnobservedTasks();
        var unobserved = new ConcurrentQueue<string>();
        void OnUnobserved(object? sender, UnobservedTaskExceptionEventArgs args)
        {
            foreach (var error in args.Exception.Flatten().InnerExceptions)
                if (error is ZlinkSubmitException)
                    unobserved.Enqueue(error.ToString());
        }

        TaskScheduler.UnobservedTaskException += OnUnobserved;
        try
        {
            for (var iteration = 0; iteration < Iterations; iteration++)
            {
                await RunCycleAsync(serverFirst, iteration);
                CollectUnobservedTasks();
            }
            Assert.True(
                unobserved.IsEmpty,
                $"serverFirst={serverFirst}, iterations={Iterations}, unobserved={unobserved.Count}"
                    + Environment.NewLine
                    + string.Join(Environment.NewLine, unobserved)
            );
        }
        finally
        {
            TaskScheduler.UnobservedTaskException -= OnUnobserved;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private async Task RunCycleAsync(bool serverFirst, int iteration)
    {
        var store = redis.CreateStore(out var prefix);
        await using var server = MakeHost(true, store);
        await using var client = MakeHost(
            false,
            new ZLinkRedisLocationStore(
                new ZLinkRedisLocationOptions
                {
                    ConnectionString = redis.ConnectionString,
                    KeyPrefix = prefix,
                }
            )
        );
        await server.StartAsync();
        await client.StartAsync();
        var reply = await client
            .Services.GetRequiredService<IZLinkRouteClient>()
            .RequestToChannel("work", new Ping(iteration))
            .Async<Pong>();
        Assert.Equal(iteration, reply.Value);
        if (serverFirst)
        {
            await server.StopAsync();
            await client.StopAsync();
        }
        else
        {
            await client.StopAsync();
            await client.DisposeAsync();
            await server.StopAsync();
        }
    }

    private static WebApplication MakeHost(bool server, ZLinkRedisLocationStore store)
    {
        var builder = WebApplication.CreateBuilder();
        builder.Logging.ClearProviders();
        builder.Logging.AddSimpleConsole();
        builder.WebHost.UseUrls("http://127.0.0.1:0");
        builder.Services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.AddLocationStore(store);
            if (server)
                options
                    .AddClientServerChannel("work")
                    .Server()
                    .Listen()
                    .SetBindHost("127.0.0.1")
                    .SetAdvertiseHost("127.0.0.1")
                    .AddRequestHandler<PingHandler, Ping, Pong>();
            else
                options.AddClientServerChannel("work").Client();
        });
        return builder.Build();
    }

    private static void CollectUnobservedTasks()
    {
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
    }

    public sealed record Ping(int Value);

    public sealed record Pong(int Value);

    public sealed class PingHandler : IZLinkRequestHandler<Ping, Pong>
    {
        public ValueTask<Pong> HandleAsync(
            Ping request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(new Pong(request.Value));
    }
}
