/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract

import java.util.concurrent.CompletableFuture
import kotlinx.coroutines.future.await
import kotlinx.coroutines.runBlocking
import systems.zlink.contracts.messaging.Message
import systems.zlink.contracts.messaging.RequestSubmission
import systems.zlink.contracts.messaging.SendSubmitOperation
import systems.zlink.contracts.messaging.SendSubmission
import systems.zlink.contracts.sockets.SubmitResult
import kotlin.test.Test
import kotlin.test.assertEquals

class CompletionStageCoroutineContractTest {
    @Test
    fun completionStageIsTheSharedCoroutineAwaitBoundary() = runBlocking {
        assertEquals(
            SendSubmission::class.java,
            SendSubmitOperation::class.java.getMethod("submit").returnType,
        )
        assertEquals("completed", CompletableFuture.completedFuture(
            "completed").await())

        val send = object : SendSubmission {
            override fun result() = SubmitResult.OK
            override fun admitted() = completedVoid()
        }
        send.admitted().await()

        val request = object : RequestSubmission {
            override fun result() = SubmitResult.OK
            override fun admitted() = completedVoid()
            override fun reply() = CompletableFuture.completedFuture(
                emptyList<Message>())
        }
        request.admitted().await()
        assertEquals(emptyList(), request.reply().await())
    }

    private fun completedVoid() = CompletableFuture<Void>().also {
        it.complete(null)
    }
}
