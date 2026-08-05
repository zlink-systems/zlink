/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;

public final class ZLinkFrameworkHttpExecutionTurn implements ZLinkHttpExecutionTurn {
    @Override
    public <T> CompletionStage<T> async(CompletionStage<T> operation) {
        return ZLinkAsyncSerialQueue.manageCurrent(operation);
    }

    @Override
    public <T> CompletionStage<T> yield(CompletionStage<T> operation) {
        return ZLinkAsyncSerialQueue.yieldCurrent(operation);
    }
}
