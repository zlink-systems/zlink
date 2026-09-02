/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import kotlinx.coroutines.future.await
import kotlinx.coroutines.runBlocking
import systems.zlink.contracts.messaging.SendSubmitOperation
import kotlin.test.Test
import kotlin.test.assertEquals

class CompletionStageCoroutineContractTest {
    @Test
    fun completionStageIsTheSharedCoroutineAwaitBoundary() = runBlocking {
        assertEquals(
            CompletionStage::class.java,
            SendSubmitOperation::class.java.getMethod("submit").returnType,
        )
        assertEquals("completed", CompletableFuture.completedFuture(
            "completed").await())
    }
}
