// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal sealed class PairSocket : MessageSocketBase, IPairSocket
{
    public PairSocket(Context context)
        : base(context, SocketType.Pair)
    {
    }
}