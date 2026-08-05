// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal static class SendResultErrno
{
    private const int EAgain = 11;
    private const int EWouldBlockWin = 10035;
    private const int ENotConn = 107;
    private const int ENotConnWin = 10057;
    private const int EHostUnreach = 113;
    private const int EHostUnreachWin = 10065;
    private const int ETimedOut = 110;
    private const int ETimedOutWin = 10060;

    public static SendResult? TryMap(SubmitResult result)
    {
        return result switch
        {
            SubmitResult.Backpressured => SendResult.Backpressured,
            SubmitResult.NotConnected => SendResult.NotReady,
            SubmitResult.NotFound => SendResult.NotReady,
            _ => null
        };
    }

    public static SendResult? TryMap(int errno)
    {
        return errno switch
        {
            EAgain => SendResult.Backpressured,
            EWouldBlockWin => SendResult.Backpressured,
            ENotConn => SendResult.NotReady,
            ENotConnWin => SendResult.NotReady,
            EHostUnreach => SendResult.NotReady,
            EHostUnreachWin => SendResult.NotReady,
            ETimedOut => SendResult.NotReady,
            ETimedOutWin => SendResult.NotReady,
            _ => null
        };
    }

    public static SendResult? TryMapCurrent()
    {
        return TryMap(NativeMethods.zlink_errno());
    }
}
