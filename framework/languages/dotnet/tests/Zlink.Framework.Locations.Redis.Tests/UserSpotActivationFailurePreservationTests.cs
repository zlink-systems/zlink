using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace Zlink.Framework.Locations.Redis.Tests;

public sealed class UserSpotActivationFailurePreservationTests(RedisTestFixture redis)
    : IClassFixture<RedisTestFixture>
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task MessageLessCallbackFailureReachesCreationCaller(bool failInitialization)
    {
        Assert.True(redis.RedisAvailable, redis.SkipReason);
        var original = new MessageLessFailure();
        var builder = Host.CreateApplicationBuilder();
        var services = builder.Services;
        services.AddSingleton(new FailureScenario(original, failInitialization));
        services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(redis.CreateStore());
            options
                .AddRouteMesh("activation-failure")
                .Listen("tcp://127.0.0.1:0")
                .SetSpotLimit(1)
                .Objects()
                .Server()
                .AddSpotFactory<FailingSpot>(
                    "failing-spot",
                    factory => factory.DisableRelocation()
                );
        });
        var host = builder.Build();
        await using var lifetime = (IAsyncDisposable)host;
        await host.StartAsync(CancellationToken.None);
        try
        {
            var spots = host.Services.GetRequiredService<IZLinkSpotManager>();
            var failure = await Assert.ThrowsAnyAsync<Exception>(async () =>
                await spots
                    .GetOrCreate("failed-activation", "failing-spot")
                    .InMesh("activation-failure")
                    .Request("activation-request")
                    .Async()
            );

            Assert.Same(original, failure.GetBaseException());
            Assert.Null(original.Message);
            Assert.Null(await spots.FindAsync("failed-activation"));
        }
        finally
        {
            await host.StopAsync(CancellationToken.None);
        }
    }

    public sealed class MessageLessFailure : Exception
    {
        public override string Message => null!;
    }

    public sealed record FailureScenario(Exception Failure, bool FailInitialization);

    public sealed class FailingSpot(IZLinkSpotContext context, FailureScenario scenario)
        : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;

        public ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
            ZLinkMessage request,
            CancellationToken cancellationToken
        ) =>
            scenario.FailInitialization
                ? ValueTask.FromResult(ZLinkSpotCreateResponse.Accept())
                : throw scenario.Failure;

        public ValueTask OnInitializeAsync(CancellationToken cancellationToken) =>
            ValueTask.FromException(scenario.Failure);
    }
}
