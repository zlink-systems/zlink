namespace Zlink.Framework.Runtime.Identifiers;

internal readonly record struct ZLinkMeshName
{
    private readonly string? _value;

    private ZLinkMeshName(string value) => _value = value;

    internal string Value => _value ?? string.Empty;

    internal static ZLinkMeshName FromBoundary(string value, string paramName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(value, paramName);
        return new ZLinkMeshName(value);
    }

    public override string ToString() => Value;
}

internal readonly record struct ZLinkChannelName
{
    private readonly string? _value;

    private ZLinkChannelName(string value) => _value = value;

    internal string Value => _value ?? string.Empty;

    internal static ZLinkChannelName FromBoundary(string value, string paramName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(value, paramName);
        if (value.Contains('\0')
            || System.Text.Encoding.UTF8.GetByteCount(value) > byte.MaxValue)
            throw new ArgumentOutOfRangeException(
                paramName,
                "Channel name must be 1 to 255 UTF-8 bytes without NUL.");
        return new ZLinkChannelName(value);
    }

    public override string ToString() => Value;
}

internal readonly record struct ZLinkActorId
{
    private readonly string? _value;

    private ZLinkActorId(string value) => _value = value;

    internal string Value => _value ?? string.Empty;

    internal static ZLinkActorId FromBoundary(string value, string paramName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(value, paramName);
        if (value.Contains('\0')
            || System.Text.Encoding.UTF8.GetByteCount(value) > byte.MaxValue)
            throw new ArgumentOutOfRangeException(
                paramName,
                "Actor ID must be 1 to 255 UTF-8 bytes without NUL.");
        return new ZLinkActorId(value);
    }

    public override string ToString() => Value;
}

internal readonly record struct ZLinkSpotNodeName
{
    private readonly string? _value;

    private ZLinkSpotNodeName(string value) => _value = value;

    internal string Value => _value ?? string.Empty;

    internal static ZLinkSpotNodeName FromBoundary(string value, string paramName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(value, paramName);
        return new ZLinkSpotNodeName(value);
    }

    public override string ToString() => Value;
}

internal readonly record struct ZLinkStreamNodeName
{
    private readonly string? _value;

    private ZLinkStreamNodeName(string value) => _value = value;

    internal string Value => _value ?? string.Empty;

    internal static ZLinkStreamNodeName FromBoundary(string value, string paramName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(value, paramName);
        return new ZLinkStreamNodeName(value);
    }

    public override string ToString() => Value;
}

internal readonly record struct ZLinkTimerName
{
    private readonly string? _value;

    private ZLinkTimerName(string value) => _value = value;

    internal string Value => _value ?? string.Empty;

    internal static ZLinkTimerName FromBoundary(string value, string paramName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(value, paramName);
        return new ZLinkTimerName(value);
    }

    public override string ToString() => Value;
}

internal static class ZLinkRuntimeIdentifierDictionaryExtensions
{
    internal static bool TryGetValue<TValue>(
        this Dictionary<ZLinkActorId, TValue> source,
        string value,
        out TValue result) =>
        source.TryGetValue(
            ZLinkActorId.FromBoundary(value, nameof(value)),
            out result!);

    internal static bool ContainsKey<TValue>(
        this Dictionary<ZLinkActorId, TValue> source,
        string value) =>
        source.ContainsKey(ZLinkActorId.FromBoundary(value, nameof(value)));

    internal static void Add<TValue>(
        this Dictionary<ZLinkActorId, TValue> source,
        string value,
        TValue item) =>
        source.Add(
            ZLinkActorId.FromBoundary(value, nameof(value)),
            item);

    internal static bool Remove<TValue>(
        this Dictionary<ZLinkActorId, TValue> source,
        string value) =>
        source.Remove(ZLinkActorId.FromBoundary(value, nameof(value)));

    internal static bool TryGetValue<TValue>(
        this Dictionary<ZLinkChannelName, TValue> source,
        string value,
        out TValue result) =>
        source.TryGetValue(
            ZLinkChannelName.FromBoundary(value, nameof(value)),
            out result!);

    internal static void Add<TValue>(
        this Dictionary<ZLinkChannelName, TValue> source,
        string value,
        TValue item) =>
        source.Add(
            ZLinkChannelName.FromBoundary(value, nameof(value)),
            item);

    internal static bool Remove<TValue>(
        this Dictionary<ZLinkChannelName, TValue> source,
        string value) =>
        source.Remove(ZLinkChannelName.FromBoundary(value, nameof(value)));

    internal static bool TryGetValue<TValue>(
        this Dictionary<ZLinkSpotNodeName, TValue> source,
        string value,
        out TValue result) =>
        source.TryGetValue(
            ZLinkSpotNodeName.FromBoundary(value, nameof(value)),
            out result!);

    internal static void Add<TValue>(
        this Dictionary<ZLinkSpotNodeName, TValue> source,
        string value,
        TValue item) =>
        source.Add(
            ZLinkSpotNodeName.FromBoundary(value, nameof(value)),
            item);

    internal static bool Remove<TValue>(
        this Dictionary<ZLinkSpotNodeName, TValue> source,
        string value) =>
        source.Remove(ZLinkSpotNodeName.FromBoundary(value, nameof(value)));

    internal static bool TryGetValue<TValue>(
        this Dictionary<ZLinkStreamNodeName, TValue> source,
        string value,
        out TValue result) =>
        source.TryGetValue(
            ZLinkStreamNodeName.FromBoundary(value, nameof(value)),
            out result!);

    internal static void Add<TValue>(
        this Dictionary<ZLinkStreamNodeName, TValue> source,
        string value,
        TValue item) =>
        source.Add(
            ZLinkStreamNodeName.FromBoundary(value, nameof(value)),
            item);

    internal static bool Remove<TValue>(
        this Dictionary<ZLinkStreamNodeName, TValue> source,
        string value) =>
        source.Remove(ZLinkStreamNodeName.FromBoundary(value, nameof(value)));
}

internal static class ZLinkSpotIdDictionaryExtensions
{
    internal static bool ContainsKey<TValue>(
        this Dictionary<ZLinkSpotId, TValue> source,
        string spotId) =>
        source.ContainsKey(ZLinkSpotId.FromBoundary(
            spotId,
            nameof(spotId)));

    internal static bool TryGetValue<TValue>(
        this Dictionary<ZLinkSpotId, TValue> source,
        string spotId,
        out TValue value) =>
        source.TryGetValue(
            ZLinkSpotId.FromBoundary(spotId, nameof(spotId)),
            out value!);

    internal static void Add<TValue>(
        this Dictionary<ZLinkSpotId, TValue> source,
        string spotId,
        TValue value) =>
        source.Add(
            ZLinkSpotId.FromBoundary(spotId, nameof(spotId)),
            value);

    internal static bool Remove<TValue>(
        this Dictionary<ZLinkSpotId, TValue> source,
        string spotId) =>
        source.Remove(ZLinkSpotId.FromBoundary(
            spotId,
            nameof(spotId)));

    internal static bool Remove<TValue>(
        this Dictionary<ZLinkSpotId, TValue> source,
        string spotId,
        out TValue value) =>
        source.Remove(
            ZLinkSpotId.FromBoundary(spotId, nameof(spotId)),
            out value!);
}
