namespace Zlink.Framework.Runtime.Configuration;

internal static class ZLinkFanoutRoutingIdPolicy
{
    internal static void ValidatePrefix(string prefix)
    {
        ArgumentNullException.ThrowIfNull(prefix);
        if (prefix.Length is < 1 or > 64
            || prefix.Any(static character =>
                !((character >= 'A' && character <= 'Z')
                  || (character >= 'a' && character <= 'z')
                  || (character >= '0' && character <= '9')
                  || character is '.' or '_' or '-')))
            throw new ZLinkConfigurationException(
                "Fanout publisher routing-id prefix must contain 1 to 64 ASCII "
                + "letters, digits, '.', '_' or '-'.");
    }

    internal static RoutingId Create(string prefix)
    {
        ValidatePrefix(prefix);
        return RoutingId.From($"{prefix}-{Guid.NewGuid():D}");
    }
}
