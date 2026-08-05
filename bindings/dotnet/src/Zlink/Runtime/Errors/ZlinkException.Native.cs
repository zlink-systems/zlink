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
    private const int EfsmNative = ZlinkHausnumero + 51;
    private const int EnoCompatProtoNative = ZlinkHausnumero + 52;
    private const int EtermNative = ZlinkHausnumero + 53;
    private const int EmThreadNative = ZlinkHausnumero + 54;

    internal static void ThrowConfigIfError(int rc)
    {
        if (rc != 0)
            throw CreateConfigException((ConfigResult)rc);
    }

    internal static void ThrowConnectIfError(int rc)
    {
        if (rc != 0)
            throw CreateConnectException((ConnectResult)rc);
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
            (int)ErrorCode.EShutdown => ErrorCode.EShutdown,
            (int)ErrorCode.EInProgress or 36 or EinProgressFallback =>
                ErrorCode.EInProgress,
            EfsmNative => ErrorCode.Efsm,
            EnoCompatProtoNative => ErrorCode.EnoCompatProto,
            EtermNative => ErrorCode.Eterm,
            EmThreadNative => ErrorCode.EmThread,
            _ => ErrorCode.Unknown
        };
    }

    internal static ZlinkSubmitException CreateSubmitException(int errno)
    {
        return new ZlinkSubmitException(MapSubmitResult(errno), errno);
    }

    internal static ZlinkSubmitException CreateSubmitException(SubmitResult result)
    {
        return new ZlinkSubmitException(result);
    }

    internal static ZlinkRequestException CreateRequestException(int errno)
    {
        return new ZlinkRequestException(MapRequestResult(errno), errno);
    }

    internal static ZlinkRequestException CreateRequestException(RequestResult result)
    {
        return new ZlinkRequestException(result);
    }

    internal static ZlinkRecvException CreateRecvException(int errno)
    {
        return new ZlinkRecvException(MapRecvResult(errno), errno);
    }

    internal static ZlinkRecvException CreateRecvException(RecvResult result)
    {
        return new ZlinkRecvException(result);
    }

    internal static ZlinkHandlerException CreateHandlerException(int errno)
    {
        return new ZlinkHandlerException(MapHandlerResult(errno), errno);
    }

    internal static ZlinkHandlerException CreateHandlerException(HandlerResult result)
    {
        return new ZlinkHandlerException(result);
    }

    internal static ZlinkCloseException CreateCloseException(int errno)
    {
        return new ZlinkCloseException(MapCloseResult(errno), errno);
    }

    internal static ZlinkCloseException CreateCloseException(CloseResult result)
    {
        return new ZlinkCloseException(result);
    }

    internal static ZlinkBindException CreateBindException(int errno)
    {
        return new ZlinkBindException(MapBindResult(errno), errno);
    }

    internal static ZlinkBindException CreateBindException(BindResult result)
    {
        return new ZlinkBindException(result);
    }

    internal static ZlinkConnectException CreateConnectException(int errno)
    {
        return new ZlinkConnectException(MapConnectResult(errno), errno);
    }

    internal static ZlinkConnectException CreateConnectException(ConnectResult result)
    {
        return new ZlinkConnectException(result);
    }

    internal static ZlinkConfigException CreateConfigException(int errno)
    {
        return new ZlinkConfigException(MapConfigResult(errno), errno);
    }

    internal static ZlinkConfigException CreateConfigException(ConfigResult result)
    {
        return new ZlinkConfigException(result);
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
            156384765 => SubmitResult.Terminated,
            9 or 88 => SubmitResult.InvalidHandle,
            22 => SubmitResult.InvalidArgument,
            95 or 93 or 97 => SubmitResult.NotSupported,
            16 => SubmitResult.InvalidState,
            156384766 => SubmitResult.ThreadViolation,
            12 or 105 => SubmitResult.OutOfMemory,
            _ => SubmitResult.InternalError
        };
    }

    private static RequestResult MapRequestResult(int errno)
    {
        return errno switch
        {
            0 => RequestResult.Ok,
            60 or 110 or 10060 => RequestResult.TimedOut,
            3 or 2 => RequestResult.NotFound,
            156384765 => RequestResult.Terminated,
            104 => RequestResult.ProtocolError,
            12 or 105 => RequestResult.InternalError,
            1 or 13 => RequestResult.Rejected,
            17 => RequestResult.Conflict,
            16 => RequestResult.Busy,
            107 or 113 or 111 or 10057 or 10065 =>
                RequestResult.NotConnected,
            22 => RequestResult.InvalidArgument,
            108 => RequestResult.InvalidState,
            95 or 93 or 97 or 156384766 => RequestResult.NotSupported,
            _ => RequestResult.InternalError
        };
    }

    private static RecvResult MapRecvResult(int errno)
    {
        return errno switch
        {
            0 => RecvResult.Ok,
            11 or 35 or 10035 => RecvResult.NoData,
            16 => RecvResult.Busy,
            156384765 => RecvResult.Terminated,
            9 or 14 or 88 => RecvResult.InvalidHandle,
            95 or 93 or 97 => RecvResult.NotSupported,
            _ => RecvResult.InternalError
        };
    }

    private static HandlerResult MapHandlerResult(int errno)
    {
        return errno switch
        {
            0 => HandlerResult.Ok,
            22 => HandlerResult.InvalidArgument,
            16 => HandlerResult.Busy,
            95 or 93 or 97 => HandlerResult.NotSupported,
            35 => HandlerResult.Deadlock,
            9 or 88 => HandlerResult.InvalidHandle,
            _ => HandlerResult.InternalError
        };
    }

    private static CloseResult MapCloseResult(int errno)
    {
        return errno switch
        {
            0 => CloseResult.Ok,
            16 => CloseResult.Busy,
            108 => CloseResult.Shutdown,
            9 or 88 => CloseResult.InvalidHandle,
            _ => CloseResult.InternalError
        };
    }

    private static BindResult MapBindResult(int errno)
    {
        return errno switch
        {
            0 => BindResult.Ok,
            22 => BindResult.InvalidArgument,
            98 => BindResult.AddrInUse,
            95 or 93 or 97 => BindResult.NotSupported,
            9 or 88 => BindResult.InvalidHandle,
            _ => BindResult.InternalError
        };
    }

    private static ConnectResult MapConnectResult(int errno)
    {
        return errno switch
        {
            0 => ConnectResult.Ok,
            22 => ConnectResult.InvalidArgument,
            95 or 93 or 97 => ConnectResult.NotSupported,
            9 or 88 => ConnectResult.InvalidHandle,
            2 or 3 => ConnectResult.NotFound,
            98 => ConnectResult.Conflict,
            16 => ConnectResult.Busy,
            _ => ConnectResult.InternalError
        };
    }

    private static ConfigResult MapConfigResult(int errno)
    {
        return errno switch
        {
            0 => ConfigResult.Ok,
            9 or 88 => ConfigResult.InvalidHandle,
            22 => ConfigResult.InvalidArgument,
            95 or 93 or 97 => ConfigResult.NotSupported,
            16 or 108 => ConfigResult.InvalidState,
            2 or 3 => ConfigResult.NotFound,
            _ => ConfigResult.InternalError
        };
    }

}
