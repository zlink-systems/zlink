using System.Security.Cryptography;
using System.Text;
using StackExchange.Redis;

namespace Zlink.Framework.Locations.Redis.Tests;

[Collection(RedisTestCollection.Name)]
public sealed class RedisOpaqueProviderTests(RedisTestFixture fixture)
{
    [Fact]
    public void Location_provider_implements_only_the_opaque_store_contract()
    {
        var locationInterfaces = typeof(ZLinkRedisLocationStore).GetInterfaces();
        var relocationInterfaces =
            typeof(ZLinkRedisRelocationStore).GetInterfaces();

        Assert.Contains(typeof(IZLinkLocationStore), locationInterfaces);
        Assert.DoesNotContain(
            locationInterfaces,
            type => type.Name == "IZLinkLocationRepository");
        Assert.Contains(typeof(IZLinkRelocationStore), relocationInterfaces);
        Assert.DoesNotContain(
            relocationInterfaces,
            type => type.Name == "IZLinkRelocationRepository");

        var dependencies = typeof(ZLinkRedisLocationStore).Assembly
            .GetReferencedAssemblies()
            .Select(static dependency => dependency.Name)
            .ToArray();
        Assert.Contains(
            "Zlink.Framework.Provider.Abstractions",
            dependencies);
        Assert.DoesNotContain("Zlink.Framework", dependencies);

        var exactOptionMembers = new[]
        {
            "get_ConfigurationOptions",
            "get_ConnectionString",
            "get_KeyPrefix",
            "get_OperationTimeout",
            "set_ConfigurationOptions",
            "set_ConnectionString",
            "set_KeyPrefix",
            "set_OperationTimeout"
        };
        Assert.Equal(
            exactOptionMembers,
            typeof(ZLinkRedisLocationOptions)
                .GetMethods(
                    System.Reflection.BindingFlags.Public
                    | System.Reflection.BindingFlags.Instance
                    | System.Reflection.BindingFlags.DeclaredOnly)
                .Select(static method => method.Name)
                .Order(StringComparer.Ordinal));
        Assert.Equal(
            exactOptionMembers,
            typeof(ZLinkRedisRelocationOptions)
                .GetMethods(
                    System.Reflection.BindingFlags.Public
                    | System.Reflection.BindingFlags.Instance
                    | System.Reflection.BindingFlags.DeclaredOnly)
                .Select(static method => method.Name)
                .Order(StringComparer.Ordinal));
    }

    [SkippableFact]
    public async Task Location_store_applies_the_conditional_batch_atomically()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var store = fixture.CreateStore();
        var key = new ZLinkStoreKey("authority:actor:player-1");

        var first = Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(new ZLinkStoreWriteRequest(
                [new ZLinkStoreCondition.Missing(key)],
                [new ZLinkStoreMutation.Put(key, new byte[] { 1, 2, 3 }, null)])));
        var version = Assert.Single(first.PutVersions).Value;

        Assert.IsType<ZLinkStoreWriteResult.Conflict>(
            await store.WriteAsync(new ZLinkStoreWriteRequest(
                [new ZLinkStoreCondition.Missing(key)],
                [new ZLinkStoreMutation.Delete(key)])));

        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(new ZLinkStoreWriteRequest(
                [new ZLinkStoreCondition.Version(key, version)],
                [new ZLinkStoreMutation.Put(key, new byte[] { 4 }, null)])));
        var found = Assert.IsType<ZLinkStoreReadResult.Found>(
            await store.ReadAsync(key));
        Assert.Equal(new byte[] { 4 }, found.Value.Bytes.ToArray());
    }

    [SkippableFact]
    public async Task Relocation_store_preserves_caller_reference_idempotency()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var store = fixture.CreateRelocationStore();
        var reference = new ZLinkBlobReference(Guid.NewGuid().ToString("N"));
        var retention = TimeSpan.FromMinutes(1);

        Assert.IsType<ZLinkBlobPutResult.Stored>(
            await store.PutAsync(reference, new byte[] { 1, 2 }, retention));
        Assert.IsType<ZLinkBlobPutResult.AlreadyStored>(
            await store.PutAsync(reference, new byte[] { 1, 2 }, retention));
        Assert.IsType<ZLinkBlobPutResult.Conflict>(
            await store.PutAsync(reference, new byte[] { 9 }, retention));

        var found = Assert.IsType<ZLinkBlobReadResult.Found>(
            await store.ReadAsync(reference));
        Assert.Equal(new byte[] { 1, 2 }, found.Bytes.ToArray());
        Assert.IsType<ZLinkBlobRenewResult.Renewed>(
            await store.RenewAsync(reference, retention));

        await store.DeleteAsync(reference);
        Assert.IsType<ZLinkBlobReadResult.Missing>(
            await store.ReadAsync(reference));
        Assert.IsType<ZLinkBlobRenewResult.Missing>(
            await store.RenewAsync(reference, retention));
    }

    [SkippableFact]
    public async Task Location_scan_keeps_the_first_page_snapshot()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var store = fixture.CreateStore();
        foreach (var (key, value) in new[]
                 {
                     ("actor:1", (byte)1),
                     ("actor:2", (byte)2),
                     ("actor:3", (byte)3),
                     ("other:1", (byte)9)
                 })
        {
            Assert.IsType<ZLinkStoreWriteResult.Applied>(
                await store.WriteAsync(new ZLinkStoreWriteRequest(
                    [new ZLinkStoreCondition.Missing(new ZLinkStoreKey(key))],
                    [new ZLinkStoreMutation.Put(
                        new ZLinkStoreKey(key),
                        new[] { value },
                        null)])));
        }

        var first = Assert.IsType<ZLinkStoreScanResult.Page>(
            await store.ScanAsync(new ZLinkStoreScanRequest(
                "actor:",
                null,
                2)));
        Assert.Equal(2, first.Value.Items.Count);
        Assert.NotNull(first.Value.NextCursor);

        var thirdKey = new ZLinkStoreKey("actor:3");
        var third = Assert.IsType<ZLinkStoreReadResult.Found>(
            await store.ReadAsync(thirdKey));
        var fourthKey = new ZLinkStoreKey("actor:4");
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(new ZLinkStoreWriteRequest(
                [
                    new ZLinkStoreCondition.Version(
                        thirdKey,
                        third.Value.Version),
                    new ZLinkStoreCondition.Missing(fourthKey)
                ],
                [
                    new ZLinkStoreMutation.Delete(thirdKey),
                    new ZLinkStoreMutation.Put(
                        fourthKey,
                        new byte[] { 4 },
                        null)
                ])));

        var second = Assert.IsType<ZLinkStoreScanResult.Page>(
            await store.ScanAsync(new ZLinkStoreScanRequest(
                "actor:",
                first.Value.NextCursor,
                2)));
        var item = Assert.Single(second.Value.Items);
        Assert.Equal("actor:3", item.Key.Value);
        Assert.Equal(new byte[] { 3 }, item.Value.Bytes.ToArray());
        Assert.Null(second.Value.NextCursor);

        Assert.IsType<ZLinkStoreScanResult.Expired>(
            await store.ScanAsync(new ZLinkStoreScanRequest(
                "actor:",
                new ZLinkStoreScanCursor(
                    $"{Guid.NewGuid():N}:0"),
                2)));
    }

    [SkippableFact]
    public async Task Location_scan_cursor_stores_only_bounded_metadata()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var store = fixture.CreateStore(out var keyPrefix);
        for (var index = 0; index < 4; index++)
        {
            var key = new ZLinkStoreKey($"metadata:{index}");
            Assert.IsType<ZLinkStoreWriteResult.Applied>(
                await store.WriteAsync(new ZLinkStoreWriteRequest(
                    [],
                    [
                        new ZLinkStoreMutation.Put(
                            key,
                            new byte[512 * 1024],
                            null)
                    ])));
        }

        var first = Assert.IsType<ZLinkStoreScanResult.Page>(
            await store.ScanAsync(new ZLinkStoreScanRequest(
                "metadata:",
                null,
                1)));
        var cursor = Assert.IsType<ZLinkStoreScanCursor>(
            first.Value.NextCursor);
        var scanId = cursor.Value[..cursor.Value.IndexOf(':')];
        var scanKey =
            $"{keyPrefix}:{{zlink-location-v3}}:opaque:scan:{scanId}";

        await using var connection =
            await ConnectionMultiplexer.ConnectAsync(fixture.ConnectionString);
        var metadata = await connection.GetDatabase().HashGetAllAsync(scanKey);

        Assert.Equal(3, metadata.Length);
        Assert.True(
            metadata.Sum(static item =>
                item.Name.ToString().Length + item.Value.ToString().Length)
            < 4096);
    }

    [SkippableFact]
    public async Task Hot_key_history_is_bounded_without_postponing_snapshot_cleanup()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var store = fixture.CreateStore(out var keyPrefix);
        var firstKey = new ZLinkStoreKey("mvcc:a");
        var hotKey = new ZLinkStoreKey("mvcc:b");
        foreach (var (key, value) in new[]
                 {
                     (firstKey, (byte)1),
                     (hotKey, (byte)2)
                 })
        {
            Assert.IsType<ZLinkStoreWriteResult.Applied>(
                await store.WriteAsync(new ZLinkStoreWriteRequest(
                    [],
                    [new ZLinkStoreMutation.Put(key, new[] { value }, null)])));
        }

        var firstPage = Assert.IsType<ZLinkStoreScanResult.Page>(
            await store.ScanAsync(new ZLinkStoreScanRequest(
                "mvcc:",
                null,
                1)));
        Assert.Equal(firstKey, Assert.Single(firstPage.Value.Items).Key);
        Assert.NotNull(firstPage.Value.NextCursor);

        var current = Assert.IsType<ZLinkStoreReadResult.Found>(
            await store.ReadAsync(hotKey));
        await using var connection =
            await ConnectionMultiplexer.ConnectAsync(fixture.ConnectionString);
        var database = connection.GetDatabase();
        var cleanupKey =
            $"{keyPrefix}:{{zlink-location-v3}}:opaque:cleanup";

        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(new ZLinkStoreWriteRequest(
                [new ZLinkStoreCondition.Version(
                    hotKey,
                    current.Value.Version)],
                [new ZLinkStoreMutation.Put(hotKey, new byte[] { 3 }, null)])));
        var fixedCleanupDue = await database.SortedSetScoreAsync(
            cleanupKey,
            hotKey.Value);
        Assert.NotNull(fixedCleanupDue);

        current = Assert.IsType<ZLinkStoreReadResult.Found>(
            await store.ReadAsync(hotKey));
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(new ZLinkStoreWriteRequest(
                [new ZLinkStoreCondition.Version(
                    hotKey,
                    current.Value.Version)],
                [new ZLinkStoreMutation.Put(hotKey, new byte[] { 4 }, null)])));
        Assert.Equal(
            fixedCleanupDue,
            await database.SortedSetScoreAsync(cleanupKey, hotKey.Value));

        for (var index = 0; index < 125; index++)
        {
            current = Assert.IsType<ZLinkStoreReadResult.Found>(
                await store.ReadAsync(hotKey));
            Assert.IsType<ZLinkStoreWriteResult.Applied>(
                await store.WriteAsync(new ZLinkStoreWriteRequest(
                    [new ZLinkStoreCondition.Version(
                        hotKey,
                        current.Value.Version)],
                    [new ZLinkStoreMutation.Put(
                        hotKey,
                        new[] { (byte)(index & 0xff) },
                        null)])));
        }

        current = Assert.IsType<ZLinkStoreReadResult.Found>(
            await store.ReadAsync(hotKey));
        await Assert.ThrowsAsync<IOException>(
            () => store.WriteAsync(new ZLinkStoreWriteRequest(
                [new ZLinkStoreCondition.Version(
                    hotKey,
                    current.Value.Version)],
                [new ZLinkStoreMutation.Put(
                    hotKey,
                    new byte[] { 0xff },
                    null)])).AsTask());

        var finalPage = Assert.IsType<ZLinkStoreScanResult.Page>(
            await store.ScanAsync(new ZLinkStoreScanRequest(
                "mvcc:",
                firstPage.Value.NextCursor,
                1)));
        var snapshotItem = Assert.Single(finalPage.Value.Items);
        Assert.Equal(hotKey, snapshotItem.Key);
        Assert.Equal(new byte[] { 2 }, snapshotItem.Value.Bytes.ToArray());
        Assert.Null(finalPage.Value.NextCursor);

        var snapshotBoundaryKey =
            $"{keyPrefix}:{{zlink-location-v3}}:opaque:snapshot-boundary";
        Assert.Equal(
            0,
            await database.SortedSetLengthAsync(snapshotBoundaryKey));
        var cleanupDue = Assert.IsType<double>(
            await database.SortedSetScoreAsync(cleanupKey, hotKey.Value));
        var cleanupDelay = Math.Max(
            0,
            (long)Math.Ceiling(
                cleanupDue - DateTimeOffset.UtcNow.ToUnixTimeMilliseconds())
            + 100);
        await Task.Delay(TimeSpan.FromMilliseconds(cleanupDelay));
        current = Assert.IsType<ZLinkStoreReadResult.Found>(
            await store.ReadAsync(hotKey));
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(new ZLinkStoreWriteRequest(
                [new ZLinkStoreCondition.Version(
                    hotKey,
                    current.Value.Version)],
                [new ZLinkStoreMutation.Put(
                    hotKey,
                    new byte[] { 5 },
                    null)])));

        var digest = Convert.ToHexString(
                SHA256.HashData(Encoding.UTF8.GetBytes(hotKey.Value)))
            .ToLowerInvariant();
        var recordKey =
            $"{keyPrefix}:{{zlink-location-v3}}:opaque:{digest}";
        Assert.InRange(
            await database.SortedSetLengthAsync(recordKey),
            1,
            2);
    }

    [SkippableFact]
    public async Task Hot_key_without_snapshot_accepts_rapid_updates()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var store = fixture.CreateStore();
        var key = new ZLinkStoreKey("hot:capacity");

        var current = Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(new ZLinkStoreWriteRequest(
                [new ZLinkStoreCondition.Missing(key)],
                [new ZLinkStoreMutation.Put(key, new byte[] { 0 }, null)])));
        var version = current.PutVersions[key];

        for (var index = 1; index <= 256; index++)
        {
            current = Assert.IsType<ZLinkStoreWriteResult.Applied>(
                await store.WriteAsync(new ZLinkStoreWriteRequest(
                    [new ZLinkStoreCondition.Version(key, version)],
                    [new ZLinkStoreMutation.Put(
                        key,
                        new[] { (byte)index },
                        null)])));
            version = current.PutVersions[key];
        }

        var found = Assert.IsType<ZLinkStoreReadResult.Found>(
            await store.ReadAsync(key));
        Assert.Equal(new byte[] { 0 }, found.Value.Bytes.ToArray());
    }

    [SkippableFact]
    public async Task Conflict_does_not_apply_any_mutation_or_issue_a_version()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var store = fixture.CreateStore();
        var guard = new ZLinkStoreKey("batch:guard");
        var target = new ZLinkStoreKey("batch:target");
        var initial = Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(new ZLinkStoreWriteRequest(
                [new ZLinkStoreCondition.Missing(guard)],
                [new ZLinkStoreMutation.Put(guard, new byte[] { 1 }, null)])));

        var conflict = Assert.IsType<ZLinkStoreWriteResult.Conflict>(
            await store.WriteAsync(new ZLinkStoreWriteRequest(
                [new ZLinkStoreCondition.Missing(guard)],
                [new ZLinkStoreMutation.Put(target, new byte[] { 2 }, null)])));

        Assert.True(conflict.StoreNow >= initial.StoreNow);
        Assert.IsType<ZLinkStoreReadResult.Missing>(
            await store.ReadAsync(target));
        var unchanged = Assert.IsType<ZLinkStoreReadResult.Found>(
            await store.ReadAsync(guard));
        Assert.Equal(
            initial.PutVersions[guard],
            unchanged.Value.Version);
    }

    [SkippableFact]
    public async Task Expiring_and_durable_values_follow_provider_time()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var store = fixture.CreateStore();
        var expiring = new ZLinkStoreKey("retention:short");
        var durable = new ZLinkStoreKey("retention:durable");
        var applied = Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(new ZLinkStoreWriteRequest(
                [],
                [
                    new ZLinkStoreMutation.Put(
                        expiring,
                        new byte[] { 1 },
                        TimeSpan.FromMilliseconds(150)),
                    new ZLinkStoreMutation.Put(
                        durable,
                        new byte[] { 2 },
                        null)
                ])));
        var beforeExpiry = Assert.IsType<ZLinkStoreReadResult.Found>(
            await store.ReadAsync(expiring));

        Assert.NotNull(beforeExpiry.Value.ExpiresAt);
        Assert.True(beforeExpiry.Value.ExpiresAt > applied.StoreNow);
        Assert.Null(Assert.IsType<ZLinkStoreReadResult.Found>(
            await store.ReadAsync(durable)).Value.ExpiresAt);

        await Task.Delay(250);
        Assert.IsType<ZLinkStoreReadResult.Missing>(
            await store.ReadAsync(expiring));
        Assert.IsType<ZLinkStoreReadResult.Found>(
            await store.ReadAsync(durable));
    }

    [SkippableFact]
    public async Task Maximum_unique_key_batch_is_committed_atomically()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var store = fixture.CreateStore();
        var mutations = Enumerable.Range(0, 2048)
            .Select(index => (ZLinkStoreMutation)new ZLinkStoreMutation.Put(
                new ZLinkStoreKey($"bound:{index:D4}"),
                new byte[] { (byte)(index & 0xff) },
                null))
            .ToArray();

        var result = Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(new ZLinkStoreWriteRequest([], mutations)));

        Assert.Equal(2048, result.PutVersions.Count);
        var page = Assert.IsType<ZLinkStoreScanResult.Page>(
            await store.ScanAsync(new ZLinkStoreScanRequest("bound:", null, 1000)));
        Assert.Equal(1000, page.Value.Items.Count);
        Assert.NotNull(page.Value.NextCursor);
    }

    [Fact]
    public async Task Provider_rejects_contract_bounds_before_connecting()
    {
        await using var store = new ZLinkRedisLocationStore(
            new ZLinkRedisLocationOptions
            {
                ConnectionString = "unused:6379",
                KeyPrefix = "zlink:bounds"
            },
            _ => throw new InvalidOperationException(
                "Validation must finish before Redis I/O."));

        await Assert.ThrowsAsync<ArgumentException>(
            () => store.ReadAsync(new ZLinkStoreKey(string.Empty)).AsTask());
        await Assert.ThrowsAsync<ArgumentException>(
            () => store.WriteAsync(new ZLinkStoreWriteRequest(
                [],
                [
                    new ZLinkStoreMutation.Put(
                        new ZLinkStoreKey("too-large"),
                        new byte[(1024 * 1024) + 1],
                        null)
                ])).AsTask());
        await Assert.ThrowsAsync<ArgumentException>(
            () => store.WriteAsync(new ZLinkStoreWriteRequest(
                [],
                Enumerable.Range(0, 2049)
                    .Select(index => (ZLinkStoreMutation)
                        new ZLinkStoreMutation.Delete(
                            new ZLinkStoreKey($"too-many:{index}")))
                    .ToArray())).AsTask());
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(
            () => store.ScanAsync(new ZLinkStoreScanRequest("", null, 1001))
                .AsTask());
    }
}
