/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

/**
 * Builds a DEALER or ROUTER send whose admission completes asynchronously.
 *
 * <p>The binding retains the complete multipart record while its exact target
 * is back-pressured. The operation never exposes a blocking or callback
 * terminal.
 */
public interface RoutedSendOperation
  extends MessageBuilderStage<RoutedSendSubmitOperation> {
    /** Adds the first message part; consumed when Core accepts the record. */
    RoutedSendSubmitOperation message(Message part);
}
