using Xunit;
using Zlink.Framework.LocationProvider;

namespace Zlink.Framework.Tests;

internal static class LocationStoreCounterContract
{
    internal static async Task VerifyAsync(IZLinkLocationStore store)
    {
        var active = new ZLinkStoreKey("capacity/active");
        var pending = new ZLinkStoreKey("capacity/pending");
        var authority = new ZLinkStoreKey("authority");
        var counterKeys = new[] { active, pending };
        var reserved = await store.WriteAsync(
            new(
                [new ZLinkStoreCondition.SumAtMost(counterKeys, 1, 1)],
                [new ZLinkStoreMutation.Add(pending, 1)]
            )
        );
        var pendingBefore = Assert.IsType<ZLinkStoreReadResult.Found>(
            await store.ReadAsync(pending)
        );
        Assert.Equal(
            Assert.IsType<ZLinkStoreWriteResult.Applied>(reserved).PutVersions[pending],
            pendingBefore.Value.Version
        );
        Assert.IsType<ZLinkStoreWriteResult.Conflict>(
            await store.WriteAsync(
                new(
                    [new ZLinkStoreCondition.SumAtMost(counterKeys, 1, 1)],
                    [new ZLinkStoreMutation.Add(pending, 1)]
                )
            )
        );
        Assert.IsType<ZLinkStoreWriteResult.Corrupt>(
            await store.WriteAsync(
                new(
                    [],
                    [
                        new ZLinkStoreMutation.Put(authority, new byte[] { 0xff }, null),
                        new ZLinkStoreMutation.Add(pending, -2),
                    ]
                )
            )
        );
        Assert.IsType<ZLinkStoreReadResult.Missing>(await store.ReadAsync(authority));
        Assert.Equal(
            pendingBefore.Value.Version,
            Assert.IsType<ZLinkStoreReadResult.Found>(await store.ReadAsync(pending)).Value.Version
        );
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(
                new(
                    [],
                    [
                        new ZLinkStoreMutation.Add(pending, -1),
                        new ZLinkStoreMutation.Add(active, long.MaxValue),
                    ]
                )
            )
        );
        Assert.IsType<ZLinkStoreWriteResult.Conflict>(
            await store.WriteAsync(
                new([new ZLinkStoreCondition.SumAtMost(counterKeys, 1, long.MaxValue)], [])
            )
        );
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(
                new(
                    [new ZLinkStoreCondition.SumAtMost(counterKeys, long.MinValue, 0)],
                    [new ZLinkStoreMutation.Add(active, -1)]
                )
            )
        );
        Assert.Equal(
            "9223372036854775806"u8.ToArray(),
            Assert
                .IsType<ZLinkStoreReadResult.Found>(await store.ReadAsync(active))
                .Value.Bytes.ToArray()
        );
    }

    internal static async Task VerifyConcurrentBatchesAsync(IZLinkLocationStore store)
    {
        const int operations = 200;
        var key = new ZLinkStoreKey("capacity/pending");
        var start = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var batches = Enumerable
            .Range(0, operations)
            .Select(async index =>
            {
                var authority = new ZLinkStoreKey($"authority/{index}");
                await start.Task;
                return await store.WriteAsync(
                    new(
                        [new ZLinkStoreCondition.Missing(authority)],
                        [
                            new ZLinkStoreMutation.Put(authority, "opaque"u8.ToArray(), null),
                            new ZLinkStoreMutation.Add(key, 1),
                        ]
                    )
                );
            })
            .ToArray();

        start.SetResult();
        foreach (var result in await Task.WhenAll(batches))
            Assert.IsType<ZLinkStoreWriteResult.Applied>(result);
        var current = Assert.IsType<ZLinkStoreReadResult.Found>(await store.ReadAsync(key));
        Assert.Equal("200"u8.ToArray(), current.Value.Bytes.ToArray());
    }
}
