namespace Systems.Zlink.Stream.Connector.Runtime.Protocol;

internal static class ZlinkAdmissionTicketWire
{
    internal const byte Version = 1;
    internal const int HeaderSize = sizeof(byte) + sizeof(long) + sizeof(ulong);
    internal const int MacSize = 32;
    internal const int Size = HeaderSize + MacSize;
    internal const string CanonicalFinalCharacters = "AQgw";
    internal static readonly int EncodedSize = UnpaddedBase64UrlSize(Size);

    internal static int UnpaddedBase64UrlSize(int byteCount) => (byteCount * 4 + 2) / 3;
}
