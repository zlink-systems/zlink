// SPDX-License-Identifier: MPL-2.0

using System.Globalization;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

public abstract partial class ZlinkException
{
    private const int ZlinkHausnumero = 156384712;
    private const int EnotSupFallback = ZlinkHausnumero + 1;
    private const int EprotoNoSupportFallback = ZlinkHausnumero + 2;
    private const int EnoBufsFallback = ZlinkHausnumero + 3;
    private const int EnetDownFallback = ZlinkHausnumero + 4;
    private const int EaddrInUseFallback = ZlinkHausnumero + 5;
    private const int EaddrNotAvailFallback = ZlinkHausnumero + 6;
    private const int EconnRefusedFallback = ZlinkHausnumero + 7;
    private const int EinProgressFallback = ZlinkHausnumero + 8;
    private const int EnotSockFallback = ZlinkHausnumero + 9;
    private const int EmsgSizeFallback = ZlinkHausnumero + 10;
    private const int EafNoSupportFallback = ZlinkHausnumero + 11;
    private const int EnetUnreachFallback = ZlinkHausnumero + 12;
    private const int EconnAbortedFallback = ZlinkHausnumero + 13;
    private const int EconnResetFallback = ZlinkHausnumero + 14;
    private const int EnotConnFallback = ZlinkHausnumero + 15;
    private const int EtimedOutFallback = ZlinkHausnumero + 16;
    private const int EhostUnreachFallback = ZlinkHausnumero + 17;
    private const int EnetResetFallback = ZlinkHausnumero + 18;
    private const int EshutdownFallback = ZlinkHausnumero + 22;
    private const int EfsmNative = ZlinkHausnumero + 51;
    private const int EnoCompatProtoNative = ZlinkHausnumero + 52;
    internal const int EtermNative = ZlinkHausnumero + 53;
    private const int EmThreadNative = ZlinkHausnumero + 54;

    // Core returned rc; its errno stays attached as the diagnostic value.
    internal static void ThrowConfigIfError(int rc)
    {
        if (rc != 0)
            throw new ZlinkConfigException((ConfigResult)rc,
                Systems.Zlink.Runtime.Native.NativeMethods.GetLastPInvokeError());
    }

    internal static void ThrowConnectIfError(int rc)
    {
        if (rc != 0)
            throw new ZlinkConnectException((ConnectResult)rc,
                Systems.Zlink.Runtime.Native.NativeMethods.GetLastPInvokeError());
    }

    internal static void ThrowSubmitIfError(int rc)
    {
        if (rc != 0)
            throw CreateSubmitException((SubmitResult)rc);
    }

    internal static void ThrowHandlerIfError(int rc)
    {
        if (rc != 0)
            throw CreateHandlerException((HandlerResult)rc);
    }

    internal static void ThrowCloseIfError(int rc)
    {
        if (rc != 0)
            throw CreateCloseException((CloseResult)rc);
    }

    internal static ErrorCode MapErrorCode(int errno)
    {
        return errno switch
        {
            0 => ErrorCode.None,
            (int)ErrorCode.EBusy => ErrorCode.EBusy,
            (int)ErrorCode.EIntr => ErrorCode.EIntr,
            (int)ErrorCode.EAgain or 35 => ErrorCode.EAgain,
            (int)ErrorCode.EBadf => ErrorCode.EBadf,
            (int)ErrorCode.ENomem => ErrorCode.ENomem,
            (int)ErrorCode.EAccess => ErrorCode.EAccess,
            (int)ErrorCode.EFault => ErrorCode.EFault,
            (int)ErrorCode.EInval => ErrorCode.EInval,
            (int)ErrorCode.ENotSock or 38 or EnotSockFallback =>
                ErrorCode.ENotSock,
            (int)ErrorCode.EMsgSize or 40 or EmsgSizeFallback =>
                ErrorCode.EMsgSize,
            (int)ErrorCode.EProtoNoSupport or 43 or EprotoNoSupportFallback =>
                ErrorCode.EProtoNoSupport,
            (int)ErrorCode.ENotSup or 45 or EnotSupFallback =>
                ErrorCode.ENotSup,
            (int)ErrorCode.EAfNoSupport or 47 or EafNoSupportFallback =>
                ErrorCode.EAfNoSupport,
            (int)ErrorCode.EAddrInUse or 48 or EaddrInUseFallback =>
                ErrorCode.EAddrInUse,
            (int)ErrorCode.EAddrNotAvail or 49 or EaddrNotAvailFallback =>
                ErrorCode.EAddrNotAvail,
            (int)ErrorCode.ENetDown or 50 or EnetDownFallback =>
                ErrorCode.ENetDown,
            (int)ErrorCode.ENetUnreach or 51 or EnetUnreachFallback =>
                ErrorCode.ENetUnreach,
            (int)ErrorCode.ENetReset or 52 or EnetResetFallback =>
                ErrorCode.ENetReset,
            (int)ErrorCode.EConnAborted or 53 or EconnAbortedFallback =>
                ErrorCode.EConnAborted,
            (int)ErrorCode.EConnReset or 54 or EconnResetFallback =>
                ErrorCode.EConnReset,
            (int)ErrorCode.ENoBufs or 55 or EnoBufsFallback =>
                ErrorCode.ENoBufs,
            (int)ErrorCode.ENotConn or 57 or EnotConnFallback =>
                ErrorCode.ENotConn,
            (int)ErrorCode.ETimedOut or 60 or EtimedOutFallback =>
                ErrorCode.ETimedOut,
            (int)ErrorCode.EConnRefused or 61 or EconnRefusedFallback =>
                ErrorCode.EConnRefused,
            (int)ErrorCode.EHostUnreach or 65 or EhostUnreachFallback =>
                ErrorCode.EHostUnreach,
            (int)ErrorCode.EShutdown or 58 or 10058 or EshutdownFallback =>
                ErrorCode.EShutdown,
            (int)ErrorCode.EInProgress or 36 or EinProgressFallback =>
                ErrorCode.EInProgress,
            EfsmNative => ErrorCode.Efsm,
            EnoCompatProtoNative => ErrorCode.EnoCompatProto,
            EtermNative => ErrorCode.Eterm,
            EmThreadNative => ErrorCode.EmThread,
            _ => ErrorCode.Unknown
        };
    }

    internal static bool IsTerminationError(int errno)
    {
        var error = MapErrorCode(errno);
        return error is ErrorCode.EShutdown or ErrorCode.Eterm;
    }

    // A WRITABLE SEND_TERMINAL record carries no submit result, only errno.
    internal static ZlinkSubmitException CreateSubmitException(int errno)
    {
        return new ZlinkSubmitException(MapSubmitResult(errno), errno);
    }

    internal static ZlinkSubmitException CreateSubmitException(SubmitResult result)
    {
        return new ZlinkSubmitException(result, NativeMethods.GetLastPInvokeError());
    }

    internal static ZlinkRecvException CreateRecvException(RecvResult result)
    {
        return new ZlinkRecvException(result, NativeMethods.GetLastPInvokeError());
    }

    internal static ZlinkHandlerException CreateHandlerException(HandlerResult result)
    {
        return new ZlinkHandlerException(result, NativeMethods.GetLastPInvokeError());
    }

    internal static ZlinkCloseException CreateCloseException(CloseResult result)
    {
        return new ZlinkCloseException(result, NativeMethods.GetLastPInvokeError());
    }

    internal static ZlinkBindException CreateBindException(BindResult result)
    {
        return new ZlinkBindException(result, NativeMethods.GetLastPInvokeError());
    }

    internal static ZlinkConnectException CreateConnectException(ConnectResult result)
    {
        return new ZlinkConnectException(result, NativeMethods.GetLastPInvokeError());
    }

    internal static ZlinkConfigException CreateConfigException(int errno)
    {
        return new ZlinkConfigException(MapConfigResult(errno), errno);
    }

    internal static ZlinkConfigException CreateConfigException(ConfigResult result)
    {
        return new ZlinkConfigException(result, NativeMethods.GetLastPInvokeError());
    }

    private static string BuildMessage(int code, int nativeErrno)
    {
        return nativeErrno == 0
            ? $"zlink error code {code}"
            : string.Create(CultureInfo.InvariantCulture, $"zlink error code {code} (errno {nativeErrno})");
    }

    private static SubmitResult MapSubmitResult(int errno)
    {
        return errno switch
        {
            0 => SubmitResult.Ok,
            11 or 35 or 10035 => SubmitResult.Backpressured,
            13 => SubmitResult.NotAdmitted,
            107 or 113 or 111 or 110 or 10057 or 10060 or 10065 =>
                SubmitResult.NotConnected,
            2 or 3 => SubmitResult.NotFound,
            58 or 108 or 10058 or EshutdownFallback or 156384765 =>
                SubmitResult.Terminated,
            9 or 88 => SubmitResult.InvalidHandle,
            22 => SubmitResult.InvalidArgument,
            95 or 93 or 97 => SubmitResult.NotSupported,
            16 => SubmitResult.InvalidState,
            156384766 => SubmitResult.ThreadViolation,
            12 or 105 => SubmitResult.OutOfMemory,
            _ => SubmitResult.InternalError
        };
    }

    private static ConfigResult MapConfigResult(int errno)
    {
        return errno switch
        {
            0 => ConfigResult.InternalError,
            9 or 88 => ConfigResult.InvalidHandle,
            22 => ConfigResult.InvalidArgument,
            95 or 93 or 97 => ConfigResult.NotSupported,
            16 or 108 => ConfigResult.InvalidState,
            2 or 3 => ConfigResult.NotFound,
            _ => ConfigResult.InternalError
        };
    }

}
