using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using StackExchange.Redis;
using Zlink.Framework.Runtime.Locations;

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
        // Bound raised from 1 MiB to 2 MiB (checklist C-2b): the collapsed
        // authority row embeds its base64 payload inline, so a maximum-size
        // (1 MiB, spec §6) payload plus JSON/base64 overhead must still fit
        // in one opaque record value.
        await Assert.ThrowsAsync<ArgumentException>(
            () => store.WriteAsync(new ZLinkStoreWriteRequest(
                [],
                [
                    new ZLinkStoreMutation.Put(
                        new ZLinkStoreKey("too-large"),
                        new byte[(2 * 1024 * 1024) + 1],
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

    /// <summary>
    /// Checklist C-4 (dotnet store convergence) / C-4c: an encode-side
    /// conformance test that drives the real Lua write path (not a pure
    /// function) and inspects the raw ZSET member Redis stored, so a
    /// regression in the format-tag byte, the expiresAtMs sentinel
    /// (0 = no expiry, not -1 -- 22-location-store-redis.md#7 pins
    /// expiresAtMs as an *unsigned* MessagePack int), or the tombstone type
    /// (MessagePack bool 0xc2/0xc3, not an integer 0/1) fails here against
    /// the shared golden fixture's encoding rules
    /// (framework/runtime/protocol/golden/store-record-v1.json).
    /// </summary>
    [SkippableFact]
    public async Task Opaque_record_wire_bytes_use_the_golden_tag_expiry_and_tombstone_encoding()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var store = fixture.CreateStore(out var keyPrefix);
        var key = new ZLinkStoreKey("wire-format:probe");
        var recordKey =
            $"{keyPrefix}:{{zlink-location-v3}}:opaque:{Sha256Hex(key.Value)}";

        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(new ZLinkStoreWriteRequest(
                [new ZLinkStoreCondition.Missing(key)],
                [new ZLinkStoreMutation.Put(
                    key,
                    Encoding.UTF8.GetBytes("wire-format-value"),
                    null)])));

        await using var connection =
            await ConnectionMultiplexer.ConnectAsync(fixture.ConnectionString);
        var database = connection.GetDatabase();
        var putEntries = await database.SortedSetRangeByScoreWithScoresAsync(
            recordKey);
        var putMember = (byte[])Assert.Single(putEntries).Element!;
        Assert.Equal(0x01, putMember[0]);
        var (putOriginalKey, putRawBytes, putVersion, putExpiresAt, putTombstone) =
            DecodeOpaqueMember(putMember, 1);
        Assert.Equal(key.Value, putOriginalKey);
        Assert.Equal("wire-format-value", Encoding.UTF8.GetString(putRawBytes));
        Assert.False(string.IsNullOrEmpty(putVersion));
        Assert.Equal(0UL, putExpiresAt);
        Assert.False(putTombstone);

        var current = Assert.IsType<ZLinkStoreReadResult.Found>(
            await store.ReadAsync(key));
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(new ZLinkStoreWriteRequest(
                [new ZLinkStoreCondition.Version(
                    key,
                    current.Value.Version)],
                [new ZLinkStoreMutation.Delete(key)])));

        var deleteEntries = await database.SortedSetRangeByScoreWithScoresAsync(
            recordKey);
        var deleteMember = deleteEntries
            .OrderByDescending(static entry => entry.Score)
            .First()
            .Element;
        var deleteBytes = (byte[])deleteMember!;
        Assert.Equal(0x01, deleteBytes[0]);
        var (deleteOriginalKey, deleteRawBytes, deleteVersion, deleteExpiresAt, deleteTombstone) =
            DecodeOpaqueMember(deleteBytes, 1);
        Assert.Equal(key.Value, deleteOriginalKey);
        Assert.Empty(deleteRawBytes);
        // The tombstone still carries a real, freshly issued version (the
        // shared golden fixture's ownerLease-tombstone vector pins a
        // non-empty version) -- distinct from the version the Put above
        // received.
        Assert.False(string.IsNullOrEmpty(deleteVersion));
        Assert.NotEqual(putVersion, deleteVersion);
        Assert.Equal(0UL, deleteExpiresAt);
        Assert.True(deleteTombstone);
    }

    /// <summary>
    /// Checklist C-4 / H-class (authority single-row collapse): drives a
    /// real Reserve -> CompleteCreation through the production
    /// ZLinkProviderLocationRepository against live Redis, then inspects
    /// the raw stored bytes to prove (a) the authority row is exactly one
    /// opaque key -- no separate payload or generation key survives the
    /// collapse (checklist C-2b) -- and (b) its JSON envelope uses the
    /// canonical field names/types pinned by 21-location-runtime.md#2.4
    /// and the updated store-record-v1.json authority-actor-normal vector
    /// (payload as base64, objectGeneration/authorityOwnerGeneration/
    /// ownerLeaseGeneration as decimal strings, allocation.state/
    /// objectKind as lowercase strings, allocation.descriptor.
    /// routingIdHex, allocation.descriptorLifecycleGeneration as a
    /// string). The exact generation numbers aren't asserted against the
    /// golden's "7"/"3" -- those are Store-wide sequence values the golden
    /// fixture picked arbitrarily, not reproducible on demand from a
    /// fresh Reserve.
    /// </summary>
    [SkippableFact]
    public async Task Authority_row_is_a_single_opaque_key_using_the_canonical_envelope()
    {
        Skip.IfNot(fixture.RedisAvailable, fixture.SkipReason);
        await using var store = fixture.CreateStore(out var keyPrefix);
        var repository = new ZLinkProviderLocationRepository(store);

        var owner = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await repository.ClaimOwnerLeaseAsync(
                "authority-envelope-owner",
                TimeSpan.FromMinutes(2))).Token;
        var descriptor = new ZLinkMeshNodeDescriptor(
            "main",
            RoutingId.From("authority-envelope-node"),
            1,
            1,
            "tcp://127.0.0.1:7301",
            new Dictionary<string, int>(StringComparer.Ordinal),
            string.Empty,
            owner.OwnerId,
            owner.LeaseGeneration,
            default)
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            ObjectCapabilities =
            [
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.Actor,
                    "player",
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    0)
            ],
            State = ZLinkFrameworkRuntimeState.Serving
        };
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await repository.UpdateMeshNodeAsync(
                descriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);

        var authorityKey = ZLinkAuthorityKeyCodec.EncodeActor(
            "authority-envelope-actor");
        var intent = "create:authority-envelope-actor"u8.ToArray();
        var request = new ZLinkObjectReservationRequest(
            ZLinkPlacementObjectKind.Actor,
            authorityKey,
            "player",
            "inline:authority-envelope-actor",
            SHA256.HashData(intent),
            intent.Length,
            new ZLinkMeshNodeDescriptorKey(descriptor.MeshName, descriptor.Rid),
            descriptor.LifecycleGeneration,
            owner,
            intent,
            new ZLinkCapacityVector(1, 0, null));
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(request));
        var readyPayload = new byte[] { 0xDE, 0xAD, 0xBE, 0xEF };
        Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await repository.CommitAsync(
                reserved.Reservation,
                readyPayload));

        // Single-row proof: enumerate the Redis keyspace under this test's
        // isolated prefix and confirm exactly one opaque record key exists
        // for this authority row's identity -- not a meta+payload(+
        // generation) triple.
        await using var connection =
            await ConnectionMultiplexer.ConnectAsync(fixture.ConnectionString);
        var database = connection.GetDatabase();
        var server = connection.GetServer(connection.GetEndPoints()[0]);
        var recordKeys = server
            .Keys(pattern: $"{keyPrefix}:{{zlink-location-v3}}:opaque:*")
            .Where(key => !key.ToString().Contains(":opaque:index", StringComparison.Ordinal)
                          && !key.ToString().Contains(":opaque:map", StringComparison.Ordinal)
                          && !key.ToString().Contains(":opaque:cleanup", StringComparison.Ordinal)
                          && !key.ToString().Contains(":opaque:sequence", StringComparison.Ordinal)
                          && !key.ToString().Contains(":opaque:snapshot", StringComparison.Ordinal)
                          && !key.ToString().Contains(":opaque:scan", StringComparison.Ordinal))
            .ToArray();

        JsonElement? authorityJson = null;
        var matchingKeyCount = 0;
        foreach (var recordKey in recordKeys)
        {
            var members = await database.SortedSetRangeByScoreWithScoresAsync(
                recordKey);
            var latest = members.OrderByDescending(static m => m.Score).First();
            var bytes = (byte[])latest.Element!;
            if (bytes[0] != 0x01) continue;
            var offset = 1;
            _ = ReadArrayHead(bytes, ref offset);
            var originalKey = System.Text.Encoding.UTF8.GetString(
                ReadStr(bytes, ref offset));
            if (!originalKey.Contains(
                    "authority-envelope-actor",
                    StringComparison.Ordinal))
                continue;
            matchingKeyCount++;
            var rawBytes = ReadStr(bytes, ref offset);
            using var parsed = JsonDocument.Parse(rawBytes);
            authorityJson = parsed.RootElement.Clone();
        }

        // Single-row proof (checklist C-2b): in the old split scheme, the
        // meta, payload, and generation keys all embedded the actor id in
        // their original-key string, so this count would have been 3.
        // Collapsed to one opaque row, it's 1.
        Assert.Equal(1, matchingKeyCount);
        Assert.NotNull(authorityJson);
        var json = authorityJson!.Value;
        Assert.Equal(1, json.GetProperty("recordVersion").GetInt32());
        Assert.Equal(
            Convert.ToBase64String(readyPayload),
            json.GetProperty("payload").GetString());
        Assert.Matches("^[0-9]+$", json.GetProperty("objectGeneration").GetString()!);
        Assert.Matches(
            "^[0-9]+$",
            json.GetProperty("authorityOwnerGeneration").GetString()!);
        Assert.Equal(owner.OwnerId, json.GetProperty("ownerId").GetString());
        Assert.Equal(
            owner.LeaseGeneration.ToString(),
            json.GetProperty("ownerLeaseGeneration").GetString());
        var allocation = json.GetProperty("allocation");
        Assert.Equal("active", allocation.GetProperty("state").GetString());
        Assert.Equal("actor", allocation.GetProperty("objectKind").GetString());
        Assert.Equal("player", allocation.GetProperty("stableType").GetString());
        Assert.Equal(
            "main",
            allocation.GetProperty("descriptor").GetProperty("meshName")
                .GetString());
        Assert.Equal(
            RoutingId.From("authority-envelope-node").ToHex(),
            allocation.GetProperty("descriptor").GetProperty("routingIdHex")
                .GetString());
        Assert.Equal(
            "1",
            allocation.GetProperty("descriptorLifecycleGeneration")
                .GetString());
        var capacity = allocation.GetProperty("capacity");
        Assert.Equal(1, capacity.GetProperty("actors").GetInt32());
        Assert.Equal(0, capacity.GetProperty("spots").GetInt32());
        Assert.Equal(
            JsonValueKind.Null,
            capacity.GetProperty("spotType").ValueKind);
        Assert.Equal(
            JsonValueKind.Null,
            json.GetProperty("pendingCreation").ValueKind);
    }

    private static string Sha256Hex(string value) =>
        Convert.ToHexString(
                SHA256.HashData(Encoding.UTF8.GetBytes(value)))
            .ToLowerInvariant();

    private static (string OriginalKey, byte[] RawBytes, string Version, ulong ExpiresAt, bool Tombstone)
        DecodeOpaqueMember(byte[] bytes, int offset)
    {
        var count = ReadArrayHead(bytes, ref offset);
        if (count != 5)
            throw new InvalidDataException($"invalid opaque member arity: {count}");
        var originalKey = Encoding.UTF8.GetString(ReadStr(bytes, ref offset));
        var rawBytes = ReadStr(bytes, ref offset);
        var version = Encoding.UTF8.GetString(ReadStr(bytes, ref offset));
        var expiresAt = ReadUint(bytes, ref offset);
        var tombstone = ReadBool(bytes, ref offset);
        return (originalKey, rawBytes, version, expiresAt, tombstone);
    }

    private static byte NextByte(byte[] bytes, ref int offset) => bytes[offset++];

    private static int ReadArrayHead(byte[] bytes, ref int offset)
    {
        var tag = NextByte(bytes, ref offset);
        if ((tag & 0xf0) == 0x90) return tag & 0x0f;
        throw new InvalidDataException($"invalid msgpack array tag: {tag}");
    }

    private static byte[] ReadStr(byte[] bytes, ref int offset)
    {
        var tag = NextByte(bytes, ref offset);
        int length;
        if ((tag & 0xe0) == 0xa0) length = tag & 0x1f;
        else if (tag == 0xd9) length = NextByte(bytes, ref offset);
        else if (tag == 0xda)
            length = (NextByte(bytes, ref offset) << 8) | NextByte(bytes, ref offset);
        else
            throw new InvalidDataException($"invalid msgpack str tag: {tag}");
        var value = bytes[offset..(offset + length)];
        offset += length;
        return value;
    }

    private static ulong ReadUint(byte[] bytes, ref int offset)
    {
        var tag = NextByte(bytes, ref offset);
        if ((tag & 0x80) == 0) return tag;
        if (tag == 0xcc) return NextByte(bytes, ref offset);
        if (tag == 0xcd)
            return (ulong)((NextByte(bytes, ref offset) << 8) | NextByte(bytes, ref offset));
        if (tag == 0xce)
        {
            ulong v = 0;
            for (var i = 0; i < 4; i++) v = (v << 8) | NextByte(bytes, ref offset);
            return v;
        }
        if (tag == 0xcf)
        {
            ulong v = 0;
            for (var i = 0; i < 8; i++) v = (v << 8) | NextByte(bytes, ref offset);
            return v;
        }
        throw new InvalidDataException($"invalid msgpack uint tag: {tag}");
    }

    private static bool ReadBool(byte[] bytes, ref int offset)
    {
        var tag = NextByte(bytes, ref offset);
        if (tag == 0xc2) return false;
        if (tag == 0xc3) return true;
        throw new InvalidDataException($"invalid msgpack bool tag: {tag}");
    }
}
