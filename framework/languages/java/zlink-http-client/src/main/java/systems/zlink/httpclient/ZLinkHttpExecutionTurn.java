/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient;

import java.util.concurrent.CompletionStage;

public interface ZLinkHttpExecutionTurn {
    <T> CompletionStage<T> async(CompletionStage<T> operation);

    <T> CompletionStage<T> yield(CompletionStage<T> operation);
}
