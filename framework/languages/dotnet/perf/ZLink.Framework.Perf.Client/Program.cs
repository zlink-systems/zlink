using System.Text.Json;
using ZLink.Framework.Perf;

if (args.Length != 4 || args[0] != "--endpoint-config" || args[2] != "--client-index" || !int.TryParse(args[3], out var index))
    throw new ArgumentException("Client requires --endpoint-config <file> --client-index <index>.");
var manifest = PerfJson.Read<EndpointManifest>(File.ReadAllText(args[1]));
if (index < 0 || index >= manifest.workload.clientCount) throw new ArgumentOutOfRangeException(nameof(index));
var cs = manifest.roles.Any(r => r.streamEndpoint is not null);
var config = new RoleConfig(manifest.runId, manifest.cellId, manifest.configHash, "client", index,
    cs ? "session-echo-only" : "channel-echo-only", null, null, null, null, null, "", "", false,
    "None", null, [], [], "Immediate", manifest.workload, manifest.provenance);
using var measurement = new Measurement(config, cs);
await using var scenario = cs ? new SessionEchoOnlyScenario(manifest, measurement, index) : null;
using var admin = new MetricsClient(manifest);
if (scenario is not null) await scenario.PrepareAsync();
Console.WriteLine(PerfJson.Write(new { type = "prepared", ok = !measurement.HasErrors,
    snapshot = measurement.Snapshot(null) }));
while (await Console.In.ReadLineAsync() is { } line)
{
    try
    {
        using var document = JsonDocument.Parse(line);
        var root = document.RootElement;
        var command = root.GetProperty("command").GetString();
        object response;
        switch (command)
        {
            case "start":
                response = measurement.Start(PerfJson.Read<PerfTriggerRequest>(root.GetProperty("request").GetRawText()),
                    scenario is null ? null : scenario.RunAsync);
                break;
            case "triggerRoles":
                response = await admin.TriggerRolesAsync(PerfJson.Read<PerfTriggerRequest>(root.GetProperty("request").GetRawText()));
                break;
            case "resetRoles":
                response = await admin.ResetRolesAsync(PerfJson.Read<ResetRequest>(root.GetProperty("request").GetRawText()));
                break;
            case "reset":
                response = measurement.Reset(PerfJson.Read<ResetRequest>(root.GetProperty("request").GetRawText()), null);
                break;
            case "wait":
                await measurement.PhaseTask;
                response = new { ok = !measurement.HasErrors, phase = measurement.Phase };
                break;
            case "stats": response = measurement.Snapshot(null); break;
            case "stop": return;
            default: throw new JsonException("Unknown control command.");
        }
        Console.WriteLine(PerfJson.Write(new { ok = true, response }));
    }
    catch (Exception error)
    {
        measurement.RecordDiagnostic(error);
        Console.WriteLine(PerfJson.Write(new { ok = false, errorType = error.GetType().FullName, error.Message }));
    }
}
