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
        : base((int)result, 0)
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
