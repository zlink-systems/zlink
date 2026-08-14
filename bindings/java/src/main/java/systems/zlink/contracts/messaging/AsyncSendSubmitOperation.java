/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import java.util.concurrent.CompletionStage;

/** Completes a send when Core accepts the complete multipart record. */
public interface AsyncSendSubmitOperation
  extends MessageBuilderStage<AsyncSendSubmitOperation> {
    /** Adds another message part. */
    AsyncSendSubmitOperation message(Message part);

    /**
     * Starts admission without blocking the calling thread.
     *
     * @return a stage completed when Core accepts the complete record
     */
    CompletionStage<Void> submit();
}
