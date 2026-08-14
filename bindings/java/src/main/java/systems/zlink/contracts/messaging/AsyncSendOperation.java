/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

/**
 * Builds a multipart send whose terminal waits asynchronously for local Core
 * admission.
 */
public interface AsyncSendOperation
  extends MessageBuilderStage<AsyncSendSubmitOperation> {
    /** Adds the first message part. */
    AsyncSendSubmitOperation message(Message part);
}
