namespace Zlink.Framework.Runtime.Channels;

internal static class ZLinkClientServerMessageBound
{
    internal static bool Fits(
        IReadOnlyList<Message> parts,
        uint maximumMessageBytes)
    {
        ulong total = 0;
        for (var index = 0; index < parts.Count; index++)
        {
            total += checked((ulong)parts[index].Size);
            if (total > maximumMessageBytes)
                return false;
        }
        return true;
    }

    internal static ZLinkFrameworkException CreateExceededException(
        uint maximumMessageBytes) =>
        new(
            ZLinkFrameworkErrorKind.CapacityExceeded,
            $"The complete ClientServer message exceeds the admitted {maximumMessageBytes}-byte bound.",
            ZLinkRetryAdvice.DoNotRetry);
}
