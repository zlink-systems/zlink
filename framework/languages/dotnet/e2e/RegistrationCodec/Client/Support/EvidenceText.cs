namespace RegistrationCodec.Client.Support;

internal static class EvidenceText
{
    public static string ExtractValue(string line, string key)
    {
        var marker = key + "=";
        var start = line.IndexOf(marker, StringComparison.Ordinal);
        if (start < 0) return string.Empty;

        start += marker.Length;
        var end = line.IndexOf('|', start);
        return end < 0 ? line[start..] : line[start..end];
    }

    public static bool HasCodec(string[] lines, string codec, string contentType)
    {
        return lines.Any(line => line.Contains($"codec-request|codec={codec}", StringComparison.Ordinal)
                                 && line.Contains($"content={contentType}", StringComparison.Ordinal))
               && lines.Any(line => line.Contains($"codec-command|codec={codec}", StringComparison.Ordinal)
                                    && line.Contains($"content={contentType}", StringComparison.Ordinal));
    }
}