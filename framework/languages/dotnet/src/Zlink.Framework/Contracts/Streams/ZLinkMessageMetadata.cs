namespace Zlink.Framework.Contracts.Streams;

public sealed class ZLinkMessageMetadata
{
    public static ZLinkMessageMetadata Empty { get; } =
        new(new Dictionary<string, string>(StringComparer.Ordinal));

    public ZLinkMessageMetadata(IReadOnlyDictionary<string, string> values)
    {
        ArgumentNullException.ThrowIfNull(values);
        Values = new System.Collections.ObjectModel.ReadOnlyDictionary<string, string>(
            new Dictionary<string, string>(values, StringComparer.Ordinal)
        );
    }

    public IReadOnlyDictionary<string, string> Values { get; }

    public string? Find(string key)
    {
        return Values.TryGetValue(key, out var value) ? value : null;
    }
}

public interface IZLinkMessageMetadataPolicy
{
    bool CanForward(string key);
}
