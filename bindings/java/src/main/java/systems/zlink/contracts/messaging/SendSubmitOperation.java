/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import java.util.concurrent.CompletionStage;

/** Accepts further parts and completes one captured-target send. */
public interface SendSubmitOperation
  extends MessageBuilderStage<SendSubmitOperation> {
    /**
     * Adds another message part; consumed on a successful submit (see
     * {@link SendOperation} for the ownership contract).
     *
     * @param part the message part
     * @return this operation for chaining
     */
    SendSubmitOperation message(Message part);

    /**
     * Starts an asynchronous send and completes after the packet is admitted.
     *
     * <p>Each native attempt uses Core DONTWAIT. Immediate admission produces
     * no SEND completion. If an attempt reports BACKPRESSURED/EAGAIN, Core
     * retains only a wait token and the binding retains the packet. After
     * POLLOUT, the binding drains completion records and retries the same
     * packet only for the matching WRITABLE token.
     */
    CompletionStage<Void> submit();

    /** Blocks in Core until local send-queue admission completes. */
    void submit_sync();
}
