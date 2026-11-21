using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class CreationReservationDeadlineTests
{
    [Theory]
    [InlineData(true, true)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(false, false)]
    public async Task GetOrCreate_distinguishes_reservation_deadline_from_caller_cancellation(
        bool actor,
        bool expire
    )
    {
        const string objectId = "deadline-object";
        var store = new SpotCloseFaultStore(new ZLinkInMemoryProviderLocationStore());
        await using var host = await SpotCloseHost.StartAsync(store);
        using var caller = new CancellationTokenSource();
        store.HoldNextPutFor = objectId;
        var timeout = expire ? TimeSpan.FromSeconds(1) : TimeSpan.FromSeconds(30);
        Task operation = actor
            ? host
                .Runtime.Services.GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(objectId, SpotCloseHost.ActorType)
                .Timeout(timeout)
                .Async(caller.Token)
                .AsTask()
            : host
                .Manager.GetOrCreate(objectId, SpotCloseHost.SpotType)
                .Timeout(timeout)
                .Async(caller.Token)
                .AsTask();
        try
        {
            await store.PutHeld.Task.WaitAsync(TimeSpan.FromSeconds(5));
            if (expire)
            {
                var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                    operation.WaitAsync(TimeSpan.FromSeconds(5))
                );
                Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, failure.Kind);
            }
            else
            {
                caller.Cancel();
                await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                    operation.WaitAsync(TimeSpan.FromSeconds(5))
                );
            }
        }
        finally
        {
            store.ReleasePut.TrySetResult();
        }
    }
}
