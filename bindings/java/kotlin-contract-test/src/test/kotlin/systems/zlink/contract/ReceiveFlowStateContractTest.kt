/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract

import systems.zlink.contracts.sockets.CommonSocketOptions
import systems.zlink.contracts.sockets.ReceiveFlowState
import kotlin.test.Test
import kotlin.test.assertEquals

class ReceiveFlowStateContractTest {
    @Test
    fun enumValueParityWithCAbi() {
        assertEquals(0, ReceiveFlowState.RUNNING.value())
        assertEquals(1, ReceiveFlowState.PAUSED.value())
        assertEquals(ReceiveFlowState.RUNNING, ReceiveFlowState.fromValue(0))
        assertEquals(ReceiveFlowState.PAUSED, ReceiveFlowState.fromValue(1))
    }

    @Test
    fun javaSurfaceExposesTheReceiveFlowStateSetter() {
        val setReceiveFlowState: (CommonSocketOptions, ReceiveFlowState) -> Unit =
            { options, value -> options.receiveFlowState(value) }
        @Suppress("UNUSED_VARIABLE")
        val compileTimeContract = listOf(setReceiveFlowState)
    }
}
