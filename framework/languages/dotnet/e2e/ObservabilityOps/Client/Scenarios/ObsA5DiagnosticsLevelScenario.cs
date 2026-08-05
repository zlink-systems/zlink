// Verifies OBS-A5 applies runtime diagnostics level changes.
using System.Diagnostics;
using ObservabilityOps.Client.Support;
using ObservabilityOps.Shared;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsA5DiagnosticsLevelScenario
{
    public static async Task RunAsync(ScenarioContext context)
    {
        _ = await context.WorkflowNodeIdAsync("workflow-a");
        _ = await context.WorkflowNodeIdAsync("workflow-b");
        var workflowId = $"workflow-a5-{Guid.NewGuid():N}";
        var created = (await context.WorkflowA.Post("/workflows")
            .Body(new CreateWorkflowReq(workflowId))
            .Async<CreateWorkflowRes>()).Body;
        var owner = context.Workflow(created.NodeRid);

        await SetLevelAsync(owner, "key_transitions");
        await AssertProbeAsync(owner, workflowId, "before");
        await SetLevelAsync(owner, "off");
        await AssertProbeAsync(owner, workflowId, "off");
        await SetLevelAsync(owner, "errors_only");
        await AssertProbeAsync(owner, workflowId, "errors");
        await SetLevelAsync(owner, "key_transitions");
        await AssertProbeAsync(owner, workflowId, "after");

        var lines = context.ReadFlowLines(await context.WorkflowRoleForNodeAsync(created.NodeRid));
        ZlinkStreamAssert.Ensure(
            HasPacket(lines, nameof(DiagnosticsBeforeReq))
            && HasPacket(lines, nameof(DiagnosticsAfterReq)),
            "OBS-A5 enabled diagnostics did not emit request flow records.");
        ZlinkStreamAssert.Ensure(
            !HasPacket(lines, nameof(DiagnosticsOffReq))
            && !HasPacket(lines, nameof(DiagnosticsErrorsReq)),
            "OBS-A5 emitted successful request flow records while diagnostics "
            + "was Off or Errors-only.");
        Console.WriteLine("scenario OBS-A5 passed");
    }

    private static async Task SetLevelAsync(
        Zlink.HttpClient.ZLinkHttpClient owner,
        string level)
    {
        var started = Stopwatch.GetTimestamp();
        await owner.Post("/diagnostics/level")
            .Query("level", level).AsyncRaw();
        ZlinkStreamAssert.Ensure(
            Stopwatch.GetElapsedTime(started) < TimeSpan.FromSeconds(1),
            $"OBS-A5 diagnostics level '{level}' waited for message processing.");
    }

    private static async Task AssertProbeAsync(
        Zlink.HttpClient.ZLinkHttpClient owner,
        string workflowId,
        string phase)
    {
        var response = (await owner
            .Post($"/workflows/{workflowId}/diagnostics-probe/{phase}")
            .Async<DiagnosticsProbeRes>()).Body;
        ZlinkStreamAssert.Ensure(
            response.Marker == $"diagnostics-{phase}",
            $"OBS-A5 business response changed in phase '{phase}'.");
    }

    private static bool HasPacket(IEnumerable<string> lines, string packet) =>
        lines.Any(line =>
            line.Contains($"packet={packet}", StringComparison.Ordinal));
}
