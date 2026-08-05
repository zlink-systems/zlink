// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

public readonly partial struct PollEvent
{
    internal PollEvent(PollSourceKind sourceKind, nuint slot,
        PollEventFlags revents, int fd)
    {
        SourceKind = sourceKind;
        Slot = slot;
        Revents = revents;
        Fd = fd;
    }
}