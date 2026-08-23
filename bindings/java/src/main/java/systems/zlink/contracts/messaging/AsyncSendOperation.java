/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

/** Builds a PAIR send whose terminal is completed by Core send completion. */
public interface AsyncSendOperation
  extends MessageBuilderStage<AsyncSendSubmitOperation> {
    /** Adds the first message part. */
    AsyncSendSubmitOperation message(Message part);
}
