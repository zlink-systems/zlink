// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

public sealed partial class Message
{
    private const int PoolCapacity = 256;

    [ThreadStatic] private static Message[]? t_pool;

    [ThreadStatic] private static int t_poolCount;

    private int _knownSize = -1;
    private ZlinkMsg _msg;
    private bool _pooled;

    internal Message(bool init)
    {
        if (init)
            Init();
    }
}
