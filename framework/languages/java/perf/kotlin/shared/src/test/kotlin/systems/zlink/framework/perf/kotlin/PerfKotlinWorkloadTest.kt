package systems.zlink.framework.perf.kotlin

import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Test

class PerfKotlinWorkloadTest {
    @Test
    fun streamTargetsAreAssignedForEachStreamAtSetup() {
        assertEquals(
            listOf("spot-a", "spot-b", "spot-a", "spot-b", "spot-a"),
            planStreamTargets(listOf("spot-a", "spot-b"), 5),
        )
    }

}
