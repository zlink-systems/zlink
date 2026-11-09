using Etcdserverpb;
using Grpc.Core;

namespace Zlink.Framework.Locations.Etcd.Tests;

public sealed class EtcdProviderTests
{
    [Fact]
    public async Task Computed_expiry_above_the_shared_time_cap_is_a_caller_error_without_a_commit()
    {
        await using var server = await FakeEtcdServer.StartAsync();
        server.Clock((ulong)EtcdEncoding.MaxTime - 1);
        await using var store = server.CreateStore();
        await Assert.ThrowsAnyAsync<ArgumentException>(() =>
            store
                .WriteAsync(
                    new(
                        [],
                        [
                            new ZLinkStoreMutation.Put(
                                new("key"),
                                new byte[] { 1 },
                                TimeSpan.FromMilliseconds(2)
                            ),
                        ]
                    )
                )
                .AsTask()
        );
        Assert.Equal(0, server.WriteCount);
        Assert.Single(server.Calls);
    }

    [Fact]
    public void Public_surface_exports_only_the_approved_store_and_options()
    {
        Assert.Equal(
            new[]
            {
                "Zlink.Framework.Locations.Etcd.ZLinkEtcdClientIdentity",
                "Zlink.Framework.Locations.Etcd.ZLinkEtcdCredentials",
                "Zlink.Framework.Locations.Etcd.ZLinkEtcdLocationOptions",
                "Zlink.Framework.Locations.Etcd.ZLinkEtcdLocationStore",
                "Zlink.Framework.Locations.Etcd.ZLinkEtcdTlsOptions",
            },
            typeof(ZLinkEtcdLocationStore)
                .Assembly.GetExportedTypes()
                .Select(type => type.FullName)
                .Order(StringComparer.Ordinal)
        );
    }

    private static ZLinkStoreWriteRequest Put(
        string key,
        byte value,
        params ZLinkStoreCondition[] conditions
    ) => new(conditions, [new ZLinkStoreMutation.Put(new(key), new byte[] { value }, null)]);

    [Theory]
    [InlineData(StatusCode.OutOfRange, "required revision has been compacted", "Compaction")]
    [InlineData(StatusCode.ResourceExhausted, "database space exceeded", "NoSpace")]
    [InlineData(StatusCode.Unauthenticated, "invalid auth token", "Authentication")]
    [InlineData(StatusCode.InvalidArgument, "revision of auth store is old", "Authentication")]
    [InlineData(StatusCode.OutOfRange, "other error", "StoreFailure")]
    [InlineData(StatusCode.Internal, "required revision has been compacted", "StoreFailure")]
    [InlineData(StatusCode.Unauthenticated, "permission denied", "StoreFailure")]
    [InlineData(StatusCode.PermissionDenied, "invalid auth token", "StoreFailure")]
    public void Classification_requires_both_code_and_description(
        StatusCode code,
        string description,
        string expected
    ) => Assert.Equal(expected, EtcdClient.Classify(code, description).ToString());

    [Theory]
    [InlineData(StatusCode.Unauthenticated, "etcdserver: invalid auth token")]
    [InlineData(StatusCode.InvalidArgument, "etcdserver: revision of auth store is old")]
    public async Task Authentication_rejection_reauthenticates_once_with_the_same_deadline(
        StatusCode code,
        string description
    )
    {
        await using var server = await FakeEtcdServer.StartAsync();
        server.Clock();
        var attempts = 0;
        server.BeforeCall = (method, _, context) =>
        {
            if (method == "Txn")
            {
                Assert.Equal(
                    attempts == 0 ? "token1" : "token2",
                    context.RequestHeaders.GetValue("token")
                );
                if (Interlocked.Increment(ref attempts) == 1)
                    throw new RpcException(new Status(code, description));
            }
            return method == "Authenticate" ? Task.Delay(100) : Task.CompletedTask;
        };
        await using var store = server.CreateStore(
            credentials: new() { User = "user", Password = "password" }
        );
        Assert.IsType<ZLinkStoreReadResult.Missing>(await store.ReadAsync(new("absent")));
        Assert.Equal(2, server.AuthenticateCount);
        Assert.Equal(2, attempts);
        AssertSharedDeadline(server);
    }

    [Fact]
    public async Task A_second_authentication_rejection_is_not_retried()
    {
        await using var server = await FakeEtcdServer.StartAsync();
        server.Clock();
        server.BeforeCall = (method, _, _) =>
            method == "Txn"
                ? throw new RpcException(
                    new Status(StatusCode.Unauthenticated, "etcdserver: invalid auth token")
                )
                : Task.CompletedTask;
        await using var store = server.CreateStore(
            credentials: new() { User = "user", Password = "password" }
        );
        await Assert.ThrowsAnyAsync<IOException>(() => store.ReadAsync(new("absent")).AsTask());
        Assert.Equal(2, server.AuthenticateCount);
        Assert.Equal(2, server.Calls.Count(call => call.Method == "Txn"));
    }

    [Theory]
    [InlineData(StatusCode.Unavailable, "etcdserver: unavailable")]
    [InlineData(StatusCode.ResourceExhausted, "etcdserver: mvcc: database space exceeded")]
    public async Task A_write_applied_before_an_error_is_not_replayed(
        StatusCode code,
        string description
    )
    {
        await using var server = await FakeEtcdServer.StartAsync();
        server.Clock();
        server.BeforeCall = (method, request, _) =>
        {
            if (
                method == "Txn"
                && request is TxnRequest transaction
                && transaction.Success.Any(op => op.RequestPut is not null)
            )
            {
                server.Apply(transaction);
                throw new RpcException(new Status(code, description));
            }
            return Task.CompletedTask;
        };
        await using var store = server.CreateStore();
        await Assert.ThrowsAnyAsync<IOException>(() =>
            store.WriteAsync(Put("written", 7)).AsTask()
        );
        Assert.Equal(1, server.WriteCount);
        server.BeforeCall = null;
        Assert.Equal(
            new byte[] { 7 },
            Assert
                .IsType<ZLinkStoreReadResult.Found>(await store.ReadAsync(new("written")))
                .Value.Bytes.ToArray()
        );
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Unavailable_or_deadline_rotates_only_the_next_operation(bool noResponse)
    {
        await using var first = await FakeEtcdServer.StartAsync();
        await using var second = await FakeEtcdServer.StartAsync();
        second.Clock();
        first.BeforeCall = async (_, _, context) =>
        {
            if (noResponse)
                await Task.Delay(Timeout.Infinite, context.CancellationToken);
            throw new RpcException(new Status(StatusCode.Unavailable, "unavailable"));
        };
        await using var store = first.CreateStore(
            timeout: TimeSpan.FromMilliseconds(300),
            followingEndpoints: [second.Endpoint]
        );
        await Assert.ThrowsAnyAsync<IOException>(() => store.ReadAsync(new("absent")).AsTask());
        Assert.Empty(second.Calls);
        Assert.IsType<ZLinkStoreReadResult.Missing>(await store.ReadAsync(new("absent")));
        Assert.Single(first.Calls);
        Assert.Single(second.Calls);
    }

    [Fact]
    public async Task Compacted_value_decision_restarts_at_a_new_revision()
    {
        await using var server = await FakeEtcdServer.StartAsync();
        server.Clock();
        server.Record("condition", [1]);
        var compacted = false;
        server.BeforeCall = (_, request, _) =>
        {
            var isPinnedRead =
                request is RangeRequest range && range.Revision > 0
                || request is TxnRequest txn
                    && txn.Success.Any(op => op.RequestRange?.Revision > 0);
            if (isPinnedRead && !compacted)
            {
                compacted = true;
                server.CompactedRevision = server.Revision;
                server.Record("condition", [2]);
                throw new RpcException(
                    new Status(
                        StatusCode.OutOfRange,
                        "etcdserver: mvcc: required revision has been compacted"
                    )
                );
            }
            return Task.CompletedTask;
        };
        await using var store = server.CreateStore();
        Assert.IsType<ZLinkStoreWriteResult.Conflict>(
            await store.WriteAsync(
                Put("target", 9, new ZLinkStoreCondition.Value(new("condition"), new byte[] { 1 }))
            )
        );
        Assert.True(compacted);
        Assert.Equal(0, server.WriteCount);
        AssertSharedDeadline(server);
        Assert.IsType<ZLinkStoreReadResult.Missing>(await store.ReadAsync(new("target")));
    }

    [Fact]
    public async Task Apply_comparison_failure_redecides_instead_of_returning_conflict()
    {
        await using var server = await FakeEtcdServer.StartAsync();
        server.Clock();
        server.Record("condition", [1]);
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
                server.Record("condition", [1]);
            }
            return Task.CompletedTask;
        };
        await using var store = server.CreateStore();
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(
                Put("target", 9, new ZLinkStoreCondition.Value(new("condition"), new byte[] { 1 }))
            )
        );
        Assert.True(changed);
        Assert.Equal(1, server.WriteCount);
        AssertSharedDeadline(server);
    }

    [Fact]
    public async Task Expiry_between_decision_and_apply_is_redecided_as_conflict()
    {
        await using var server = await FakeEtcdServer.StartAsync();
        server.Clock();
        server.Record("condition", [1], 1001);
        await using var store = server.CreateStore();
        var version = Assert
            .IsType<ZLinkStoreReadResult.Found>(await store.ReadAsync(new("condition")))
            .Value.Version;
        server.BeforeCall = (_, request, _) =>
        {
            if (request is TxnRequest txn && txn.Success.Any(op => op.RequestPut is not null))
                server.Clock(1001);
            return Task.CompletedTask;
        };
        Assert.IsType<ZLinkStoreWriteResult.Conflict>(
            await store.WriteAsync(
                Put("target", 9, new ZLinkStoreCondition.Version(new("condition"), version))
            )
        );
        Assert.Equal(0, server.WriteCount);
    }

    [Fact]
    public async Task Cancellation_before_io_does_not_call_the_server()
    {
        await using var server = await FakeEtcdServer.StartAsync();
        await using var store = server.CreateStore();
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            store.ReadAsync(new("key"), cancellation.Token).AsTask()
        );
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            store.WriteAsync(Put("key", 1), cancellation.Token).AsTask()
        );
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            store.ScanAsync(new("", null, 1), cancellation.Token).AsTask()
        );
        Assert.Empty(server.Calls);
    }

    [Fact]
    public async Task Cancellation_after_io_cancels_the_rpc_and_returns_store_failure()
    {
        await using var server = await FakeEtcdServer.StartAsync();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var cancelled = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        server.BeforeCall = async (_, _, context) =>
        {
            entered.SetResult();
            try
            {
                await Task.Delay(Timeout.Infinite, context.CancellationToken);
            }
            catch (OperationCanceledException)
            {
                cancelled.SetResult();
                throw;
            }
        };
        await using var store = server.CreateStore();
        using var cancellation = new CancellationTokenSource();
        var operation = store.ReadAsync(new("key"), cancellation.Token).AsTask();
        await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<IOException>(() => operation);
        await cancelled.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Single(server.Calls);
    }

    [Fact]
    public async Task Dispose_rejects_new_work_and_waits_for_existing_work()
    {
        await using var server = await FakeEtcdServer.StartAsync();
        server.Clock();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        server.BeforeCall = async (_, _, _) =>
        {
            entered.TrySetResult();
            await release.Task;
        };
        var store = server.CreateStore();
        var operation = store.ReadAsync(new("key")).AsTask();
        await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var dispose = store.DisposeAsync().AsTask();
        var duplicate = store.DisposeAsync().AsTask();
        Assert.False(dispose.IsCompleted);
        Assert.False(duplicate.IsCompleted);
        await Assert.ThrowsAsync<ObjectDisposedException>(() =>
            store.ReadAsync(new("new")).AsTask()
        );
        release.SetResult();
        Assert.IsType<ZLinkStoreReadResult.Missing>(await operation);
        await Task.WhenAll(dispose, duplicate);
        await store.DisposeAsync();
    }

    [Fact]
    public async Task Invalid_inputs_and_options_fail_before_io()
    {
        await using var server = await FakeEtcdServer.StartAsync();
        await using var store = server.CreateStore();
        await Assert.ThrowsAnyAsync<ArgumentException>(() => store.ReadAsync(new("")).AsTask());
        await Assert.ThrowsAnyAsync<ArgumentException>(() =>
            store.ReadAsync(new(new string('k', 1025))).AsTask()
        );
        await Assert.ThrowsAnyAsync<ArgumentException>(() =>
            store.ScanAsync(new("", null, 1001)).AsTask()
        );
        await Assert.ThrowsAnyAsync<ArgumentException>(() =>
            store.ScanAsync(new("", new(new string('c', 4097)), 1)).AsTask()
        );
        await Assert.ThrowsAnyAsync<ArgumentException>(() =>
            store
                .WriteAsync(
                    new([], [new ZLinkStoreMutation.Put(new("key"), new byte[1048577], null)])
                )
                .AsTask()
        );
        await Assert.ThrowsAnyAsync<ArgumentException>(() =>
            store
                .WriteAsync(
                    new([], [new ZLinkStoreMutation.Put(new("key"), new byte[1], TimeSpan.Zero)])
                )
                .AsTask()
        );
        await Assert.ThrowsAnyAsync<ArgumentException>(() =>
            store
                .WriteAsync(
                    new(
                        [],
                        [
                            new ZLinkStoreMutation.Delete(new("key")),
                            new ZLinkStoreMutation.Delete(new("key")),
                        ]
                    )
                )
                .AsTask()
        );
        await Assert.ThrowsAnyAsync<ArgumentException>(() =>
            store
                .WriteAsync(
                    new(
                        [],
                        Enumerable
                            .Range(0, 2049)
                            .Select(i =>
                                (ZLinkStoreMutation)new ZLinkStoreMutation.Delete(new("k" + i))
                            )
                            .ToArray()
                    )
                )
                .AsTask()
        );
        foreach (
            var endpoint in new[]
            {
                "ftp://localhost",
                "http://user:password@localhost",
                "http://localhost/path",
                "http://localhost?query=yes",
            }
        )
            Assert.ThrowsAny<ArgumentException>(() =>
                new ZLinkEtcdLocationStore(o =>
                {
                    o.Namespace = "test";
                    o.Endpoints.Add(endpoint);
                })
            );
        Assert.ThrowsAny<ArgumentException>(() =>
            new ZLinkEtcdLocationStore(o =>
            {
                o.Namespace = "test";
                o.Endpoints.Add(server.Endpoint);
                o.Tls = new();
            })
        );
        Assert.ThrowsAny<ArgumentException>(() =>
            new ZLinkEtcdLocationStore(o =>
            {
                o.Namespace = "test";
                o.Endpoints.Add("https://localhost");
                o.Tls = new() { TrustedCaPem = "invalid PEM" };
            })
        );
        Assert.Empty(server.Calls);
    }

    [Fact]
    public async Task Missing_store_clock_fails_all_operations_instead_of_returning_missing_or_conflict()
    {
        await using var server = await FakeEtcdServer.StartAsync();
        await using var store = server.CreateStore();
        await Assert.ThrowsAnyAsync<IOException>(() => store.ReadAsync(new("key")).AsTask());
        await Assert.ThrowsAnyAsync<IOException>(() => store.WriteAsync(Put("key", 1)).AsTask());
        await Assert.ThrowsAnyAsync<IOException>(() => store.ScanAsync(new("", null, 1)).AsTask());
        Assert.Equal(0, server.WriteCount);
    }

    [Fact]
    public async Task Corrupt_condition_metadata_and_missing_scan_payload_are_store_failures()
    {
        await using var server = await FakeEtcdServer.StartAsync();
        server.Clock();
        var metadata = new byte[14];
        metadata[0] = 2;
        server.Seed(FakeEtcdServer.Key("test", "m", "bad"), metadata);
        await using var store = server.CreateStore();
        await Assert.ThrowsAnyAsync<IOException>(() =>
            store.WriteAsync(Put("target", 1, new ZLinkStoreCondition.Missing(new("bad")))).AsTask()
        );
        Assert.Equal(0, server.WriteCount);
        metadata[0] = 1;
        server.Seed(FakeEtcdServer.Key("test", "m", "bad"), metadata);
        await Assert.ThrowsAnyAsync<IOException>(() =>
            store.ScanAsync(new("bad", null, 1)).AsTask()
        );
    }

    [Fact]
    public async Task Scan_keeps_revision_and_store_time_and_rejects_foreign_or_compacted_cursors()
    {
        await using var server = await FakeEtcdServer.StartAsync();
        server.Clock();
        server.Record("scan/a", [1]);
        server.Record("scan/b", [2]);
        await using var store = server.CreateStore();
        var first = Assert
            .IsType<ZLinkStoreScanResult.Page>(await store.ScanAsync(new("scan/", null, 1)))
            .Value;
        Assert.NotNull(first.NextCursor);
        server.Clock(2000);
        server.Record("scan/b", [3]);
        var second = Assert
            .IsType<ZLinkStoreScanResult.Page>(
                await store.ScanAsync(new("scan/", first.NextCursor, 1))
            )
            .Value;
        Assert.Equal(first.StoreNow, second.StoreNow);
        Assert.Equal(new byte[] { 2 }, Assert.Single(second.Items).Value.Bytes.ToArray());
        await using var foreign = server.CreateStore();
        Assert.IsType<ZLinkStoreScanResult.Expired>(
            await foreign.ScanAsync(new("scan/", first.NextCursor, 1))
        );
        Assert.IsType<ZLinkStoreScanResult.Expired>(
            await store.ScanAsync(new("other/", first.NextCursor, 1))
        );
        server.CompactedRevision = server.Revision;
        Assert.IsType<ZLinkStoreScanResult.Expired>(
            await store.ScanAsync(new("scan/", first.NextCursor, 1))
        );
    }

    [Fact]
    public async Task Scan_enforces_encoded_page_bound_and_keeps_the_unconsumed_item_for_the_next_page()
    {
        await using var server = await FakeEtcdServer.StartAsync();
        server.Clock();
        var payload = new byte[1024 * 1024];
        for (var i = 0; i < 4; i++)
            server.Record("large/" + i, payload);
        await using var store = server.CreateStore();
        var first = Assert
            .IsType<ZLinkStoreScanResult.Page>(await store.ScanAsync(new("large/", null, 1000)))
            .Value;
        Assert.Equal(3, first.Items.Count);
        Assert.NotNull(first.NextCursor);
        var second = Assert
            .IsType<ZLinkStoreScanResult.Page>(
                await store.ScanAsync(new("large/", first.NextCursor, 1000))
            )
            .Value;
        Assert.Equal("large/3", Assert.Single(second.Items).Key.Value);
        Assert.Null(second.NextCursor);
        Assert.Equal(first.StoreNow, second.StoreNow);
    }

    [Fact]
    public async Task Compaction_between_scan_metadata_and_payload_returns_expired_without_partial_items()
    {
        await using var server = await FakeEtcdServer.StartAsync();
        server.Clock();
        server.Record("scan/a", [1]);
        server.BeforeCall = (_, request, _) =>
        {
            if (request is TxnRequest txn && txn.Success.Any(op => op.RequestRange?.Revision > 0))
                throw new RpcException(
                    new Status(
                        StatusCode.OutOfRange,
                        "etcdserver: mvcc: required revision has been compacted"
                    )
                );
            return Task.CompletedTask;
        };
        await using var store = server.CreateStore();
        Assert.IsType<ZLinkStoreScanResult.Expired>(
            await store.ScanAsync(new("scan/", null, 1000))
        );
    }

    private static void AssertSharedDeadline(FakeEtcdServer server)
    {
        // The wire carries a relative gRPC timeout, reconstructed against the server clock.
        var deadlines = server.Calls.Select(call => call.Deadline).ToArray();
        Assert.True(deadlines.Max() - deadlines.Min() < TimeSpan.FromMilliseconds(50));
    }
}
