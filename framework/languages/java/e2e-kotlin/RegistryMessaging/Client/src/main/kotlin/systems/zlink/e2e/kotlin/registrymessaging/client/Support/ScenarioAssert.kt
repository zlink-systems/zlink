package Support

import systems.zlink.e2e.kotlin.registrymessaging.client.Support
import java.time.Duration
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.ProfileRes

object ScenarioAssert {
    fun that(condition: Boolean, message: String) {
        if (!condition) {
            throw IllegalStateException(message)
        }
    }

    fun countNewEvidence(after: Collection<String>, before: Collection<String>, prefix: String, marker: String): Int {
        val beforeCount = before.count { it.contains(prefix) && it.contains(marker) }
        val afterCount = after.count { it.contains(prefix) && it.contains(marker) }
        return afterCount - beforeCount
    }

    fun requestProfileEventually(role: HttpJson, marker: String): ProfileRes {
        return retryUntil("profile request $marker") {
            role.post("/profile/request", ProfileReq(marker))
        }
    }

    fun <T> retryUntil(description: String, action: () -> T): T {
        val deadline = System.nanoTime() + Duration.ofSeconds(15).toNanos()
        var last: RuntimeException? = null
        while (System.nanoTime() < deadline) {
            try {
                return action()
            } catch (error: RuntimeException) {
                last = error
                Thread.sleep(100)
            }
        }
        throw IllegalStateException("Timed out waiting for $description.", last)
    }

    fun requestUntilProvidersSeen(
        role: HttpJson,
        markerPrefix: String,
        expectedProviders: Set<String>,
    ): Set<String> {
        val deadline = System.nanoTime() + Duration.ofSeconds(30).toNanos()
        val providers = mutableSetOf<String>()
        var index = 0
        while (System.nanoTime() < deadline && !providers.containsAll(expectedProviders)) {
            providers += requestProfileEventually(role, "$markerPrefix-$index").providerRid
            index++
            Thread.sleep(100)
        }
        return providers
    }
}
