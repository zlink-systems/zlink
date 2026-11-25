using Etcdserverpb;
using Zlink.Framework.Tests;

namespace Zlink.Framework.Locations.Etcd.Tests;

public sealed class EtcdLocationStoreCounterTests
{
    [Fact]
    public async Task CounterOperationsAreAtomic()
    {
        await using var server = await FakeEtcdServer.StartAsync();
        server.Clock();
        await using var store = server.CreateStore();
        await LocationStoreCounterContract.VerifyAsync(store);
    }

    [Fact]
    public async Task CounterRevisionChangeIsReevaluatedInsideProvider()
    {
        await using var server = await FakeEtcdServer.StartAsync();
        server.Clock();
        server.Record("capacity", "1"u8.ToArray());
        var changed = false;
        server.BeforeCall = (_, request, _) =>
        {
            if (
                !changed
                && request is TxnRequest txn
                && txn.Success.Any(op => op.RequestPut is not null)
            )
            {
                changed = true;
                server.Record("capacity", "2"u8.ToArray());
            }
            return Task.CompletedTask;
        };
        await using var store = server.CreateStore();
        var key = new ZLinkStoreKey("capacity");
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(
                new(
                    [new ZLinkStoreCondition.SumAtMost([key], 1, 3)],
                    [new ZLinkStoreMutation.Add(key, 1)]
                )
            )
        );
        Assert.True(changed);
        Assert.Equal(1, server.WriteCount);
        var current = Assert.IsType<ZLinkStoreReadResult.Found>(await store.ReadAsync(key));
        Assert.Equal("3"u8.ToArray(), current.Value.Bytes.ToArray());
    }
}
