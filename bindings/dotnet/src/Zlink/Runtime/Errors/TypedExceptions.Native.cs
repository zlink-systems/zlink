// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

public sealed partial class ZlinkSubmitException
{
    internal ZlinkSubmitException(SubmitResult result)
        : base((int)result, 0)
    {
        Result = (ErrorCode)(int)result;
    }

    internal ZlinkSubmitException(SubmitResult result, int nativeErrno)
        : base((int)result, nativeErrno)
    {
        Result = (ErrorCode)(int)result;
    }
}

public sealed partial class ZlinkRequestException
{
    internal ZlinkRequestException(RequestResult result)
        : base((int)result, result switch
        {
            RequestResult.TimedOut => (int)global::Systems.Zlink.ErrorCode.ETimedOut,
            RequestResult.NotFound => (int)global::Systems.Zlink.ErrorCode.ENoent,
            RequestResult.Terminated => (int)global::Systems.Zlink.ErrorCode.Eterm,
            RequestResult.ProtocolError => (int)global::Systems.Zlink.ErrorCode.EProto,
            RequestResult.Rejected => (int)global::Systems.Zlink.ErrorCode.EAccess,
            RequestResult.Conflict => (int)global::Systems.Zlink.ErrorCode.EExist,
            RequestResult.Busy => (int)global::Systems.Zlink.ErrorCode.EBusy,
            RequestResult.NotConnected => (int)global::Systems.Zlink.ErrorCode.ENotConn,
            RequestResult.InvalidArgument => (int)global::Systems.Zlink.ErrorCode.EInval,
            RequestResult.InvalidState => (int)global::Systems.Zlink.ErrorCode.Efsm,
            RequestResult.NotSupported => (int)global::Systems.Zlink.ErrorCode.ENotSup,
            RequestResult.Backpressured => (int)global::Systems.Zlink.ErrorCode.EAgain,
            _ => 0
        })
    {
        Result = (ErrorCode)(int)result;
    }

    internal ZlinkRequestException(RequestResult result, int nativeErrno)
        : base((int)result, nativeErrno)
    {
        Result = (ErrorCode)(int)result;
    }
}

public sealed partial class ZlinkRecvException
{
    internal ZlinkRecvException(RecvResult result)
        : base((int)result, 0)
    {
        Result = (ErrorCode)(int)result;
    }

    internal ZlinkRecvException(RecvResult result, int nativeErrno)
        : base((int)result, nativeErrno)
    {
        Result = (ErrorCode)(int)result;
    }
}

public sealed partial class ZlinkHandlerException
{
    internal ZlinkHandlerException(HandlerResult result, int nativeErrno)
        : base((int)result, nativeErrno)
    {
        Result = (ErrorCode)(int)result;
    }

    internal ZlinkHandlerException(HandlerResult result)
        : base((int)result, 0)
    {
        Result = (ErrorCode)(int)result;
    }
}

public sealed partial class ZlinkCloseException
{
    internal ZlinkCloseException(CloseResult result, int nativeErrno)
        : base((int)result, nativeErrno)
    {
        Result = (ErrorCode)(int)result;
    }

    internal ZlinkCloseException(CloseResult result)
        : base((int)result, 0)
    {
        Result = (ErrorCode)(int)result;
    }
}

public sealed partial class ZlinkBindException
{
    internal ZlinkBindException(BindResult result, int nativeErrno)
        : base((int)result, nativeErrno)
    {
        Result = (ErrorCode)(int)result;
    }

    internal ZlinkBindException(BindResult result)
        : base((int)result, 0)
    {
        Result = (ErrorCode)(int)result;
    }
}

public sealed partial class ZlinkConnectException
{
    internal ZlinkConnectException(ConnectResult result, int nativeErrno)
        : base((int)result, nativeErrno)
    {
        Result = (ErrorCode)(int)result;
    }

    internal ZlinkConnectException(ConnectResult result)
        : base((int)result, 0)
    {
        Result = (ErrorCode)(int)result;
    }
}

public sealed partial class ZlinkConfigException
{
    internal ZlinkConfigException(ConfigResult result, int nativeErrno)
        : base((int)result, nativeErrno)
    {
        Result = (ErrorCode)(int)result;
    }

    internal ZlinkConfigException(ConfigResult result)
        : base((int)result, 0)
    {
        Result = (ErrorCode)(int)result;
    }
}
