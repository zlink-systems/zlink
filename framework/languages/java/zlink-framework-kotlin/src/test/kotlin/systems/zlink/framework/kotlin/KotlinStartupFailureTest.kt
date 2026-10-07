package systems.zlink.framework.kotlin

import java.util.function.Supplier
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test
import org.junit.jupiter.api.assertThrows
import org.springframework.context.ApplicationContextException
import org.springframework.context.annotation.AnnotationConfigApplicationContext
import org.springframework.context.annotation.Configuration
import systems.zlink.contracts.core.RoutingId
import systems.zlink.framework.locationprovider.ZLinkLocationStore
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.framework.testkit.FakeZLinkBackendAdapterFactory

class KotlinStartupFailureTest {
    @Test
    fun routingIdConflictFailsContextRefresh() =
        runBlocking<Unit> {
            val store = ZLinkInMemoryProviderLocationStore()
            context(store, "inproc://kotlin-startup-owner").use { owner ->
                context(store, "inproc://kotlin-startup-conflict").use { conflict ->
                    owner.refresh()
                    val runtime = owner.getBean(ZLinkFrameworkRuntime::class.java)
                    withTimeout(3000) { runtime.observe().asFlow().first { runtime.isReady } }
                    assertThrows<ApplicationContextException> { conflict.refresh() }
                }
            }
        }

    @Test
    fun successfulRefreshWaitsForRuntimeStartup() {
        context(ZLinkInMemoryProviderLocationStore(), "inproc://kotlin-startup-ready").use {
            it.refresh()
            assertTrue(it.getBean(ZLinkFrameworkRuntime::class.java).isReady)
        }
    }

    private fun context(
        store: ZLinkLocationStore,
        endpoint: String,
    ): AnnotationConfigApplicationContext {
        val context = AnnotationConfigApplicationContext()
        context.registerBean(ZLinkLocationStore::class.java, Supplier { store })
        context.registerBean(
            ZLinkBackendAdapterProvider::class.java,
            Supplier { FakeZLinkBackendAdapterFactory() },
        )
        context.registerBean(
            ZLinkFrameworkConfigurer::class.java,
            Supplier {
                ZLinkFrameworkConfigurer { options ->
                    options
                        .addRouteMesh("startup")
                        .listen(endpoint)
                        .setRoutingId(RoutingId.from("kotlin-startup-conflict"))
                }
            },
        )
        context.register(StartupConfiguration::class.java)
        return context
    }

    @Configuration @EnableZLinkFramework class StartupConfiguration
}
