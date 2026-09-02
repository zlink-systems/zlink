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

    /** Submits with Core DONTWAIT and completes after native admission. */
    CompletionStage<Void> submit();

    /** Blocks in Core until local send-queue admission completes. */
    void submit_sync();
}
