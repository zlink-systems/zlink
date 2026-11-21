package systems.zlink.framework.kotlin

import java.lang.reflect.Proxy
import java.util.Optional
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.yield
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertSame
import org.junit.jupiter.api.Test
import systems.zlink.framework.streams.ZLinkDisconnectReason
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkStreamError
import systems.zlink.framework.streams.ZLinkStreamSessionError

class KotlinStreamDisconnectReasonTest {
    @Test
    fun `suspending error callback reads the same optional reason after suspension`() {
        val context =
            Proxy.newProxyInstance(
                ZLinkSessionContext::class.java.classLoader,
                arrayOf(ZLinkSessionContext::class.java),
            ) { _, method, _ ->
                error("unexpected context access: ${method.name}")
            } as ZLinkSessionContext
        val values = ZLinkDisconnectReason.values().map { Optional.of(it) } + Optional.empty()
        for (reason in values) {
            val expected =
                ZLinkStreamError(ZLinkStreamSessionError.TRANSPORT_ERROR, "closed", reason)
            var callbacks = 0
            val session =
                object : ZLinkSuspendingSession() {
                    override fun context(): ZLinkSessionContext = context

                    override suspend fun onErrorSuspending(error: ZLinkStreamError) {
                        yield()
                        callbacks++
                        assertSame(expected, error)
                        assertEquals(reason, error.disconnectReason())
                    }
                }
            session.onError(expected).toCompletableFuture().get(5, TimeUnit.SECONDS)
            assertEquals(1, callbacks)
        }
    }
}
