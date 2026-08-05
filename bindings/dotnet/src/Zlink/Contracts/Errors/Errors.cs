// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Base class for all exceptions thrown by the zlink bindings.
/// </summary>
public abstract partial class ZlinkException : Exception
{
    /// <summary>
    ///     Creates an exception with the given zlink code and no underlying errno.
    /// </summary>
    protected ZlinkException(int code)
        : this(code, 0)
    {
    }

    /// <summary>
    ///     Creates an exception with the given zlink code and the native errno that
    ///     produced it.
    /// </summary>
    protected ZlinkException(int code, int nativeErrno)
        : base(BuildMessage(code, nativeErrno))
    {
        Code = code;
        NativeErrno = nativeErrno;
    }

    /// <summary>
    ///     Gets the zlink result code that classifies this failure.
    /// </summary>
    public int Code { get; }

    /// <summary>
    ///     Gets the native errno that produced this failure, or 0 when none.
    /// </summary>
    public int NativeErrno { get; }

    /// <summary>
    ///     Rejects success codes for public exception construction.
    /// </summary>
    protected static TErrorCode ValidatePublicErrorCode<TErrorCode>(
        TErrorCode result)
        where TErrorCode : struct, Enum
    {
        if (Convert.ToInt32(result) == 0)
            throw new ArgumentOutOfRangeException(nameof(result),
                "An exception result code must not be Ok.");
        return result;
    }
}
