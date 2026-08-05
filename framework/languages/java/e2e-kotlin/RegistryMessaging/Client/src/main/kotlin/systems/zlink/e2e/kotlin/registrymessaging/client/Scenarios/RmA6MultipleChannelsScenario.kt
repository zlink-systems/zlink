package systems.zlink.e2e.kotlin.registrymessaging.client.Scenarios

import java.util.UUID
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.HttpJson
import systems.zlink.e2e.kotlin.registrymessaging.client.Support.ScenarioAssert
import systems.zlink.e2e.kotlin.registrymessaging.shared.WorkflowReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.WorkflowRes

object RmA6MultipleChannelsScenario {
    fun run(discoveryConsumer: HttpJson, providerA: HttpJson, providerB: HttpJson, workflow: HttpJson) {
        val profileMarker = "a6-api-${UUID.randomUUID()}"
        val workflowMarker = "a6-workflow-${UUID.randomUUID()}"
        val api = ScenarioAssert.requestProfileEventually(discoveryConsumer, profileMarker)
        ScenarioAssert.that(api.value == "profile:$profileMarker", "RM-A6 api reply mismatch.")
        ScenarioAssert.that(api.providerRid == "api-a" || api.providerRid == "api-b", "RM-A6 api resolved wrong provider.")

        val workflowReply = requestWorkflowEventually(discoveryConsumer, workflowMarker)
        ScenarioAssert.that(workflowReply.providerRid == "workflow-a", "RM-A6 workflow resolved wrong provider.")
        ScenarioAssert.that(workflowReply.value == "workflow:$workflowMarker", "RM-A6 workflow reply mismatch.")

        val providerEvidence = waitAnyEvidence(providerA, providerB, profileMarker)
        val workflowEvidence = waitEvidence(workflow, workflowMarker)
        ScenarioAssert.that(
            providerEvidence.any { it.contains("ProfileReq") && it.contains(profileMarker) },
            "RM-A6 profile evidence missing.",
        )
        ScenarioAssert.that(
            workflowEvidence.any { it.contains("workflow-request") && it.contains(workflowMarker) },
            "RM-A6 workflow evidence missing.",
        )
        val allProviderEvidence = providerA.get<List<String>>("/evidence") + providerB.get<List<String>>("/evidence")
        ScenarioAssert.that(
            allProviderEvidence.none { it.contains("workflow-request") && it.contains(workflowMarker) },
            "RM-A6 workflow request was recorded on profile providers.",
        )
        println("scenario RM-A6 passed")
    }

    private fun requestWorkflowEventually(consumer: HttpJson, marker: String): WorkflowRes =
        ScenarioAssert.retryUntil("workflow request $marker") {
            consumer.post<WorkflowRes>("/workflow/request", WorkflowReq(marker))
        }

    private fun waitAnyEvidence(providerA: HttpJson, providerB: HttpJson, marker: String): List<String> =
        ScenarioAssert.retryUntil("provider evidence $marker") {
            val evidence = providerA.get<List<String>>("/evidence") + providerB.get<List<String>>("/evidence")
            if (evidence.any { it.contains(marker) }) {
                evidence
            } else {
                throw IllegalStateException("provider evidence missing")
            }
        }

    private fun waitEvidence(role: HttpJson, marker: String): List<String> =
        ScenarioAssert.retryUntil("evidence $marker") {
            val evidence = role.get<List<String>>("/evidence")
            if (evidence.any { it.contains(marker) }) {
                evidence
            } else {
                throw IllegalStateException("evidence missing")
            }
        }
}
