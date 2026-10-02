// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal enum CloseResult
{
    Ok = 0,
    Busy = 401,
    Shutdown = 402,
    InvalidHandle = 403,
    InternalError = 404
}

internal enum BindResult
{
    Ok = 0,
    InvalidArgument = 501,
    AddrInUse = 502,
    NotSupported = 503,
    InvalidHandle = 504,
    InternalError = 505
}

internal enum ConnectResult
{
    Ok = 0,
    InvalidArgument = 601,
    NotSupported = 602,
    InvalidHandle = 603,
    InternalError = 604,
    NotFound = 605,
    Conflict = 606,
    Busy = 607
}

internal enum ConfigResult
{
    Ok = 0,
    InvalidHandle = 701,
    InvalidArgument = 702,
    NotSupported = 703,
    InternalError = 704,
    InvalidState = 705,
    NotFound = 706,
    Conflict = 707,
    BufferTooSmall = 708,
    Busy = 709
}

internal static class ErrorCode
{
    internal static readonly int EBusy;
    internal static readonly int EAgain;
    internal static readonly int EBadf;
    internal static readonly int ENoent;
    internal static readonly int ESrch;
    internal static readonly int EIo;
    internal static readonly int EAccess;
    internal static readonly int EExist;
    internal static readonly int EInval;
    internal static readonly int ENotSock;
    internal static readonly int EProtoNoSupport;
    internal static readonly int EProto;
    internal static readonly int ENotSup;
    internal static readonly int EAfNoSupport;
    internal static readonly int ENotConn;
    internal static readonly int ETimedOut;
    internal static readonly int EShutdown;
    internal static readonly int Efsm;
    internal static readonly int Eterm;

    static ErrorCode()
    {
        string platform = OperatingSystem.IsWindows() ? "windows"
            : OperatingSystem.IsMacOS() ? "darwin"
            : OperatingSystem.IsLinux() ? "linux"
            : throw new PlatformNotSupportedException();
        using Stream stream = typeof(ErrorCode).Assembly.GetManifestResourceStream(
            $"Zlink.Errno.{platform}.properties")
            ?? throw new InvalidOperationException("Core errno resource is missing.");
        using var reader = new StreamReader(stream);
        var values = new Dictionary<string, int>(StringComparer.Ordinal);
        while (reader.ReadLine() is { } line)
        {
            if (line.Length == 0 || line.StartsWith('#'))
                continue;
            int separator = line.IndexOf('=');
            values.Add(line[..separator], int.Parse(line[(separator + 1)..],
                System.Globalization.CultureInfo.InvariantCulture));
        }
        EBusy = values["EBUSY"];
        EAgain = values["EAGAIN"];
        EBadf = values["EBADF"];
        ENoent = values["ENOENT"];
        ESrch = values["ESRCH"];
        EIo = values["EIO"];
        EAccess = values["EACCES"];
        EExist = values["EEXIST"];
        EInval = values["EINVAL"];
        ENotSock = values["ENOTSOCK"];
        EProtoNoSupport = values["EPROTONOSUPPORT"];
        EProto = values["EPROTO"];
        ENotSup = values["ENOTSUP"];
        EAfNoSupport = values["EAFNOSUPPORT"];
        ENotConn = values["ENOTCONN"];
        ETimedOut = values["ETIMEDOUT"];
        EShutdown = values["ESHUTDOWN"];
        Efsm = values["EFSM"];
        Eterm = values["ETERM"];
    }
}
