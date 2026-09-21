/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient;

import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;

import java.util.concurrent.CompletionStage;

public final class ZLinkFrameworkHttpExecutionTurn implements ZLinkHttpExecutionTurn {
    @Override
    public <T> CompletionStage<T> async(CompletionStage<T> operation) {
        return ZLinkSerialExecutionQueue.manageCurrent(operation);
    }

    @Override
    public <T> CompletionStage<T> yield(CompletionStage<T> operation) {
        return ZLinkSerialExecutionQueue.yieldCurrent(operation);
    }
}
