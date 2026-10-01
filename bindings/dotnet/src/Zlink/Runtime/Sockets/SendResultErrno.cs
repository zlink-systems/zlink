// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal static class SendResultErrno
{
    public static SendResult? TryMap(SubmitResult result)
    {
        return result switch
        {
            SubmitResult.Backpressured => SendResult.Backpressured,
            _ => null
        };
    }
}
