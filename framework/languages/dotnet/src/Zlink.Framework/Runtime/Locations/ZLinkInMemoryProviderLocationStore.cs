using System.Globalization;
using System.Text;

namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Deterministic single-process implementation of the opaque provider SPI.
/// It is used only by framework tests.
/// </summary>
internal sealed class ZLinkInMemoryProviderLocationStore(
    TimeProvider? timeProvider = null) : IZLinkLocationStore
{
    private const int MaximumEncodedBatchBytes = 4 * 1024 * 1024;
    private const int MaximumEncodedPageBytes = 4 * 1024 * 1024;
    private static readonly TimeSpan ScanRetention = TimeSpan.FromMinutes(1);
    private readonly object _gate = new();
    private readonly TimeProvider _time = timeProvider ?? TimeProvider.System;
    private readonly Dictionary<ZLinkStoreKey, Entry> _entries = [];
    private readonly Dictionary<string, Snapshot> _snapshots =
        new(StringComparer.Ordinal);
    private ulong _version;
    private DateTimeOffset? _nextEntryExpiry;

    public ValueTask<ZLinkStoreReadResult> ReadAsync(
        ZLinkStoreKey key,
        CancellationToken cancellationToken = default)
    {
        ValidateKey(key);
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            var now = _time.GetUtcNow();
            RemoveExpiredNoLock(now);
            return ValueTask.FromResult<ZLinkStoreReadResult>(
                _entries.TryGetValue(key, out var entry)
                    ? new ZLinkStoreReadResult.Found(
                        ToValue(entry, now))
                    : new ZLinkStoreReadResult.Missing(now));
        }
    }

    public ValueTask<ZLinkStoreWriteResult> WriteAsync(
        ZLinkStoreWriteRequest request,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(request);
        ValidateRequest(request);
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            var now = _time.GetUtcNow();
            RemoveExpiredNoLock(now);
            foreach (var condition in request.Conditions)
            {
                var satisfied = condition switch
                {
                    ZLinkStoreCondition.Missing missing =>
                        !_entries.ContainsKey(missing.Key),
                    ZLinkStoreCondition.Version version =>
                        _entries.TryGetValue(version.Key, out var current)
                        && current.Version == version.Expected,
                    _ => false
                };
                if (!satisfied)
                {
                    return ValueTask.FromResult<ZLinkStoreWriteResult>(
                        new ZLinkStoreWriteResult.Conflict(now));
                }
            }

            var versions =
                new Dictionary<ZLinkStoreKey, ZLinkStoreVersion>();
            foreach (var mutation in request.Mutations)
            {
                switch (mutation)
                {
                    case ZLinkStoreMutation.Put put:
                    {
                        var version = new ZLinkStoreVersion(
                            checked(++_version).ToString(
                                CultureInfo.InvariantCulture));
                        _entries[put.Key] = new Entry(
                            put.Bytes.ToArray(),
                            version,
                            put.Retention is { } retention
                                ? now + retention
                                : null);
                        if (put.Retention is { } putRetention)
                        {
                            var expiresAt = now + putRetention;
                            if (_nextEntryExpiry is null
                                || expiresAt < _nextEntryExpiry.Value)
                                _nextEntryExpiry = expiresAt;
                        }
                        versions.Add(put.Key, version);
                        break;
                    }
                    case ZLinkStoreMutation.Delete delete:
                        _entries.Remove(delete.Key);
                        break;
                }
            }
            return ValueTask.FromResult<ZLinkStoreWriteResult>(
                new ZLinkStoreWriteResult.Applied(versions, now));
        }
    }

    public ValueTask<ZLinkStoreScanResult> ScanAsync(
        ZLinkStoreScanRequest request,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (request.Limit is < 1 or > 1000)
            throw new ArgumentOutOfRangeException(nameof(request));
        var prefix = request.Prefix
                     ?? throw new ArgumentException(
                         "The scan prefix cannot be null.",
                         nameof(request));
        if (Encoding.UTF8.GetByteCount(prefix) > 1024)
            throw new ArgumentException(
                "The scan prefix exceeds 1024 UTF-8 bytes.",
                nameof(request));
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            var now = _time.GetUtcNow();
            RemoveExpiredNoLock(now);
            RemoveExpiredSnapshotsNoLock(now);

            Snapshot snapshot;
            var offset = 0;
            if (request.Cursor is { } cursor)
            {
                var cursorValue = cursor.Value ?? string.Empty;
                var cursorBytes = Encoding.UTF8.GetByteCount(cursorValue);
                if (cursorBytes is < 1 or > 4096)
                    throw new ArgumentException(
                        "Store scan cursors must contain 1..4096 UTF-8 bytes.",
                        nameof(request));
                var separator = cursorValue.LastIndexOf(':');
                if (separator <= 0
                    || !int.TryParse(
                        cursorValue[(separator + 1)..],
                        NumberStyles.None,
                        CultureInfo.InvariantCulture,
                        out offset)
                    || !_snapshots.TryGetValue(
                        cursorValue[..separator],
                        out snapshot!))
                {
                    return ValueTask.FromResult<ZLinkStoreScanResult>(
                        new ZLinkStoreScanResult.Expired());
                }
            }
            else
            {
                var id = Guid.NewGuid().ToString("N");
                snapshot = new Snapshot(
                    _entries
                        .Where(pair => pair.Key.Value.StartsWith(
                            prefix,
                            StringComparison.Ordinal))
                        .OrderBy(static pair => pair.Key.Value, StringComparer.Ordinal)
                        .Select(pair => new KeyValuePair<
                            ZLinkStoreKey,
                            ZLinkStoreValue>(
                            pair.Key,
                            ToValue(pair.Value, now)))
                        .ToArray(),
                    now + ScanRetention);
                _snapshots[id] = snapshot;
            }

            var items = new List<KeyValuePair<ZLinkStoreKey, ZLinkStoreValue>>();
            var encodedBytes = 0;
            foreach (var item in snapshot.Items.Skip(offset))
            {
                if (items.Count == request.Limit)
                    break;
                var itemBytes = Encoding.UTF8.GetByteCount(item.Key.Value)
                                + Encoding.UTF8.GetByteCount(
                                    item.Value.Version.Value)
                                + item.Value.Bytes.Length;
                if (items.Count != 0
                    && encodedBytes + itemBytes > MaximumEncodedPageBytes)
                    break;
                items.Add(item);
                encodedBytes += itemBytes;
            }
            var nextOffset = offset + items.Count;
            ZLinkStoreScanCursor? next = null;
            if (nextOffset < snapshot.Items.Count)
            {
                var id = _snapshots.First(pair =>
                    ReferenceEquals(pair.Value, snapshot)).Key;
                next = new ZLinkStoreScanCursor(
                    $"{id}:{nextOffset.ToString(CultureInfo.InvariantCulture)}");
            }
            return ValueTask.FromResult<ZLinkStoreScanResult>(
                new ZLinkStoreScanResult.Page(
                    new ZLinkStoreScanPage(items.ToArray(), next, now)));
        }
    }

    private static ZLinkStoreValue ToValue(Entry entry, DateTimeOffset now) =>
        new(entry.Bytes, entry.Version, entry.ExpiresAt, now);

    private void RemoveExpiredNoLock(DateTimeOffset now)
    {
        if (_nextEntryExpiry is null || now < _nextEntryExpiry) return;
        DateTimeOffset? nextExpiry = null;
        foreach (var pair in _entries.ToArray())
        {
            if (pair.Value.ExpiresAt <= now)
            {
                _entries.Remove(pair.Key);
                continue;
            }
            if (pair.Value.ExpiresAt is { } expiresAt
                && (nextExpiry is null || expiresAt < nextExpiry.Value))
                nextExpiry = expiresAt;
        }
        _nextEntryExpiry = nextExpiry;
    }

    private void RemoveExpiredSnapshotsNoLock(DateTimeOffset now)
    {
        foreach (var id in _snapshots
                     .Where(pair => pair.Value.ExpiresAt <= now)
                     .Select(static pair => pair.Key)
                     .ToArray())
        {
            _snapshots.Remove(id);
        }
    }

    private static void ValidateRequest(ZLinkStoreWriteRequest request)
    {
        var encodedBytes = 0L;
        var conditionKeys = request.Conditions.Select(
            static condition => condition switch
            {
                ZLinkStoreCondition.Missing missing => missing.Key,
                ZLinkStoreCondition.Version version => version.Key,
                _ => throw new ArgumentException(
                    "Unknown Store condition.",
                    nameof(request))
            }).ToArray();
        var mutationKeys = request.Mutations.Select(
            static mutation => mutation switch
            {
                ZLinkStoreMutation.Put put => put.Key,
                ZLinkStoreMutation.Delete delete => delete.Key,
                _ => throw new ArgumentException(
                    "Unknown Store mutation.",
                    nameof(request))
            }).ToArray();
        if (conditionKeys.Distinct().Count() != conditionKeys.Length
            || mutationKeys.Distinct().Count() != mutationKeys.Length
            || conditionKeys.Concat(mutationKeys).Distinct().Count() > 2048)
        {
            throw new ArgumentException(
                "The Store batch contains duplicate or too many keys.",
                nameof(request));
        }
        foreach (var key in conditionKeys.Concat(mutationKeys))
        {
            ValidateKey(key);
            encodedBytes += Encoding.UTF8.GetByteCount(key.Value);
        }
        foreach (var condition in request.Conditions
                     .OfType<ZLinkStoreCondition.Version>())
        {
            var length = Encoding.UTF8.GetByteCount(
                condition.Expected.Value ?? string.Empty);
            if (length is < 1 or > 4096)
                throw new ArgumentException(
                    "Store versions must contain 1..4096 UTF-8 bytes.",
                    nameof(request));
            encodedBytes += length;
        }
        foreach (var put in request.Mutations
                     .OfType<ZLinkStoreMutation.Put>())
        {
            if (put.Bytes.Length > 1024 * 1024
                || put.Retention is { } retention
                && retention <= TimeSpan.Zero)
            {
                throw new ArgumentException(
                    "The Store put exceeds its value or retention bound.",
                    nameof(request));
            }
            encodedBytes += put.Bytes.Length;
        }
        if (encodedBytes > MaximumEncodedBatchBytes)
            throw new ArgumentException(
                "The encoded Store batch exceeds 4 MiB.",
                nameof(request));
    }

    private static void ValidateKey(ZLinkStoreKey key)
    {
        var length = Encoding.UTF8.GetByteCount(key.Value ?? string.Empty);
        if (length is < 1 or > 1024)
            throw new ArgumentException(
                "Store keys must contain 1..1024 UTF-8 bytes.",
                nameof(key));
    }

    private sealed record Entry(
        ReadOnlyMemory<byte> Bytes,
        ZLinkStoreVersion Version,
        DateTimeOffset? ExpiresAt);

    private sealed record Snapshot(
        IReadOnlyList<KeyValuePair<ZLinkStoreKey, ZLinkStoreValue>> Items,
        DateTimeOffset ExpiresAt);
}
