package systems.zlink.framework.kotlin

import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Test
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions

class KotlinMessageFlowTracingTest {
    @Test
    fun dslConfiguresDiagnostics() {
        val options = DefaultZLinkFrameworkOptions()
        options.configureDispatch {
            messageFlow(ZLinkMessageFlowLogMode.NORMAL)
            traceSampleRate(0.5)
            includeMessageSizes(false)
        }

        val diagnostics = options.registration().dispatchOptions().diagnostics()
        assertEquals(ZLinkMessageFlowLogMode.NORMAL, diagnostics.messageFlow())
        assertEquals(0.5, diagnostics.sampleRate())
        assertEquals(false, diagnostics.includeMessageSizes())
    }
}
