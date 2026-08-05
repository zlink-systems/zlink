package systems.zlink.framework.kotlin

import java.util.concurrent.CompletableFuture
import kotlinx.coroutines.flow.toList
import kotlinx.coroutines.runBlocking
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Test
import systems.zlink.framework.locations.ZLinkLocationPage
import systems.zlink.framework.locations.ZLinkPageRequest

class KotlinLocationExtensionsTest {
    @Test
    fun `java completion stages can be awaited without a duplicate kotlin provider spi`() = runBlocking {
        val result = CompletableFuture.completedFuture(42).await()

        assertEquals(42, result)
    }

    @Test
    fun `locationPages emits every page item in order`() = runBlocking {
        val pages = mapOf(
            "" to ZLinkLocationPage(listOf("a", "b"), "2"),
            "2" to ZLinkLocationPage(listOf("c"), null),
        )

        val items = locationPages(ZLinkPageRequest(2, null)) { request ->
            java.util.concurrent.CompletableFuture.completedFuture(pages[request.continuationToken() ?: ""])
        }.toList()

        assertEquals(listOf("a", "b", "c"), items)
    }
}
