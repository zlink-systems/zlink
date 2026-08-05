using System;

public static class PerfEnv
{
    public static string ReadString(string name, string fallback)
    {
        string? value = Environment.GetEnvironmentVariable(name);
        return string.IsNullOrWhiteSpace(value) ? fallback : value.Trim();
    }

    public static int ReadPositive(string name, int fallback)
    {
        string? raw = Environment.GetEnvironmentVariable(name);
        return int.TryParse(raw, out int parsed) && parsed > 0
            ? parsed
            : fallback;
    }

    public static int ReadNonNegative(string name, int fallback)
    {
        string? raw = Environment.GetEnvironmentVariable(name);
        return int.TryParse(raw, out int parsed) && parsed >= 0
            ? parsed
            : fallback;
    }

    public static ulong ReadUInt64(string name, ulong fallback)
    {
        string? raw = Environment.GetEnvironmentVariable(name);
        return ulong.TryParse(raw, out ulong parsed) ? parsed : fallback;
    }

    public static int ReadByteSize(string name, int fallback)
    {
        string? raw = Environment.GetEnvironmentVariable(name);
        if (string.IsNullOrWhiteSpace(raw))
            return fallback;

        string token = raw.Trim();
        int multiplier = 1;
        char suffix = token[^1];
        if (!char.IsDigit(suffix))
        {
            token = token[..^1];
            multiplier = char.ToLowerInvariant(suffix) switch
            {
                'b' => 1,
                'k' => 1024,
                'm' => 1024 * 1024,
                'g' => 1024 * 1024 * 1024,
                _ => 0,
            };
        }

        if (multiplier <= 0 || !int.TryParse(token, out int parsed) || parsed <= 0)
            return fallback;

        long bytes = (long)parsed * multiplier;
        return bytes > int.MaxValue ? int.MaxValue : (int)bytes;
    }

    public static bool ReadBool(string name, bool fallback)
    {
        string? raw = Environment.GetEnvironmentVariable(name);
        if (string.IsNullOrWhiteSpace(raw))
            return fallback;
        return int.TryParse(raw.Trim(), out int parsed) ? parsed != 0 : fallback;
    }
}
