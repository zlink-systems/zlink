using Etcdserverpb;
using Google.Protobuf;
using Mvccpb;
using Zlink.Framework.Internal;
using Zlink.Framework.LocationProvider;

namespace Zlink.Framework.Locations.Etcd;

public sealed partial class ZLinkEtcdLocationStore
{
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
            foreach (var key in ZLinkStoreBatchKeys.ForCondition(condition))
            {
                size += EtcdEncoding.ValidateKey(key.Value).Length;
                if (!conditions.Add(key))
                    throw new ArgumentException("Duplicate condition key.");
            }
            if (condition is ZLinkStoreCondition.SumAtMost)
                size += 16;
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
            var key = ZLinkStoreBatchKeys.ForMutation(mutation);
            size += EtcdEncoding.ValidateKey(key.Value).Length;
            if (!mutations.Add(key))
                throw new ArgumentException("Duplicate mutation key.");
            if (mutation is ZLinkStoreMutation.Add)
                size += 8;
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
        var counterKeys = request
            .Conditions.OfType<ZLinkStoreCondition.SumAtMost>()
            .SelectMany(sum => sum.Keys)
            .Concat(request.Mutations.OfType<ZLinkStoreMutation.Add>().Select(add => add.Key))
            .ToHashSet();
        var valueConditions = request
            .Conditions.OfType<ZLinkStoreCondition.Value>()
            .ToDictionary(value => value.Key);
        var logicalKeys = request
            .Conditions.SelectMany(ZLinkStoreBatchKeys.ForCondition)
            .Union(counterKeys)
            .ToArray();
        var positions = logicalKeys
            .Select((key, index) => (key, index))
            .ToDictionary(pair => pair.key, pair => pair.index);
        var keys = logicalKeys.Select(key => EtcdEncoding.ValidateKey(key.Value)).ToArray();
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
            for (var i = 0; i < keys.Length; i++)
                if (
                    info[i] is { } item
                    && (
                        counterKeys.Contains(logicalKeys[i])
                            && !item.IsExpired(now)
                            && item.PayloadLength is >= 1 and <= ZLinkStoreCounter.MaximumDigits
                        || valueConditions.TryGetValue(logicalKeys[i], out var value)
                            && item.PayloadLength == value.Expected.Length
                    )
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
            var storeNow = DateTimeOffset.FromUnixTimeMilliseconds(now);
            ReadOnlyMemory<byte>? ReadCounter(ZLinkStoreKey key)
            {
                var i = positions[key];
                if (info[i] is not { } item || item.IsExpired(now))
                    return null;
                return payloads.TryGetValue(i, out var payload)
                    ? payload.Value.Memory
                    : ReadOnlyMemory<byte>.Empty;
            }

            foreach (var condition in request.Conditions)
            {
                if (condition is ZLinkStoreCondition.SumAtMost sum)
                {
                    if (!ZLinkStoreCounter.TrySumWithinLimit(sum, ReadCounter, out var withinLimit))
                        return new ZLinkStoreWriteResult.Corrupt(storeNow);
                    if (!withinLimit)
                        return new ZLinkStoreWriteResult.Conflict(storeNow);
                    continue;
                }

                var i = positions[ZLinkStoreBatchKeys.ForCondition(condition)[0]];
                var live = info[i] is { } record && !record.IsExpired(now);
                var matches = condition switch
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
                    return new ZLinkStoreWriteResult.Conflict(storeNow);
            }

            long? minimumExpiry = null;
            foreach (var record in info)
                if (record is { HasExpiry: true } live && !live.IsExpired(now))
                    minimumExpiry = Math.Min(minimumExpiry ?? long.MaxValue, live.ExpiresAt);

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
                var logicalKey = ZLinkStoreBatchKeys.ForMutation(mutation);
                var key = EtcdEncoding.ValidateKey(logicalKey.Value);
                var put = mutation as ZLinkStoreMutation.Put;
                if (
                    mutation is ZLinkStoreMutation.Add add
                    && !ZLinkStoreCounter.TryAdd(add, ReadCounter(logicalKey), out put)
                )
                    return new ZLinkStoreWriteResult.Corrupt(storeNow);
                if (put is not null)
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
            var version = new ZLinkStoreVersion(
                EtcdEncoding.Version(applied.Header.ClusterId, applied.Header.Revision)
            );
            return new ZLinkStoreWriteResult.Applied(
                request
                    .Mutations.Where(mutation => mutation is not ZLinkStoreMutation.Delete)
                    .ToDictionary(ZLinkStoreBatchKeys.ForMutation, _ => version),
                storeNow
            );
        }
    }
}
