using System.Text;
using System.Text.RegularExpressions;

namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkSpotId
{
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);
    private static readonly Regex ReservedEntrySpotId = new(
        @"^.+-entry-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$",
        RegexOptions.CultureInvariant | RegexOptions.Compiled);

    internal static bool IsValid(string? value)
    {
        if (string.IsNullOrEmpty(value) || value.IndexOf('\0') >= 0)
            return false;

        try
        {
            var byteCount = StrictUtf8.GetByteCount(value);
            return byteCount is >= 1 and <= byte.MaxValue;
        }
        catch (EncoderFallbackException)
        {
            return false;
        }
    }

    internal static string Require(string? value, string paramName)
    {
        if (!IsValid(value))
            throw new ArgumentException(
                "Spot ID must be valid UTF-8 with an encoded size of 1..255 bytes.",
                paramName);
        return value!;
    }

    internal static string RequireCallerProvided(string? value, string paramName)
    {
        var spotId = Require(value, paramName);
        if (IsReservedEntrySpotId(spotId))
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Spot ID '{spotId}' uses the Framework-reserved Entry Spot ID format.");
        }

        return spotId;
    }

    internal static bool IsReservedEntrySpotId(string value) =>
        ReservedEntrySpotId.IsMatch(value);

    internal static string CreateEntrySpotId(string diagnosticPrefix)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(diagnosticPrefix);
        return Require(
            $"{diagnosticPrefix}-entry-{Guid.NewGuid():D}",
            nameof(diagnosticPrefix));
    }

    internal static RoutingId ToNativeRoutingId(string value) =>
        RoutingId.From(StrictUtf8.GetBytes(Require(value, nameof(value))));

    internal static string FromNativeRoutingId(RoutingId value)
    {
        if (value.IsEmpty)
            return string.Empty;
        try
        {
            var decoded = StrictUtf8.GetString(value.ToBytes());
            return IsValid(decoded) ? decoded : string.Empty;
        }
        catch (DecoderFallbackException)
        {
            return string.Empty;
        }
    }
}
