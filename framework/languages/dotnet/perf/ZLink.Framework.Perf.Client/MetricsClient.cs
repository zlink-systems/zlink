using System.Text;

namespace ZLink.Framework.Perf;

// Only the standalone application client sends phase triggers. HTTP acknowledgements are not echo operations.
public sealed class MetricsClient(EndpointManifest manifest) : IDisposable
{
    private readonly HttpClient http = new() { Timeout = TimeSpan.FromMilliseconds(manifest.workload.adminTimeoutMs) };
    public async Task<object[]> TriggerRolesAsync(PerfTriggerRequest request)
    {
        List<object> acknowledgements = [];
        // Manifest order is receivers then source, fixed by the coordinator before processes start.
        foreach (var role in manifest.roles)
        {
            var sent = PerfClock.Now;
            using var response = await http.PostAsync(role.applicationTriggerUrl, Content(request));
            var body = await response.Content.ReadAsStringAsync();
            var received = PerfClock.Now;
            if (!response.IsSuccessStatusCode) throw new InvalidOperationException($"Trigger {role.role}/{role.roleInstance}: HTTP {(int)response.StatusCode}: {body}");
            var ack = PerfJson.Read<PerfTriggerReply>(body);
            if (!ack.accepted || ack.runId != request.runId || ack.cellId != request.cellId ||
                ack.resetSeq != request.resetSeq || ack.phase != request.phase || ack.configHash != manifest.configHash)
                throw new PerfValidationException("PhaseMismatch", "Role trigger acknowledgement identity differs.");
            acknowledgements.Add(new { role = role.role, role.roleInstance, sentTicks = DecimalText.Of(sent),
                ackTicks = DecimalText.Of(received), clockDomainId = PerfClock.Domain, acknowledgement = ack });
        }
        return acknowledgements.ToArray();
    }
    public async Task<ResetReply[]> ResetRolesAsync(ResetRequest request)
    {
        List<ResetReply> acknowledgements = [];
        foreach (var role in manifest.roles)
        {
            using var response = await http.PostAsync(role.metrics.baseUrl + "/perf/reset", Content(request));
            var body = await response.Content.ReadAsStringAsync();
            if (!response.IsSuccessStatusCode) throw new InvalidOperationException($"Reset {role.role}/{role.roleInstance}: HTTP {(int)response.StatusCode}: {body}");
            var ack = PerfJson.Read<ResetReply>(body);
            if (!ack.ok || ack.runId != request.runId || ack.cellId != request.cellId || ack.resetSeq != request.resetSeq)
                throw new PerfValidationException("PhaseMismatch", "Role reset acknowledgement identity differs.");
            acknowledgements.Add(ack);
        }
        return acknowledgements.ToArray();
    }
    private static StringContent Content<T>(T value) => new(PerfJson.Write(value), Encoding.UTF8, "application/json");
    public void Dispose() => http.Dispose();
}
