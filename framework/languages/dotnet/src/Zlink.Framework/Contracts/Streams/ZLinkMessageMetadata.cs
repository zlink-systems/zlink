namespace Zlink.Framework.Contracts.Streams;

public sealed class ZLinkMessageMetadata(
    IReadOnlyDictionary<string, string> values)
{
    public static ZLinkMessageMetadata Empty { get; } = new(
        new Dictionary<string, string>(StringComparer.Ordinal));

    public IReadOnlyDictionary<string, string> Values { get; } = values;

    public string? Find(string key)
    {
        return Values.TryGetValue(key, out var value) ? value : null;
    }
}

public interface IZLinkMessageMetadataPolicy
{
    bool CanForward(string key);
}
