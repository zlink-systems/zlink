package systems.zlink.e2e.kotlin.resiliencelifecycle

import java.time.Duration

fun ClientScenarioContext.runLocationStoreOutageScenario() {
    waitForTopology(2)
    val beforeTopology = readTopology()
    ensure(
        beforeTopology.status == "ok" && beforeTopology.matchedRouters >= 2,
        "RL-C4 topology was not ready before outage: $beforeTopology",
    )

    val before = requestWork("rl-c4-before-outage", Duration.ofSeconds(5))
    ensure(before.value() == "work:rl-c4-before-outage", "RL-C4 before-outage payload mismatch")
    waitForEvidenceValueAny("WorkReq", "rl-c4-before-outage", adminA(), adminB())

    signal("c4-pause-store")
    waitForSignal("c4-store-paused")

    val during = requestWork("rl-c4-during-outage", Duration.ofSeconds(5))
    ensure(during.value() == "work:rl-c4-during-outage", "RL-C4 during-outage payload mismatch")
    waitForEvidenceValueAny("WorkReq", "rl-c4-during-outage", adminA(), adminB())

    val outageTopology = readTopology()
    ensure(
        outageTopology.status == "error",
        "RL-C4 topology read unexpectedly succeeded during store outage: $outageTopology",
    )

    signal("c4-unpause-store")
    waitForSignal("c4-store-unpaused")
    waitForTopology(2)

    val recoveredTopology = readTopology()
    ensure(
        recoveredTopology.status == "ok" && recoveredTopology.matchedRouters >= 2,
        "RL-C4 topology did not recover after outage: $recoveredTopology",
    )

    val after = requestWork("rl-c4-after-outage", Duration.ofSeconds(5))
    ensure(after.value() == "work:rl-c4-after-outage", "RL-C4 after-outage payload mismatch")
    waitForEvidenceValueAny("WorkReq", "rl-c4-after-outage", adminA(), adminB())
    println("scenario RL-C4 passed")
}
