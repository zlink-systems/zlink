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
}
