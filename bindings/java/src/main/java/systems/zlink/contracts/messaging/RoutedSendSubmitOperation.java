/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import java.time.Duration;
import java.util.concurrent.CompletionStage;

/** Accepts further parts and starts a Core asynchronous routed send. */
public interface RoutedSendSubmitOperation
  extends MessageBuilderStage<RoutedSendSubmitOperation> {
    /** Adds another message part; consumed when Core accepts the record. */
    RoutedSendSubmitOperation message(Message part);

    /** Sets the per-operation Core deadline; {@code null} or zero means none. */
    RoutedSendSubmitOperation timeout(Duration timeout);

    /**
     * Starts the operation without blocking the calling thread.
     *
     * @return a stage completed when Core accepts the complete record
     */
    CompletionStage<Void> submit();
}
