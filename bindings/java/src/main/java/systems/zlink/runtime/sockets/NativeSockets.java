/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.PubSocket;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.StreamSocket;
import systems.zlink.contracts.sockets.SubSocket;
import systems.zlink.contracts.sockets.XPubSocket;
import systems.zlink.contracts.sockets.XSubSocket;

public final class NativeSockets {
    private NativeSockets() {
    }

    public static PairSocket pair(Context ctx) {
        return new NativePairSocket(ctx);
    }

    public static DealerSocket dealer(Context ctx) {
        return new NativeDealerSocket(ctx);
    }

    public static RouterSocket router(Context ctx) {
        return new NativeRouterSocket(ctx);
    }

    public static PubSocket pub(Context ctx) {
        return new NativePubSocket(ctx);
    }

    public static SubSocket sub(Context ctx) {
        return new NativeSubSocket(ctx);
    }

    public static XPubSocket xpub(Context ctx) {
        return new NativeXPubSocket(ctx);
    }

    public static XSubSocket xsub(Context ctx) {
        return new NativeXSubSocket(ctx);
    }

    public static StreamSocket stream(Context ctx) {
        return new NativeStreamSocket(ctx);
    }
}
