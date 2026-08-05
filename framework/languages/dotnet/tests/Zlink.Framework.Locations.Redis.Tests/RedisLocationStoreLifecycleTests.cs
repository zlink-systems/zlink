using System.Reflection;
using StackExchange.Redis;

namespace Zlink.Framework.Locations.Redis.Tests;

public sealed class RedisLocationStoreLifecycleTests
{
    [Fact]
    public void Operation_timeout_defaults_and_bounds_are_exact()
    {
        Assert.Equal(
            TimeSpan.FromSeconds(5),
            new ZLinkRedisLocationOptions().OperationTimeout);
        Assert.Equal(
            TimeSpan.FromSeconds(5),
            new ZLinkRedisRelocationOptions().OperationTimeout);

        Assert.Throws<ArgumentOutOfRangeException>(
            () => new ZLinkRedisLocationStore(
                new ZLinkRedisLocationOptions
                {
                    ConnectionString = "unused:6379",
                    KeyPrefix = "zlink:invalid-timeout",
                    OperationTimeout = TimeSpan.Zero
                }));
        Assert.Throws<ArgumentOutOfRangeException>(
            () => new ZLinkRedisRelocationStore(
                new ZLinkRedisRelocationOptions
                {
                    ConnectionString = "unused:6379",
                    KeyPrefix = "zlink:invalid-relocation-timeout",
                    OperationTimeout = TimeSpan.FromMilliseconds(-1)
                }));
    }

    [Fact]
    public async Task Operation_timeout_bounds_the_redis_command()
    {
        var database =
            DispatchProxy.Create<IDatabase, BlockingRedisDatabaseProxy>();
        var command = (BlockingRedisDatabaseProxy)(object)database;
        var connection = new TestRedisConnection(database: database);
        var options = new ZLinkRedisLocationOptions
        {
            ConnectionString = "unused:6379",
            KeyPrefix = "zlink:command-timeout",
            OperationTimeout = TimeSpan.FromMilliseconds(25)
        };
        var store = new ZLinkRedisLocationStore(
            options,
            _ => ValueTask.FromResult<IZLinkRedisConnection>(connection));
        options.OperationTimeout = TimeSpan.FromSeconds(5);

        await Assert.ThrowsAsync<TimeoutException>(
            () => store.ReadAsync(new ZLinkStoreKey("timeout:read"))
                .AsTask());

        command.ReleaseCommand.TrySetResult(MissingReadResult());
        await store.DisposeAsync().AsTask().WaitAsync(
            TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task Stores_snapshot_connection_and_key_options_at_construction()
    {
        var locationDatabase =
            DispatchProxy.Create<IDatabase, CapturingRedisDatabaseProxy>();
        var locationCommand =
            (CapturingRedisDatabaseProxy)(object)locationDatabase;
        ConfigurationOptions? locationConfiguration = null;
        var locationOptions = new ZLinkRedisLocationOptions
        {
            ConnectionString = "location-original:6379",
            KeyPrefix = "zlink:location-original"
        };
        await using var locationStore = new ZLinkRedisLocationStore(
            locationOptions,
            configuration =>
            {
                locationConfiguration = configuration;
                return ValueTask.FromResult<IZLinkRedisConnection>(
                    new TestRedisConnection(database: locationDatabase));
            });

        locationOptions.ConnectionString = "location-mutated:6379";
        locationOptions.KeyPrefix = "zlink:location-mutated";
        Assert.IsType<ZLinkStoreReadResult.Missing>(
            await locationStore.ReadAsync(new ZLinkStoreKey("snapshot:key")));

        Assert.Contains(
            "location-original",
            locationConfiguration!.ToString(),
            StringComparison.Ordinal);
        Assert.StartsWith(
            "zlink:location-original:",
            locationCommand.LastKeys![0].ToString(),
            StringComparison.Ordinal);

        var relocationDatabase =
            DispatchProxy.Create<IDatabase, CapturingRedisDatabaseProxy>();
        var relocationCommand =
            (CapturingRedisDatabaseProxy)(object)relocationDatabase;
        ConfigurationOptions? relocationConfiguration = null;
        var relocationOptions = new ZLinkRedisRelocationOptions
        {
            ConnectionString = "relocation-original:6379",
            KeyPrefix = "zlink:relocation-original"
        };
        await using var relocationStore = new ZLinkRedisRelocationStore(
            relocationOptions,
            configuration =>
            {
                relocationConfiguration = configuration;
                return ValueTask.FromResult<IZLinkRedisConnection>(
                    new TestRedisConnection(database: relocationDatabase));
            });

        relocationOptions.ConnectionString = "relocation-mutated:6379";
        relocationOptions.KeyPrefix = "zlink:relocation-mutated";
        Assert.IsType<ZLinkBlobReadResult.Missing>(
            await relocationStore.ReadAsync(
                new ZLinkBlobReference("snapshot-reference")));

        Assert.Contains(
            "relocation-original",
            relocationConfiguration!.ToString(),
            StringComparison.Ordinal);
        Assert.Equal(
            "zlink:relocation-original:payload:snapshot-reference",
            relocationCommand.LastKeys![0].ToString());
    }

    [Fact]
    public async Task Positive_sub_millisecond_location_retention_rounds_up()
    {
        var database =
            DispatchProxy.Create<IDatabase, CapturingWriteRedisDatabaseProxy>();
        var command = (CapturingWriteRedisDatabaseProxy)(object)database;
        await using var store = new ZLinkRedisLocationStore(
            new ZLinkRedisLocationOptions
            {
                ConnectionString = "unused:6379",
                KeyPrefix = "zlink:sub-millisecond"
            },
            _ => ValueTask.FromResult<IZLinkRedisConnection>(
                new TestRedisConnection(database: database)));
        var key = new ZLinkStoreKey("retention:sub-millisecond");

        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(new ZLinkStoreWriteRequest(
                [],
                [
                    new ZLinkStoreMutation.Put(
                        key,
                        new byte[] { 1 },
                        TimeSpan.FromTicks(1))
                ])));

        Assert.Equal(1L, (long)command.LastValues![7]);
    }

    [Fact]
    public async Task Positive_fractional_millisecond_relocation_retention_rounds_up()
    {
        var database = DispatchProxy.Create<
            IDatabase,
            CapturingRelocationPutRedisDatabaseProxy>();
        var command =
            (CapturingRelocationPutRedisDatabaseProxy)(object)database;
        await using var store = new ZLinkRedisRelocationStore(
            new ZLinkRedisRelocationOptions
            {
                ConnectionString = "unused:6379",
                KeyPrefix = "zlink:relocation-sub-millisecond"
            },
            _ => ValueTask.FromResult<IZLinkRedisConnection>(
                new TestRedisConnection(database: database)));

        Assert.IsType<ZLinkBlobPutResult.Stored>(
            await store.PutAsync(
                new ZLinkBlobReference("fractional-retention"),
                new byte[] { 1 },
                TimeSpan.FromTicks(10_001)));

        Assert.Equal(2L, (long)command.LastValues![1]);
    }

    [Fact]
    public async Task Normalized_connection_pool_releases_the_multiplexer_last()
    {
        var connection = new TestRedisConnection();
        var configuration = ConfigurationOptions.Parse("unused:6379");
        var key = $"{Guid.NewGuid():N}:"
                  + ZLinkRedisConnectionPool.CreateKey(configuration);
        var connectCount = 0;

        ValueTask<IZLinkRedisConnection> Connect(ConfigurationOptions _)
        {
            Interlocked.Increment(ref connectCount);
            return ValueTask.FromResult<IZLinkRedisConnection>(connection);
        }

        var first = await ZLinkRedisConnectionPool.RentAsync(
            key,
            configuration,
            Connect);
        var second = await ZLinkRedisConnectionPool.RentAsync(
            key,
            configuration,
            Connect);

        Assert.Equal(1, connectCount);
        await first.DisposeAsync();
        Assert.Equal(0, connection.DisposeCount);
        await second.DisposeAsync();
        Assert.Equal(1, connection.DisposeCount);
        await second.DisposeAsync();
        Assert.Equal(1, connection.DisposeCount);
    }

    [Fact]
    public async Task Failed_pooled_connect_does_not_poison_the_next_rent()
    {
        var connection = new TestRedisConnection();
        var configuration = ConfigurationOptions.Parse("unused:6379");
        var key = Guid.NewGuid().ToString("N");
        var connectCount = 0;

        ValueTask<IZLinkRedisConnection> Connect(ConfigurationOptions _)
        {
            if (Interlocked.Increment(ref connectCount) == 1)
            {
                return ValueTask.FromException<IZLinkRedisConnection>(
                    new IOException("Connect failed."));
            }
            return ValueTask.FromResult<IZLinkRedisConnection>(connection);
        }

        await Assert.ThrowsAsync<IOException>(
            () => ZLinkRedisConnectionPool.RentAsync(
                    key,
                    configuration,
                    Connect)
                .AsTask());

        var lease = await ZLinkRedisConnectionPool.RentAsync(
            key,
            configuration,
            Connect);
        Assert.Equal(2, connectCount);
        await lease.DisposeAsync();
        Assert.Equal(1, connection.DisposeCount);
    }

    [Fact]
    public async Task Caller_cancellation_completes_its_waiter_before_timeout()
    {
        var database =
            DispatchProxy.Create<IDatabase, BlockingRedisDatabaseProxy>();
        var command = (BlockingRedisDatabaseProxy)(object)database;
        var connection = new TestRedisConnection(database: database);
        var store = new ZLinkRedisLocationStore(
            new ZLinkRedisLocationOptions
            {
                ConnectionString = "unused:6379",
                KeyPrefix = "zlink:caller-cancellation",
                OperationTimeout = TimeSpan.FromSeconds(5)
            },
            _ => ValueTask.FromResult<IZLinkRedisConnection>(connection));
        using var cancellation = new CancellationTokenSource();

        var read = store.ReadAsync(
                new ZLinkStoreKey("cancelled:read"),
                cancellation.Token)
            .AsTask();
        await command.CommandStarted.Task.WaitAsync(
            TimeSpan.FromSeconds(5));
        cancellation.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => read);
        command.ReleaseCommand.TrySetResult(MissingReadResult());
        await store.DisposeAsync().AsTask().WaitAsync(
            TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task Timed_out_relocation_put_is_reconciled_by_exact_retry()
    {
        var database = DispatchProxy.Create<
            IDatabase,
            AmbiguousRelocationPutRedisDatabaseProxy>();
        var command =
            (AmbiguousRelocationPutRedisDatabaseProxy)(object)database;
        var connection = new TestRedisConnection(database: database);
        var store = new ZLinkRedisRelocationStore(
            new ZLinkRedisRelocationOptions
            {
                ConnectionString = "unused:6379",
                KeyPrefix = "zlink:ambiguous-put",
                OperationTimeout = TimeSpan.FromMilliseconds(25)
            },
            _ => ValueTask.FromResult<IZLinkRedisConnection>(connection));
        var reference = new ZLinkBlobReference("ambiguous-reference");
        var payload = new byte[] { 1, 2, 3 };

        await Assert.ThrowsAsync<TimeoutException>(
            () => store.PutAsync(
                    reference,
                    payload,
                    TimeSpan.FromMinutes(1))
                .AsTask());

        command.CompleteFirstPutAsStored();
        await command.FirstPutCompleted.Task.WaitAsync(
            TimeSpan.FromSeconds(5));
        Assert.IsType<ZLinkBlobPutResult.AlreadyStored>(
            await store.PutAsync(
                reference,
                payload,
                TimeSpan.FromMinutes(1)));
        await store.DisposeAsync();
    }

    [Fact]
    public async Task Dispose_Waits_For_First_Connect_And_Disposes_The_Published_Connection_Once()
    {
        var connectStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseConnect = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var connection = new TestRedisConnection();
        var store = new ZLinkRedisLocationStore(
            new ZLinkRedisLocationOptions
            {
                ConnectionString = "unused:6379",
                KeyPrefix = "zlink:lifecycle-test"
            },
            async _ =>
            {
                connectStarted.SetResult();
                await releaseConnect.Task.ConfigureAwait(false);
                return connection;
            });

        var read = store.ReadAsync(new ZLinkStoreKey("lifecycle:read"))
            .AsTask();
        await connectStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var dispose = store.DisposeAsync().AsTask();
        Assert.False(dispose.IsCompleted);

        releaseConnect.SetResult();
        Assert.IsType<ZLinkStoreReadResult.Missing>(
            await read.WaitAsync(TimeSpan.FromSeconds(5)));
        await dispose.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(1, connection.DisposeCount);
        await Assert.ThrowsAsync<ObjectDisposedException>(
            () => store.ReadAsync(new ZLinkStoreKey("lifecycle:after-dispose"))
                .AsTask());

        await store.DisposeAsync();
        Assert.Equal(1, connection.DisposeCount);
    }

    [Fact]
    public async Task Concurrent_Dispose_Callers_Await_One_Blocked_Disposal_Transaction()
    {
        var releaseDispose = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var connection = new TestRedisConnection(releaseDispose.Task);
        var store = new ZLinkRedisLocationStore(
            new ZLinkRedisLocationOptions
            {
                ConnectionString = "unused:6379",
                KeyPrefix = "zlink:concurrent-dispose-test"
            },
            _ => ValueTask.FromResult<IZLinkRedisConnection>(connection));

        Assert.IsType<ZLinkStoreReadResult.Missing>(
            await store.ReadAsync(new ZLinkStoreKey("lifecycle:connect")));

        var first = store.DisposeAsync().AsTask();
        await connection.DisposeStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var second = store.DisposeAsync().AsTask();

        Assert.Same(first, second);
        Assert.False(first.IsCompleted);
        Assert.False(second.IsCompleted);
        Assert.Equal(1, connection.DisposeCount);

        releaseDispose.SetResult();
        await Task.WhenAll(first, second).WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(1, connection.DisposeCount);
    }

    [Fact]
    public async Task Dispose_Stops_New_Admission_And_Waits_For_An_Admitted_Command()
    {
        var database = DispatchProxy.Create<IDatabase, BlockingRedisDatabaseProxy>();
        var command = (BlockingRedisDatabaseProxy)(object)database;
        var connection = new TestRedisConnection(database: database);
        var store = new ZLinkRedisLocationStore(
            new ZLinkRedisLocationOptions
            {
                ConnectionString = "unused:6379",
                KeyPrefix = "zlink:operation-drain-test"
            },
            _ => ValueTask.FromResult<IZLinkRedisConnection>(connection));

        var admitted = store.ReadAsync(new ZLinkStoreKey("lifecycle:blocked"))
            .AsTask();
        await command.CommandStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var dispose = store.DisposeAsync().AsTask();
        Assert.False(dispose.IsCompleted);
        Assert.False(connection.DisposeStarted.Task.IsCompleted);
        await Assert.ThrowsAsync<ObjectDisposedException>(
            () => store.ReadAsync(new ZLinkStoreKey("lifecycle:rejected"))
                .AsTask());

        command.ReleaseCommand.TrySetResult(MissingReadResult());
        Assert.IsType<ZLinkStoreReadResult.Missing>(
            await admitted.WaitAsync(TimeSpan.FromSeconds(5)));
        await dispose.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(1, connection.DisposeCount);
    }

    private sealed class TestRedisConnection(
        Task? disposeBlock = null,
        IDatabase? database = null) : IZLinkRedisConnection
    {
        private readonly IDatabase _database = database ?? DispatchProxy.Create<IDatabase, RedisDatabaseProxy>();
        private int _disposeCount;

        public int DisposeCount => Volatile.Read(ref _disposeCount);

        public TaskCompletionSource DisposeStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public IDatabase GetDatabase() => _database;

        public async ValueTask DisposeAsync()
        {
            Interlocked.Increment(ref _disposeCount);
            DisposeStarted.TrySetResult();
            if (disposeBlock is not null) await disposeBlock.ConfigureAwait(false);
        }
    }

    private class RedisDatabaseProxy : DispatchProxy
    {
        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            if (targetMethod?.Name == nameof(IDatabase.ScriptEvaluateAsync)
                && targetMethod.ReturnType == typeof(Task<RedisResult>))
                return Task.FromResult(MissingReadResult());

            throw new NotSupportedException(targetMethod?.ToString());
        }
    }

    private class BlockingRedisDatabaseProxy : DispatchProxy
    {
        public TaskCompletionSource CommandStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource<RedisResult> ReleaseCommand { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            if (targetMethod?.Name == nameof(IDatabase.ScriptEvaluateAsync)
                && targetMethod.ReturnType == typeof(Task<RedisResult>))
            {
                CommandStarted.TrySetResult();
                return ReleaseCommand.Task;
            }

            throw new NotSupportedException(targetMethod?.ToString());
        }
    }

    private class CapturingRedisDatabaseProxy : DispatchProxy
    {
        internal RedisKey[]? LastKeys { get; private set; }

        protected override object? Invoke(
            MethodInfo? targetMethod,
            object?[]? args)
        {
            if (targetMethod?.Name == nameof(IDatabase.ScriptEvaluateAsync)
                && targetMethod.ReturnType == typeof(Task<RedisResult>))
            {
                LastKeys = Assert.IsType<RedisKey[]>(args![1]);
                return Task.FromResult(MissingReadResult());
            }

            throw new NotSupportedException(targetMethod?.ToString());
        }
    }

    private class CapturingWriteRedisDatabaseProxy : DispatchProxy
    {
        internal RedisValue[]? LastValues { get; private set; }

        protected override object? Invoke(
            MethodInfo? targetMethod,
            object?[]? args)
        {
            if (targetMethod?.Name == nameof(IDatabase.ScriptEvaluateAsync)
                && targetMethod.ReturnType == typeof(Task<RedisResult>))
            {
                LastValues = Assert.IsType<RedisValue[]>(args![2]);
                return Task.FromResult(RedisResult.Create(
                [
                    RedisResult.Create((RedisValue)"applied"),
                    RedisResult.Create((RedisValue)DateTimeOffset.UtcNow
                        .ToUnixTimeMilliseconds()),
                    RedisResult.Create(LastValues[4]),
                    RedisResult.Create(LastValues[6])
                ]));
            }

            throw new NotSupportedException(targetMethod?.ToString());
        }
    }

    private class CapturingRelocationPutRedisDatabaseProxy : DispatchProxy
    {
        internal RedisValue[]? LastValues { get; private set; }

        protected override object? Invoke(
            MethodInfo? targetMethod,
            object?[]? args)
        {
            if (targetMethod?.Name == nameof(IDatabase.ScriptEvaluateAsync)
                && targetMethod.ReturnType == typeof(Task<RedisResult>))
            {
                LastValues = Assert.IsType<RedisValue[]>(args![2]);
                return Task.FromResult(RelocationPutResult("stored"));
            }

            throw new NotSupportedException(targetMethod?.ToString());
        }
    }

    private class AmbiguousRelocationPutRedisDatabaseProxy : DispatchProxy
    {
        private readonly TaskCompletionSource<RedisResult> _firstPut =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        private int _putCount;

        internal TaskCompletionSource FirstPutCompleted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        internal void CompleteFirstPutAsStored()
        {
            _firstPut.TrySetResult(RelocationPutResult("stored"));
            FirstPutCompleted.TrySetResult();
        }

        protected override object? Invoke(
            MethodInfo? targetMethod,
            object?[]? args)
        {
            if (targetMethod?.Name == nameof(IDatabase.ScriptEvaluateAsync)
                && targetMethod.ReturnType == typeof(Task<RedisResult>))
            {
                return Interlocked.Increment(ref _putCount) == 1
                    ? _firstPut.Task
                    : Task.FromResult(RelocationPutResult("already"));
            }

            throw new NotSupportedException(targetMethod?.ToString());
        }
    }

    private static RedisResult MissingReadResult() =>
        RedisResult.Create(
        [
            RedisResult.Create((RedisValue)"missing"),
            RedisResult.Create((RedisValue)DateTimeOffset.UtcNow
                .ToUnixTimeMilliseconds())
        ]);

    private static RedisResult RelocationPutResult(string outcome) =>
        RedisResult.Create(
        [
            RedisResult.Create((RedisValue)outcome),
            RedisResult.Create((RedisValue)DateTimeOffset.UtcNow
                .ToUnixTimeMilliseconds())
        ]);
}
