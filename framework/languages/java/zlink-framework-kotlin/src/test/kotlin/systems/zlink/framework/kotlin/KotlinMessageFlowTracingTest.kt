package systems.zlink.framework.kotlin

import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertNotNull
import org.junit.jupiter.api.Test
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions

// Kotlin ergonomics for the (Java-runtime) message-flow tracing: the configureDispatch
// DSL block and the onMessageFlow lambda observer wire the shared Java diagnostics.
class KotlinMessageFlowTracingTest {
    @Test
    fun dslConfiguresDiagnostics() {
        val options = DefaultZLinkFrameworkOptions()
        options.configureDispatch {
            messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
            traceLogFile("logs/flow.log")
            traceLabel("api")
        }

        val diagnostics = options.registration().dispatchOptions().diagnostics()
        assertEquals(ZLinkMessageFlowLogMode.KEY_TRANSITIONS, diagnostics.messageFlow())
        assertEquals("logs/flow.log", diagnostics.logFile())
        assertEquals("api", diagnostics.label())
        assertEquals(ZLinkMessageFlowLogMode.KEY_TRANSITIONS, diagnostics.effectiveMessageFlow())
    }

    @Test
    fun onMessageFlowRegistersLambdaObserver() {
        val options = DefaultZLinkFrameworkOptions()
        options.configureDispatch {
            onMessageFlow { }
        }
        assertNotNull(options.registration().dispatchOptions().messageFlowObserver())
    }
}
