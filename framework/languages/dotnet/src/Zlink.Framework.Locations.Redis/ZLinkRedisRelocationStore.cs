using System.Text;
using StackExchange.Redis;

#pragma warning disable CS1066

namespace Zlink.Framework.Locations.Redis;

/// <summary>
/// Official Redis store for immutable relocation roots and participant
/// payloads. Location authority remains in <see cref="ZLinkRedisLocationStore"/>.
/// </summary>
public sealed class ZLinkRedisRelocationStore :
    IZLinkRelocationStore,
    IAsyncDisposable
{
    // A 64 MiB relocation data chunk is stored with the Framework's 23-byte
    // immutable chunk envelope. The application adapter limit remains 64 MiB.
    private const int MaximumEncodedBlobSize =
        64 * 1024 * 1024 + 23;

    private const string PutScript = """
        if redis.replicate_commands then redis.replicate_commands() end
        local time = redis.call('TIME')
        local nowMs = tonumber(time[1]) * 1000 + math.floor(tonumber(time[2]) / 1000)
        local current = redis.call('GET', KEYS[1])
        if current then
            if current ~= ARGV[1] then
                return { 'collision', nowMs }
            end
            redis.call('PEXPIRE', KEYS[1], ARGV[2])
            return { 'already', nowMs }
        end
        redis.call('SET', KEYS[1], ARGV[1], 'PX', ARGV[2])
        return { 'stored', nowMs }
        """;

    private const string ReadScript = """
        if redis.replicate_commands then redis.replicate_commands() end
        local time = redis.call('TIME')
        local nowMs = tonumber(time[1]) * 1000 + math.floor(tonumber(time[2]) / 1000)
        local current = redis.call('GET', KEYS[1])
        if not current then
            return { 'missing', nowMs }
        end
        return { 'found', nowMs, current, redis.call('PTTL', KEYS[1]) }
        """;

    private const string RenewScript = """
        if redis.replicate_commands then redis.replicate_commands() end
        local time = redis.call('TIME')
        local nowMs = tonumber(time[1]) * 1000 + math.floor(tonumber(time[2]) / 1000)
        if redis.call('PEXPIRE', KEYS[1], ARGV[1]) == 0 then
            return { 'missing', nowMs }
        end
        return { 'renewed', nowMs }
        """;

    private readonly ConfigurationOptions _configuration;
    private readonly TimeSpan _operationTimeout;
    private readonly string _keyPrefix;
    private readonly Func<ConfigurationOptions, ValueTask<IZLinkRedisConnection>> _connect;
    private readonly SemaphoreSlim _connectGate = new(1, 1);
    private readonly object _disposeGate = new();
    private IZLinkRedisConnection? _connection;
    private Task? _disposeTask;
    private TaskCompletionSource? _operationsDrained;
    private int _activeOperations;
    private int _disposed;

    public ZLinkRedisRelocationStore(ZLinkRedisRelocationOptions options)
        : this(options, connect: null, useSharedConnection: true)
    {
    }

    internal ZLinkRedisRelocationStore(
        ZLinkRedisRelocationOptions options,
        Func<ConfigurationOptions, ValueTask<IZLinkRedisConnection>> connect)
        : this(options, connect, useSharedConnection: false)
    {
    }

    private ZLinkRedisRelocationStore(
        ZLinkRedisRelocationOptions options,
        Func<ConfigurationOptions, ValueTask<IZLinkRedisConnection>>? connect,
        bool useSharedConnection)
    {
        ArgumentNullException.ThrowIfNull(options);
        options.Validate();
        _configuration = options.BuildConfiguration();
        _operationTimeout = options.OperationTimeout;
        _keyPrefix = options.KeyPrefix;
        _connect = connect
                   ?? ZLinkRedisConnectionPool.CreateFactory(
                       _configuration,
                       useSharedConnection
                       && options.ConfigurationOptions is null);
    }

    public ZLinkRedisRelocationStore(Action<ZLinkRedisRelocationOptions> configure)
        : this(Configure(configure))
    {
    }

    public async ValueTask<ZLinkBlobPutResult> PutAsync(
        ZLinkBlobReference reference,
        ReadOnlyMemory<byte> payload,
        TimeSpan retention,
        CancellationToken cancellationToken = default)
    {
        ValidateReference(reference.Value);
        ValidatePayload(payload);
        var retentionMs = ValidateRetention(retention);
        var result = await ExecuteAsync(
                async database => (RedisResult[])(await database.ScriptEvaluateAsync(
                    PutScript,
                    [PayloadKey(reference.Value)],
                    [payload.ToArray(), retentionMs]).ConfigureAwait(false))!,
                cancellationToken)
            .ConfigureAwait(false);
        var storeNow = DateTimeOffset.FromUnixTimeMilliseconds((long)result[1]);
        return (string)result[0]! switch
        {
            "stored" => new ZLinkBlobPutResult.Stored(
                storeNow + TimeSpan.FromMilliseconds(retentionMs),
                storeNow),
            "already" => new ZLinkBlobPutResult.AlreadyStored(
                storeNow + TimeSpan.FromMilliseconds(retentionMs),
                storeNow),
            "collision" => new ZLinkBlobPutResult.Conflict(storeNow),
            _ => throw new InvalidDataException(
                "Redis returned an unknown relocation put result.")
        };
    }

    public async ValueTask<ZLinkBlobReadResult> ReadAsync(
        ZLinkBlobReference reference,
        CancellationToken cancellationToken = default)
    {
        ValidateReference(reference.Value);
        var result = await ExecuteAsync(
                async database => (RedisResult[])(await database.ScriptEvaluateAsync(
                    ReadScript,
                    [PayloadKey(reference.Value)],
                    []).ConfigureAwait(false))!,
                cancellationToken)
            .ConfigureAwait(false);
        var storeNow = DateTimeOffset.FromUnixTimeMilliseconds((long)result[1]);
        if ((string)result[0]! == "missing")
            return new ZLinkBlobReadResult.Missing(storeNow);
        var ttl = (long)result[3];
        if (ttl < 0)
            throw new InvalidDataException(
                "A relocation payload must have a positive retention.");
        return new ZLinkBlobReadResult.Found(
            (byte[])result[2]!,
            storeNow + TimeSpan.FromMilliseconds(ttl),
            storeNow);
    }

    public async ValueTask<ZLinkBlobRenewResult> RenewAsync(
        ZLinkBlobReference reference,
        TimeSpan retention,
        CancellationToken cancellationToken = default)
    {
        ValidateReference(reference.Value);
        var retentionMs = ValidateRetention(retention);
        var result = await ExecuteAsync(
                async database => (RedisResult[])(await database.ScriptEvaluateAsync(
                    RenewScript,
                    [PayloadKey(reference.Value)],
                    [retentionMs]).ConfigureAwait(false))!,
                cancellationToken)
            .ConfigureAwait(false);
        var storeNow = DateTimeOffset.FromUnixTimeMilliseconds((long)result[1]);
        return (string)result[0]! switch
        {
            "renewed" => new ZLinkBlobRenewResult.Renewed(
                storeNow + TimeSpan.FromMilliseconds(retentionMs),
                storeNow),
            "missing" => new ZLinkBlobRenewResult.Missing(storeNow),
            _ => throw new InvalidDataException(
                "Redis returned an unknown relocation renew result.")
        };
    }

    public async ValueTask DeleteAsync(
        ZLinkBlobReference reference,
        CancellationToken cancellationToken = default)
    {
        ValidateReference(reference.Value);
        _ = await ExecuteAsync(
                async database => await database.KeyDeleteAsync(
                        PayloadKey(reference.Value))
                    .ConfigureAwait(false),
                cancellationToken)
            .ConfigureAwait(false);
    }

    public ValueTask DisposeAsync()
    {
        Task disposeTask;
        TaskCompletionSource? startDispose = null;
        lock (_disposeGate)
        {
            if (_disposeTask is null)
            {
                Volatile.Write(ref _disposed, 1);
                startDispose = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                _disposeTask = DisposeCoreAsync(startDispose.Task);
            }
            disposeTask = _disposeTask;
        }
        startDispose?.TrySetResult();
        return new ValueTask(disposeTask);
    }

    private async Task DisposeCoreAsync(Task started)
    {
        await started.ConfigureAwait(false);
        Task? operationsDrained;
        lock (_disposeGate)
        {
            operationsDrained = _activeOperations == 0
                ? null
                : (_operationsDrained ??= new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously)).Task;
        }
        if (operationsDrained is not null)
            await operationsDrained.ConfigureAwait(false);

        var connection = _connection;
        _connection = null;
        try
        {
            if (connection is not null)
                await connection.DisposeAsync().ConfigureAwait(false);
        }
        finally
        {
            _connectGate.Dispose();
        }
    }

    private RedisKey PayloadKey(string reference) =>
        $"{_keyPrefix}:payload:{reference}";

    private async ValueTask<TResult> ExecuteAsync<TResult>(
        Func<IDatabase, ValueTask<TResult>> operation,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return await ExecuteCoreAsync(operation, cancellationToken)
            .AsTask()
            .WaitAsync(_operationTimeout, cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask<TResult> ExecuteCoreAsync<TResult>(
        Func<IDatabase, ValueTask<TResult>> operation,
        CancellationToken cancellationToken)
    {
        using var lease = EnterOperation();
        var database = await GetDatabaseAsync(cancellationToken)
            .ConfigureAwait(false);
        return await operation(database).ConfigureAwait(false);
    }

    private async ValueTask<IDatabase> GetDatabaseAsync(
        CancellationToken cancellationToken)
    {
        if (Volatile.Read(ref _connection) is { } connected)
            return connected.GetDatabase();

        await _connectGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var connection = _connection ??= await _connect(
                    _configuration.Clone())
                .ConfigureAwait(false);
            return connection.GetDatabase();
        }
        finally
        {
            _connectGate.Release();
        }
    }

    private OperationLease EnterOperation()
    {
        lock (_disposeGate)
        {
            ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
            _activeOperations++;
            return new OperationLease(this);
        }
    }

    private void ExitOperation()
    {
        TaskCompletionSource? drained = null;
        lock (_disposeGate)
        {
            _activeOperations--;
            if (_activeOperations == 0 && Volatile.Read(ref _disposed) != 0)
            {
                drained = _operationsDrained;
                _operationsDrained = null;
            }
        }
        drained?.TrySetResult();
    }

    private static ZLinkRedisRelocationOptions Configure(
        Action<ZLinkRedisRelocationOptions> configure)
    {
        ArgumentNullException.ThrowIfNull(configure);
        var options = new ZLinkRedisRelocationOptions();
        configure(options);
        return options;
    }

    private static long ValidateRetention(TimeSpan retention)
    {
        if (retention <= TimeSpan.Zero
            || retention.TotalMilliseconds > long.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(retention));
        return Math.Max(
            1,
            checked((long)Math.Ceiling(retention.TotalMilliseconds)));
    }

    private static void ValidatePayload(ReadOnlyMemory<byte> payload)
    {
        if (payload.Length > MaximumEncodedBlobSize)
            throw new ArgumentOutOfRangeException(nameof(payload));
    }

    private static void ValidateReference(string reference)
    {
        var bytes = Encoding.UTF8.GetByteCount(reference ?? string.Empty);
        if (bytes is < 1 or > 4096)
            throw new ArgumentException(
                "Relocation references must contain 1..4096 UTF-8 bytes.",
                nameof(reference));
    }

    private sealed class OperationLease(ZLinkRedisRelocationStore owner) : IDisposable
    {
        private ZLinkRedisRelocationStore? _owner = owner;

        public void Dispose() =>
            Interlocked.Exchange(ref _owner, null)?.ExitOperation();
    }
}

#pragma warning restore CS1066
