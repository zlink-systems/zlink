package systems.zlink.samples.kotlin.shoppingmall.client

import java.nio.file.Files
import java.nio.file.Path
import java.util.Properties

object ClientApplication {
    suspend fun run(configPath: String) {
        val properties = Properties().apply {
            Files.newBufferedReader(Path.of(configPath)).use(::load)
        }
        fun required(name: String): String =
            requireNotNull(properties.getProperty(name)?.takeIf(String::isNotBlank)) { "$name is required" }
        ShoppingMallClientScenario(
            required("sample.apiAHttpUrl"),
            required("sample.apiBHttpUrl"),
            required("sample.pendingIdempotencyKey"),
            required("sample.pendingOrderId"),
            required("sample.resumeOrderId"),
            required("sample.rebuildOrderId"),
        ).run()
    }
}
