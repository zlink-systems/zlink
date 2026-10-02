// SPDX-License-Identifier: MPL-2.0

using System.Globalization;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

public abstract partial class ZlinkException
{
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

    internal static bool IsTerminationError(int errno)
    {
        return errno == ErrorCode.EShutdown || errno == ErrorCode.Eterm;
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

    private static ConfigResult MapConfigResult(int errno)
    {
        return errno switch
        {
            0 => ConfigResult.InternalError,
            var value when value == ErrorCode.EBadf || value == ErrorCode.ENotSock => ConfigResult.InvalidHandle,
            var value when value == ErrorCode.EInval => ConfigResult.InvalidArgument,
            var value when value == ErrorCode.ENotSup
                           || value == ErrorCode.EProtoNoSupport
                           || value == ErrorCode.EAfNoSupport => ConfigResult.NotSupported,
            var value when value == ErrorCode.EBusy || value == ErrorCode.EShutdown => ConfigResult.InvalidState,
            var value when value == ErrorCode.ENoent || value == ErrorCode.ESrch => ConfigResult.NotFound,
            _ => ConfigResult.InternalError
        };
    }

}
