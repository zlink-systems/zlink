/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

/** Builds a DEALER or ROUTER send completed by Core send completion. */
public interface RoutedSendOperation
  extends MessageBuilderStage<RoutedSendSubmitOperation> {
    /** Adds the first message part; consumed when Core accepts the record. */
    RoutedSendSubmitOperation message(Message part);
}
