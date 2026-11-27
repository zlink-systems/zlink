using System.Globalization;
using System.Text;
using Zlink.Framework.LocationProvider;

namespace Zlink.Framework.Internal;

internal static class ZLinkStoreCounter
{
    internal const int MaximumDigits = 19;

    internal static bool TrySumWithinLimit(
        ZLinkStoreCondition.SumAtMost condition,
        Func<ZLinkStoreKey, ReadOnlyMemory<byte>?> read,
        out bool withinLimit
    )
    {
        Int128 sum = condition.Delta;
        foreach (var key in condition.Keys)
        {
            if (!TryRead(read(key), out var current))
            {
                withinLimit = false;
                return false;
            }
            sum += current;
        }
        withinLimit = sum <= condition.Limit;
        return true;
    }

    internal static bool TryAdd(
        ZLinkStoreMutation.Add add,
        ReadOnlyMemory<byte>? bytes,
        out ZLinkStoreMutation.Put put
    )
    {
        put = null!;
        if (!TryRead(bytes, out var current))
            return false;
        var next = (Int128)current + add.Delta;
        if (next < 0 || next > long.MaxValue)
            return false;
        put = new(
            add.Key,
            Encoding.UTF8.GetBytes(next.ToString(CultureInfo.InvariantCulture)),
            null
        );
        return true;
    }

    private static bool TryRead(ReadOnlyMemory<byte>? bytes, out long current)
    {
        current = 0;
        if (bytes is not { } value)
            return true;
        var digits = value.Span;
        return digits.Length is >= 1 and <= MaximumDigits
            && (digits.Length == 1 || digits[0] != (byte)'0')
            && digits[0] is >= (byte)'0' and <= (byte)'9'
            && digits[^1] is >= (byte)'0' and <= (byte)'9'
            && long.TryParse(digits, NumberStyles.None, CultureInfo.InvariantCulture, out current);
    }
}
