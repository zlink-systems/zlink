using Etcdserverpb;
using Google.Protobuf;
using Mvccpb;
using Zlink.Framework.LocationProvider;

namespace Zlink.Framework.Locations.Etcd;

public sealed partial class ZLinkEtcdLocationStore
{
    private static ZLinkStoreKey ConditionKey(ZLinkStoreCondition condition) =>
        condition switch
        {
            ZLinkStoreCondition.Missing value => value.Key,
            ZLinkStoreCondition.Version value => value.Key,
            ZLinkStoreCondition.Value value => value.Key,
            _ => throw new ArgumentException("Unknown Store condition."),
        };

    private static ZLinkStoreKey MutationKey(ZLinkStoreMutation mutation) =>
        mutation switch
        {
            ZLinkStoreMutation.Put value => value.Key,
            ZLinkStoreMutation.Delete value => value.Key,
            _ => throw new ArgumentException("Unknown Store mutation."),
        };

    private static void Validate(ZLinkStoreWriteRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (request.Conditions is null || request.Mutations is null)
            throw new ArgumentException("Store batch lists are required.");
        var conditions = new HashSet<ZLinkStoreKey>();
        var mutations = new HashSet<ZLinkStoreKey>();
        long size = 0;
        foreach (var condition in request.Conditions)
        {
            var key = ConditionKey(condition);
            size += EtcdEncoding.ValidateKey(key.Value).Length;
            if (!conditions.Add(key))
                throw new ArgumentException("Duplicate condition key.");
            if (condition is ZLinkStoreCondition.Version version)
            {
                if (string.IsNullOrEmpty(version.Expected.Value))
                    throw new ArgumentException("A Store version is required.");
                var count = EtcdEncoding.Utf8.GetByteCount(version.Expected.Value);
                if (count > 4096)
                    throw new ArgumentException("A Store version exceeds its bound.");
                size += count;
            }
            if (condition is ZLinkStoreCondition.Value value)
            {
                if (value.Expected.Length > EtcdEncoding.MaxPayload)
                    throw new ArgumentException("A Store value exceeds its bound.");
                size += value.Expected.Length;
            }
        }
        foreach (var mutation in request.Mutations)
        {
            var key = MutationKey(mutation);
            size += EtcdEncoding.ValidateKey(key.Value).Length;
            if (!mutations.Add(key))
                throw new ArgumentException("Duplicate mutation key.");
            if (mutation is ZLinkStoreMutation.Put put)
            {
                if (put.Bytes.Length > EtcdEncoding.MaxPayload)
                    throw new ArgumentException("A Store value exceeds its bound.");
                if (
                    put.Retention is { } retention
                    && (retention <= TimeSpan.Zero || Retention(retention) > EtcdEncoding.MaxTime)
                )
                    throw new ArgumentException("Invalid Store retention.");
                size += put.Bytes.Length;
            }
        }
        if (conditions.Union(mutations).Count() > 2048 || size > EtcdEncoding.MaxPageBytes)
            throw new ArgumentException("The Store batch exceeds its encoded size or key bound.");
    }

    private static long Retention(TimeSpan value) =>
        value.Ticks / TimeSpan.TicksPerMillisecond
        + (value.Ticks % TimeSpan.TicksPerMillisecond == 0 ? 0 : 1);

    private static Compare Exists(ByteString key, bool exists) =>
        new()
        {
            Key = key,
            Target = Compare.Types.CompareTarget.Create,
            Result = exists
                ? Compare.Types.CompareResult.Greater
                : Compare.Types.CompareResult.Equal,
            CreateRevision = 0,
        };

    private async Task<ZLinkStoreWriteResult> Write(
        ZLinkStoreWriteRequest request,
        EtcdOperation operation
    )
    {
        Validate(request);
        var keys = request
            .Conditions.Select(condition => EtcdEncoding.ValidateKey(ConditionKey(condition).Value))
            .ToArray();
        while (true)
        {
            operation.Check();
            var decision = new TxnRequest();
            decision.Success.Add(Read(clock));
            decision.Success.Add(keys.Select(key => Read(MetadataKey(key))));
            var observed = await client.Txn(decision, operation).ConfigureAwait(false);
            var now = StoreNow(observed);
            var metadata = observed
                .Responses.Skip(1)
                .Select(response => One(response.ResponseRange))
                .ToArray();
            var info = metadata
                .Select(value =>
                    value is null ? (EtcdMetadata?)null : EtcdEncoding.DecodeMetadata(value.Value)
                )
                .ToArray();
            var payloadRequests = new TxnRequest();
            var payloadPositions = new List<int>();
            for (var i = 0; i < request.Conditions.Count; i++)
                if (
                    request.Conditions[i] is ZLinkStoreCondition.Value value
                    && info[i] is { } item
                    && item.PayloadLength == value.Expected.Length
                )
                {
                    payloadRequests.Success.Add(
                        Read(PayloadKey(keys[i]), observed.Header.Revision)
                    );
                    payloadPositions.Add(i);
                }
            var payloads = new Dictionary<int, KeyValue>();
            if (payloadPositions.Count != 0)
            {
                TxnResponse payloadResponse;
                try
                {
                    payloadResponse = await client
                        .Txn(payloadRequests, operation)
                        .ConfigureAwait(false);
                }
                catch (EtcdCallException error) when (error.Kind == EtcdErrorKind.Compaction)
                {
                    continue;
                }
                for (var j = 0; j < payloadPositions.Count; j++)
                {
                    var i = payloadPositions[j];
                    var payload = One(payloadResponse.Responses[j].ResponseRange);
                    Record(metadata[i], payload);
                    payloads.Add(i, payload!);
                }
            }
            long? minimumExpiry = null;
            for (var i = 0; i < request.Conditions.Count; i++)
            {
                var live = info[i] is { } record && !record.IsExpired(now);
                var matches = request.Conditions[i] switch
                {
                    ZLinkStoreCondition.Missing => !live,
                    ZLinkStoreCondition.Version value => live
                        && EtcdEncoding.DecodeVersion(value.Expected.Value)
                            == (observed.Header.ClusterId, metadata[i]!.ModRevision),
                    ZLinkStoreCondition.Value value => live
                        && payloads.TryGetValue(i, out var payload)
                        && payload.Value.Span.SequenceEqual(value.Expected.Span),
                    _ => false,
                };
                if (!matches)
                    return new ZLinkStoreWriteResult.Conflict(
                        DateTimeOffset.FromUnixTimeMilliseconds(now)
                    );
                if (
                    live
                    && info[i]!.Value.HasExpiry
                    && request.Conditions[i] is not ZLinkStoreCondition.Missing
                )
                    minimumExpiry = Math.Min(
                        minimumExpiry ?? long.MaxValue,
                        info[i]!.Value.ExpiresAt
                    );
            }
            var apply = new TxnRequest();
            apply.Compare.Add(Exists(clock, true));
            if (minimumExpiry is { } expiry)
                apply.Compare.Add(
                    new Compare
                    {
                        Key = clock,
                        Target = Compare.Types.CompareTarget.Value,
                        Result = Compare.Types.CompareResult.Less,
                        Value = EtcdEncoding.Clock(expiry),
                    }
                );
            for (var i = 0; i < keys.Length; i++)
                apply.Compare.Add(
                    metadata[i] is null
                        ? Exists(MetadataKey(keys[i]), false)
                        : new Compare
                        {
                            Key = MetadataKey(keys[i]),
                            Target = Compare.Types.CompareTarget.Mod,
                            Result = Compare.Types.CompareResult.Equal,
                            ModRevision = metadata[i]!.ModRevision,
                        }
                );
            foreach (var mutation in request.Mutations)
            {
                var key = EtcdEncoding.ValidateKey(MutationKey(mutation).Value);
                if (mutation is ZLinkStoreMutation.Put put)
                {
                    var expires = put.Retention is { } retention
                        ? checked(now + Retention(retention))
                        : 0;
                    if (expires > EtcdEncoding.MaxTime)
                        throw new ArgumentException(
                            "The Store expiry exceeds its encoding range.",
                            nameof(request)
                        );
                    apply.Success.Add(
                        new RequestOp
                        {
                            RequestPut = new PutRequest
                            {
                                Key = MetadataKey(key),
                                Value = EtcdEncoding.Metadata(
                                    put.Retention is not null,
                                    expires,
                                    put.Bytes.Length
                                ),
                            },
                        }
                    );
                    apply.Success.Add(
                        new RequestOp
                        {
                            RequestPut = new PutRequest
                            {
                                Key = PayloadKey(key),
                                Value = ByteString.CopyFrom(put.Bytes.Span),
                            },
                        }
                    );
                }
                else
                {
                    apply.Success.Add(
                        new RequestOp
                        {
                            RequestDeleteRange = new DeleteRangeRequest { Key = MetadataKey(key) },
                        }
                    );
                    apply.Success.Add(
                        new RequestOp
                        {
                            RequestDeleteRange = new DeleteRangeRequest { Key = PayloadKey(key) },
                        }
                    );
                }
            }
            var applied = await client.Txn(apply, operation).ConfigureAwait(false);
            if (!applied.Succeeded)
                continue;
            var versions = request
                .Mutations.OfType<ZLinkStoreMutation.Put>()
                .ToDictionary(
                    put => put.Key,
                    _ => new ZLinkStoreVersion(
                        EtcdEncoding.Version(applied.Header.ClusterId, applied.Header.Revision)
                    )
                );
            return new ZLinkStoreWriteResult.Applied(
                versions,
                DateTimeOffset.FromUnixTimeMilliseconds(now)
            );
        }
    }
}
