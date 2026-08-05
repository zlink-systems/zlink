using StackExchange.Redis;

namespace Zlink.Framework.Locations.Redis;

/// <summary>
/// Redis implementation of the opaque Location Store provider contract.
/// Framework authority, placement, capacity, and relocation records remain
/// private to the Framework and are stored through this interface.
/// </summary>
public sealed partial class ZLinkRedisLocationStore :
    IZLinkLocationStore,
    IAsyncDisposable
{
    private readonly ConfigurationOptions _configuration;
    private readonly TimeSpan _operationTimeout;
    private readonly ZLinkRedisLocationKeys _keys;
    private readonly Func<ConfigurationOptions, ValueTask<IZLinkRedisConnection>> _connect;
    private readonly SemaphoreSlim _connectGate = new(1, 1);
    private readonly object _disposeGate = new();
    private IZLinkRedisConnection? _connection;
    private Task? _disposeTask;
    private TaskCompletionSource? _operationsDrained;
    private int _activeOperations;
    private int _disposed;

    public ZLinkRedisLocationStore(ZLinkRedisLocationOptions options)
        : this(options, connect: null, useSharedConnection: true)
    {
    }

    internal ZLinkRedisLocationStore(
        ZLinkRedisLocationOptions options,
        Func<ConfigurationOptions, ValueTask<IZLinkRedisConnection>> connect)
        : this(options, connect, useSharedConnection: false)
    {
    }

    private ZLinkRedisLocationStore(
        ZLinkRedisLocationOptions options,
        Func<ConfigurationOptions, ValueTask<IZLinkRedisConnection>>? connect,
        bool useSharedConnection)
    {
        ArgumentNullException.ThrowIfNull(options);
        options.Validate();
        _configuration = options.BuildConfiguration();
        _operationTimeout = options.OperationTimeout;
        _keys = new ZLinkRedisLocationKeys(options.KeyPrefix);
        _connect = connect
                   ?? ZLinkRedisConnectionPool.CreateFactory(
                       _configuration,
                       useSharedConnection
                       && options.ConfigurationOptions is null);
    }

    /// <summary>
    /// Configures the Redis connection and the key prefix used by this Store.
    /// </summary>
    public ZLinkRedisLocationStore(Action<ZLinkRedisLocationOptions> configure)
        : this(Configure(configure))
    {
    }

    private static ZLinkRedisLocationOptions Configure(
        Action<ZLinkRedisLocationOptions> configure)
    {
        ArgumentNullException.ThrowIfNull(configure);
        var options = new ZLinkRedisLocationOptions();
        configure(options);
        return options;
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
        cancellationToken.ThrowIfCancellationRequested();
        if (Volatile.Read(ref _connection) is { } connected)
            return connected.GetDatabase();

        await _connectGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var connection = _connection ??=
                await _connect(_configuration.Clone()).ConfigureAwait(false);
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
            ObjectDisposedException.ThrowIf(
                Volatile.Read(ref _disposed) != 0,
                this);
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

    private sealed class OperationLease(
        ZLinkRedisLocationStore owner) : IDisposable
    {
        private ZLinkRedisLocationStore? _owner = owner;

        public void Dispose() =>
            Interlocked.Exchange(ref _owner, null)?.ExitOperation();
    }

}

internal interface IZLinkRedisConnection : IAsyncDisposable
{
    IDatabase GetDatabase();
}

internal sealed class ZLinkStackExchangeRedisConnection(
    ConnectionMultiplexer connection) : IZLinkRedisConnection
{
    public IDatabase GetDatabase() => connection.GetDatabase();

    public ValueTask DisposeAsync() => connection.DisposeAsync();
}
