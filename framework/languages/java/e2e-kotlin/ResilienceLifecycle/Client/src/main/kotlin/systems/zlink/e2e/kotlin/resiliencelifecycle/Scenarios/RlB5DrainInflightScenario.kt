package systems.zlink.e2e.kotlin.resiliencelifecycle

import java.time.Duration
import java.util.concurrent.TimeUnit

fun ClientScenarioContext.runDrainInflightScenario() {
    post("${adminB()}/admin/drain")
    waitForWeight(adminB(), 0)
    sleep(1500)

    val slow = submitWork("slow", Duration.ofSeconds(15))
    waitForEvidence(adminA(), "SlowStarted")

    post("${adminA()}/admin/drain")
    waitForWeight(adminA(), 0)
    post("${adminB()}/admin/restore")
    waitForWeight(adminB(), 100)
    collectStableProvidersWithout("b5-after-drain", "api-a", "api-b")

    post("${adminA()}/admin/release-slow")
    val slowReply = try {
        slow.toCompletableFuture().get(20, TimeUnit.SECONDS)
    } catch (error: Exception) {
        throw IllegalStateException("RL-B5 slow request did not complete", error)
    }
    ensure(slowReply.providerRid() == "api-a", "RL-B5 slow request was not served by api-a: ${slowReply.providerRid()}")
    ensure(slowReply.value() == "work:slow", "RL-B5 slow reply payload mismatch")

    post("${adminA()}/admin/restore")
    waitForWeight(adminA(), 100)
    println("scenario RL-B5 passed")
}
