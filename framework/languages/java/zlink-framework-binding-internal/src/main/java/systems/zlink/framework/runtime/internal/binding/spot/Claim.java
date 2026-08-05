/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import systems.zlink.contracts.sockets.RecvFlags;

/** An exclusive claim over one owner's pending messages, drained into a batch. */
public interface Claim extends AutoCloseable {
    /** Returns whether this claim is still valid (not yet fully drained). */
    boolean valid();

    /** Drains this claim's pending messages into the given receive batch. */
    ClaimRecvResult recvBatch(ReceiveBatch batch, RecvFlags flags);

    /** Releases the claim without draining it. */
    void release();

    @Override
    void close();
}
