/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import java.time.Duration;
import java.util.concurrent.CompletionStage;

/** Completes a send when Core accepts the complete multipart record. */
public interface AsyncSendSubmitOperation
  extends MessageBuilderStage<AsyncSendSubmitOperation> {
    /** Adds another message part. */
    AsyncSendSubmitOperation message(Message part);

    /** Sets the per-operation Core deadline; {@code null} or zero means none. */
    AsyncSendSubmitOperation timeout(Duration timeout);

    /**
     * Starts the Core asynchronous send without blocking the calling thread.
     *
     * @return a stage completed when Core accepts the complete record
     */
    CompletionStage<Void> submit();
}
