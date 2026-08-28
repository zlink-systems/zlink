/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

/** Builds a DEALER, ROUTER, or routed STREAM send completed by Core. */
public interface RoutedSendOperation
  extends MessageBuilderStage<RoutedSendSubmitOperation> {
    /** Adds the first message part; consumed when Core accepts the record. */
    RoutedSendSubmitOperation message(Message part);
}
