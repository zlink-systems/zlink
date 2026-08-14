/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import java.util.concurrent.CompletionStage;

/** Accepts further parts and starts asynchronous exact-target admission. */
public interface RoutedSendSubmitOperation
  extends MessageBuilderStage<RoutedSendSubmitOperation> {
    /** Adds another message part; consumed when Core accepts the record. */
    RoutedSendSubmitOperation message(Message part);

    /**
     * Starts the operation without blocking the calling thread.
     *
     * @return a stage completed when Core accepts the complete record
     */
    CompletionStage<Void> submit();
}
