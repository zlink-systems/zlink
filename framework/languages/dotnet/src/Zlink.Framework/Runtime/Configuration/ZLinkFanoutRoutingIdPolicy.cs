namespace Zlink.Framework.Runtime.Configuration;

internal static class ZLinkFanoutRoutingIdPolicy
{
    internal const int MaximumPrefixLength = 64;

    internal static bool IsValidPrefix(string? prefix) =>
        !string.IsNullOrEmpty(prefix)
        && prefix.Length <= MaximumPrefixLength
        && prefix.All(static character =>
            (character >= 'A' && character <= 'Z')
            || (character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9')
            || character is '.' or '_' or '-'
        );

    internal static void ValidatePrefix(string prefix)
    {
        ArgumentNullException.ThrowIfNull(prefix);
        if (!IsValidPrefix(prefix))
            throw new ZLinkConfigurationException(
                $"Fanout publisher routing-id prefix must be non-empty and contain at most {MaximumPrefixLength} ASCII "
                    + "letters, digits, '.', '_' or '-'."
            );
    }

    internal static RoutingId Create(string prefix)
    {
        ValidatePrefix(prefix);
        return RoutingId.From($"{prefix}-{Guid.NewGuid():D}");
    }
}
