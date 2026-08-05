using System.Security.Cryptography;
using System.Text;

namespace Zlink.Framework.Runtime.Locations;

internal sealed partial class ZLinkProviderLocationRepository
{
    private const string ContinuationTokenVersion = "v1";
    private const int MaximumContinuationTokenCharacters = 5600;

    private async ValueTask<ZLinkLocationPage<T>> ListCompleteSnapshotPageAsync<T>(
        string prefix,
        ZLinkPageRequest request,
        Func<ReadOnlyMemory<byte>, T> decode,
        CancellationToken cancellationToken)
    {
        var pageSize = ZLinkPageRequestPolicy.Normalize(request).PageSize;

        ZLinkStoreScanCursor? cursor =
            request.ContinuationToken is { } continuationToken
            ? DecodeContinuationToken(prefix, continuationToken)
            : null;
        var result = await provider.ScanAsync(
                new ZLinkStoreScanRequest(prefix, cursor, pageSize),
                cancellationToken)
            .ConfigureAwait(false);
        if (result is ZLinkStoreScanResult.Expired)
        {
            throw new ZLinkLocationSnapshotExpiredException();
        }

        var page = ((ZLinkStoreScanResult.Page)result).Value;
        if (page.Items.Count > pageSize)
        {
            throw new InvalidDataException(
                "The Location Store returned more rows than requested.");
        }

        var items = new T[page.Items.Count];
        for (var index = 0; index < items.Length; index++)
            items[index] = decode(page.Items[index].Value.Bytes);

        return new ZLinkLocationPage<T>(
            items,
            page.NextCursor is { } next
                ? EncodeContinuationToken(prefix, next)
                : null);
    }

    private static string EncodeContinuationToken(
        string prefix,
        ZLinkStoreScanCursor cursor)
    {
        var cursorBytes = Encoding.UTF8.GetBytes(cursor.Value ?? string.Empty);
        if (cursorBytes.Length is < 1 or > 4096)
        {
            throw new InvalidDataException(
                "The Location Store returned an invalid snapshot cursor.");
        }

        return $"{ContinuationTokenVersion}."
               + $"{Base64Url(SHA256.HashData(Encoding.UTF8.GetBytes(prefix)))}."
               + Base64Url(cursorBytes);
    }

    private static ZLinkStoreScanCursor DecodeContinuationToken(
        string prefix,
        string token)
    {
        if (token.Length is < 1 or > MaximumContinuationTokenCharacters)
            throw InvalidContinuationToken();
        var parts = token.Split('.', 3);
        if (parts.Length != 3
            || parts[1].Length != 43
            || !string.Equals(
                parts[0],
                ContinuationTokenVersion,
                StringComparison.Ordinal))
        {
            throw InvalidContinuationToken();
        }

        byte[] prefixDigest;
        byte[] cursorBytes;
        try
        {
            prefixDigest = FromBase64Url(parts[1]);
            cursorBytes = FromBase64Url(parts[2]);
        }
        catch (FormatException)
        {
            throw InvalidContinuationToken();
        }

        var expectedDigest = SHA256.HashData(Encoding.UTF8.GetBytes(prefix));
        if (prefixDigest.Length != expectedDigest.Length
            || !CryptographicOperations.FixedTimeEquals(
                prefixDigest,
                expectedDigest)
            || cursorBytes.Length is < 1 or > 4096)
        {
            throw InvalidContinuationToken();
        }

        string cursor;
        try
        {
            cursor = new UTF8Encoding(
                    encoderShouldEmitUTF8Identifier: false,
                    throwOnInvalidBytes: true)
                .GetString(cursorBytes);
        }
        catch (DecoderFallbackException)
        {
            throw InvalidContinuationToken();
        }

        return new ZLinkStoreScanCursor(cursor);
    }

    private static string Base64Url(ReadOnlySpan<byte> bytes) =>
        Convert.ToBase64String(bytes)
            .TrimEnd('=')
            .Replace('+', '-')
            .Replace('/', '_');

    private static byte[] FromBase64Url(string value)
    {
        var padding = value.Length % 4;
        if (padding == 1)
            throw new FormatException();
        var normalized = value
            .Replace('-', '+')
            .Replace('_', '/');
        if (padding != 0)
            normalized = normalized.PadRight(
                normalized.Length + (4 - padding),
                '=');
        return Convert.FromBase64String(normalized);
    }

    private static ArgumentException InvalidContinuationToken() =>
        new(
            "The Location Store continuation token is invalid.",
            nameof(ZLinkPageRequest.ContinuationToken));
}

internal sealed class ZLinkLocationSnapshotExpiredException()
    : IOException("The Location Store snapshot has expired.");
