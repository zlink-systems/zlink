// SPDX-License-Identifier: MPL-2.0

using System.Text;

namespace Systems.Zlink;

internal static class BoundaryValidation
{
    private const int FixedUtf8MaxBytes = 255;

    public static void ValidateFixedUtf8(string value, string paramName)
    {
        if (value == null)
            throw new ArgumentNullException(paramName);
        if (value.IndexOf('\0') >= 0)
            throw new ArgumentException("Value must not contain NUL.",
                paramName);

        var byteCount = Encoding.UTF8.GetByteCount(value);
        if (byteCount == 0 || byteCount > FixedUtf8MaxBytes)
            throw new ArgumentOutOfRangeException(paramName,
                $"UTF-8 length must be between 1 and {FixedUtf8MaxBytes} bytes.");
    }

    public static void ValidateOptionalFixedUtf8(string? value, string paramName)
    {
        if (string.IsNullOrEmpty(value))
            return;

        ValidateFixedUtf8(value, paramName);
    }

    public static void ValidateNoNulString(string value, string paramName)
    {
        if (value == null)
            throw new ArgumentNullException(paramName);
        if (value.IndexOf('\0') >= 0)
            throw new ArgumentException("Value must not contain NUL.",
                paramName);
    }

    public static void ValidateTopicOrFilterUtf8(string value,
        string paramName, bool allowEmpty = true)
    {
        ValidateNoNulString(value, paramName);

        var byteCount = Encoding.UTF8.GetByteCount(value);
        if (!allowEmpty && byteCount == 0)
            throw new ArgumentException("Value must not be empty.", paramName);
        if (byteCount > FixedUtf8MaxBytes)
            throw new ArgumentOutOfRangeException(paramName,
                $"UTF-8 length must be between {(allowEmpty ? 0 : 1)} and " +
                $"{FixedUtf8MaxBytes} bytes.");
    }

    public static uint EncodeTimeoutMilliseconds(TimeSpan timeout,
        string paramName)
    {
        if (timeout == TimeSpan.Zero)
            return 0;

        var millis = timeout.TotalMilliseconds;
        if (double.IsNaN(millis) || double.IsInfinity(millis)
                                 || millis < 0 || millis > uint.MaxValue)
            throw new ArgumentOutOfRangeException(paramName);

        return (uint)Math.Ceiling(millis);
    }
}
