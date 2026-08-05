/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.core;

import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.PubSocket;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.StreamSocket;
import systems.zlink.contracts.sockets.SubSocket;
import systems.zlink.contracts.sockets.XPubSocket;
import systems.zlink.contracts.sockets.XSubSocket;

/**
 * A messaging context: the factory and owner of sockets.
 *
 * <p>Every socket created here is owned by the caller and must be
 * closed. Closing the context terminates anything still open under it.
 */
public interface Context extends AutoCloseable {
    /** Returns the context-wide options facade. */
    ContextOptions options();

    /** Creates a pair socket; the caller owns it and must close it. */
    PairSocket createPairSocket();

    /** Creates a dealer socket; the caller owns it and must close it. */
    DealerSocket createDealerSocket();

    /** Creates a router socket; the caller owns it and must close it. */
    RouterSocket createRouterSocket();

    /** Creates a pub socket; the caller owns it and must close it. */
    PubSocket createPubSocket();

    /** Creates a sub socket; the caller owns it and must close it. */
    SubSocket createSubSocket();

    /** Creates an xpub socket; the caller owns it and must close it. */
    XPubSocket createXPubSocket();

    /** Creates an xsub socket; the caller owns it and must close it. */
    XSubSocket createXSubSocket();

    /** Creates a stream socket; the caller owns it and must close it. */
    StreamSocket createStreamSocket();

    /**
     * Terminates the context, interrupting blocking operations on its sockets
     * without closing them.
     */
    void shutdown();

    /**
     * Recomputes automatic high-water marks for sockets configured with an
     * {@link systems.zlink.contracts.sockets.AutoHwmProfile}.
     */
    void recalculateAutoHwm();

    @Override
    void close();
}
