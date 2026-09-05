// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal static class EnumValidation
{
    private const SocketEvent ValidSocketEvents = SocketEvent.All;

    private const PollEventFlags ValidPollEvents = PollEventFlags.PollIn
                                                   | PollEventFlags.PollOut
                                                   | PollEventFlags.PollErr
                                                   | PollEventFlags.PollPri
                                                   | PollEventFlags.PollCompletion;

    internal static void EnsureContextOption(ContextOption option,
        string paramName)
    {
        if (!Enum.IsDefined(option))
            throw new ArgumentOutOfRangeException(paramName, option,
                "Unknown context option.");
    }

    internal static void EnsureSocketEvents(SocketEvent events, string paramName)
    {
        if (((ulong)events & ~(ulong)ValidSocketEvents) != 0)
            throw new ArgumentOutOfRangeException(paramName, events,
                "Unknown socket monitor event flag.");
    }

    internal static void EnsureMonitorPollEvents(PollEventFlags events)
    {
        if ((events & ~PollEventFlags.PollIn) != 0)
            throw new ZlinkConfigException(ConfigResult.InvalidArgument,
                (int)ErrorCode.EInval);
    }

    internal static void EnsurePollEvents(PollEventFlags events, string paramName)
    {
        if (((int)events & ~(int)ValidPollEvents) != 0)
            throw new ArgumentOutOfRangeException(paramName, events,
                "Unknown poll event flag.");
    }
}