using System.Text;

namespace Zlink.Framework.Runtime.Configuration;

internal static class ZLinkRoutingIdPolicy
{
    public static RoutingId Derive(RoutingId baseRid, string suffix)
    {
        if (baseRid.Size == 0) return default;

        var suffixBytes = Encoding.UTF8.GetBytes(suffix);
        var baseBytes = baseRid.ToBytes();
        var size = baseBytes.Length + 1 + suffixBytes.Length;
        if (size > 255)
            throw new ZLinkConfigurationException(
                $"Derived routing id with suffix '{suffix}' exceeds the 255 byte limit.");

        var bytes = new byte[size];
        baseBytes.CopyTo(bytes);
        bytes[baseBytes.Length] = 0;
        suffixBytes.CopyTo(bytes.AsSpan(baseBytes.Length + 1));
        return RoutingId.From(bytes);
    }
}