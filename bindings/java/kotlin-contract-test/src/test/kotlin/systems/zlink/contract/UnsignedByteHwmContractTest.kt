/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract

import systems.zlink.contracts.core.ContextOptions
import systems.zlink.contracts.sockets.CommonSocketOptions
import kotlin.test.Test
import kotlin.test.assertEquals

class UnsignedByteHwmContractTest {
    @Test
    fun javaSurfaceCarriesTheCompleteUnsigned64BitRange() {
        val setSend: (CommonSocketOptions, ULong) -> Unit =
            { options, value -> options.sendHwm(value.toLong()) }
        val getSend: (CommonSocketOptions) -> ULong =
            { options -> options.sendHwm().toULong() }
        val setPlanningUnit: (ContextOptions, ULong) -> Unit =
            { options, value ->
                options.autoHwmMessageUnitBytes(value.toLong())
            }
        val getPlanningUnit: (ContextOptions) -> ULong =
            { options -> options.autoHwmMessageUnitBytes().toULong() }

        assertEquals(ULong.MAX_VALUE, (-1L).toULong())
        @Suppress("UNUSED_VARIABLE")
        val compileTimeContract = listOf(
            setSend, getSend, setPlanningUnit, getPlanningUnit)
    }
}
