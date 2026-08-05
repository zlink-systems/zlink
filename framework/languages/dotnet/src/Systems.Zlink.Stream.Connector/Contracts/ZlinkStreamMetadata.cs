namespace Systems.Zlink.Stream.Connector.Contracts;

public sealed class ZlinkStreamMetadata
{
    private ZlinkStreamMetadata(IReadOnlyDictionary<string, string> values)
    {
        Values = values;
    }

    public static ZlinkStreamMetadata Empty { get; } = new(new Dictionary<string, string>());

    public int Count => Values.Count;

    public IReadOnlyDictionary<string, string> Values { get; }

    public string? Get(string key)
    {
        return Values.TryGetValue(key, out var value) ? value : null;
    }

    public bool TryGet(string key, out string value)
    {
        return Values.TryGetValue(key, out value!);
    }

    public ZlinkStreamMetadata With(string key, string value)
    {
        ValidateKey(key);
        ArgumentNullException.ThrowIfNull(value);

        var copy = new Dictionary<string, string>(Values, StringComparer.Ordinal)
        {
            [key] = value
        };
        return new ZlinkStreamMetadata(copy);
    }

    public ZlinkStreamMetadata WithMany(IEnumerable<KeyValuePair<string, string>> values)
    {
        ArgumentNullException.ThrowIfNull(values);

        var copy = new Dictionary<string, string>(Values, StringComparer.Ordinal);
        foreach (var (key, value) in values)
        {
            ValidateKey(key);
            ArgumentNullException.ThrowIfNull(value);
            copy[key] = value;
        }

        return new ZlinkStreamMetadata(copy);
    }

    internal static ZlinkStreamMetadata FromDictionary(Dictionary<string, string> values)
    {
        return values.Count == 0 ? Empty : new ZlinkStreamMetadata(values);
    }

    private static void ValidateKey(string key)
    {
        if (string.IsNullOrEmpty(key))
            throw new ZlinkStreamException(new ZlinkStreamError(
                ZlinkStreamErrorCode.ValidationFailed,
                "Metadata key must not be empty."));
    }
}