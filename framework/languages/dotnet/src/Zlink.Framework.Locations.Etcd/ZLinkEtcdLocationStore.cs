using Etcdserverpb;
using Google.Protobuf;
using Mvccpb;
using Zlink.Framework.LocationProvider;

namespace Zlink.Framework.Locations.Etcd;

public sealed partial class ZLinkEtcdLocationStore : IZLinkLocationStore, IAsyncDisposable
{
    private readonly EtcdClient client;
    private readonly string storeNamespace;
    private readonly TimeSpan timeout;
    private readonly ByteString clock;
    private readonly object lifetime = new();
    private int active;
    private TaskCompletionSource? drained;
    private Task? disposal;

    public ZLinkEtcdLocationStore(ZLinkEtcdLocationOptions options)
    {
        client = new EtcdClient(options);
        storeNamespace = options.Namespace;
        timeout = options.OperationTimeout;
        clock = EtcdEncoding.Key(storeNamespace, (byte)'c', ByteString.Empty);
    }

    public ZLinkEtcdLocationStore(Action<ZLinkEtcdLocationOptions> configure)
        : this(Configure(configure)) { }

    private static ZLinkEtcdLocationOptions Configure(Action<ZLinkEtcdLocationOptions> configure)
    {
        ArgumentNullException.ThrowIfNull(configure);
        var options = new ZLinkEtcdLocationOptions();
        configure(options);
        return options;
    }

    private ByteString MetadataKey(ByteString key) =>
        EtcdEncoding.Key(storeNamespace, (byte)'m', key);

    private ByteString PayloadKey(ByteString key) =>
        EtcdEncoding.Key(storeNamespace, (byte)'p', key);

    private static RequestOp Read(ByteString key, long revision = 0) =>
        new()
        {
            RequestRange = new RangeRequest { Key = key, Revision = revision },
        };

    private static KeyValue? One(RangeResponse response)
    {
        if (response.Kvs.Count > 1)
            throw EtcdEncoding.Failure();
        return response.Kvs.Count == 0 ? null : response.Kvs[0];
    }

    private static long StoreNow(TxnResponse response) =>
        EtcdEncoding.DecodeClock(
            One(response.Responses[0].ResponseRange)?.Value ?? throw EtcdEncoding.Failure()
        );

    private static EtcdMetadata? Record(KeyValue? metadata, KeyValue? payload)
    {
        if (metadata is null && payload is null)
            return null;
        if (metadata is null || payload is null || metadata.ModRevision != payload.ModRevision)
            throw EtcdEncoding.Failure();
        var value = EtcdEncoding.DecodeMetadata(metadata.Value);
        if (value.PayloadLength != payload.Value.Length)
            throw EtcdEncoding.Failure();
        return value;
    }

    private static ZLinkStoreValue Value(
        KeyValue metadata,
        KeyValue payload,
        EtcdMetadata info,
        ulong cluster,
        long now
    ) =>
        new(
            payload.Value.ToByteArray(),
            new(EtcdEncoding.Version(cluster, metadata.ModRevision)),
            info.HasExpiry ? DateTimeOffset.FromUnixTimeMilliseconds(info.ExpiresAt) : null,
            DateTimeOffset.FromUnixTimeMilliseconds(now)
        );

    private async ValueTask<T> Run<T>(
        CancellationToken cancellationToken,
        Func<EtcdOperation, Task<T>> work
    )
    {
        lock (lifetime)
        {
            ObjectDisposedException.ThrowIf(disposal is not null, this);
            cancellationToken.ThrowIfCancellationRequested();
            active++;
        }
        try
        {
            var now = DateTime.UtcNow;
            var deadline =
                timeout <= DateTime.MaxValue - now ? now.Add(timeout) : DateTime.MaxValue;
            return await work(new EtcdOperation(deadline, cancellationToken)).ConfigureAwait(false);
        }
        finally
        {
            lock (lifetime)
                if (--active == 0)
                    drained?.TrySetResult();
        }
    }

    public ValueTask<ZLinkStoreReadResult> ReadAsync(
        ZLinkStoreKey key,
        CancellationToken cancellationToken = default
    ) =>
        Run<ZLinkStoreReadResult>(
            cancellationToken,
            async operation =>
            {
                var bytes = EtcdEncoding.ValidateKey(key.Value);
                var request = new TxnRequest();
                request.Success.Add(Read(clock));
                request.Success.Add(Read(MetadataKey(bytes)));
                request.Success.Add(Read(PayloadKey(bytes)));
                var response = await client.Txn(request, operation).ConfigureAwait(false);
                var now = StoreNow(response);
                var metadata = One(response.Responses[1].ResponseRange);
                var payload = One(response.Responses[2].ResponseRange);
                var info = Record(metadata, payload);
                if (info is null || info.Value.IsExpired(now))
                    return new ZLinkStoreReadResult.Missing(
                        DateTimeOffset.FromUnixTimeMilliseconds(now)
                    );
                return new ZLinkStoreReadResult.Found(
                    Value(metadata!, payload!, info.Value, response.Header.ClusterId, now)
                );
            }
        );

    public ValueTask<ZLinkStoreWriteResult> WriteAsync(
        ZLinkStoreWriteRequest request,
        CancellationToken cancellationToken = default
    ) => Run<ZLinkStoreWriteResult>(cancellationToken, operation => Write(request, operation));

    public ValueTask<ZLinkStoreScanResult> ScanAsync(
        ZLinkStoreScanRequest request,
        CancellationToken cancellationToken = default
    ) => Run<ZLinkStoreScanResult>(cancellationToken, operation => Scan(request, operation));

    public ValueTask DisposeAsync()
    {
        lock (lifetime)
        {
            if (disposal is not null)
                return new(disposal);
            drained = new(TaskCreationOptions.RunContinuationsAsynchronously);
            if (active == 0)
                drained.SetResult();
            disposal = DisposeAfter(drained.Task);
            return new(disposal);
        }
    }

    private async Task DisposeAfter(Task pending)
    {
        await pending.ConfigureAwait(false);
        client.Dispose();
    }
}
