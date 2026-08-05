using System.Diagnostics;

namespace SubmitAdmission.Server.Infrastructure;

internal sealed class AdmissionActivityObserver : IDisposable
{
    private readonly ActivityListener _listener;

    public AdmissionActivityObserver(OperationEvidenceStore evidence)
    {
        _listener = new ActivityListener
        {
            ShouldListenTo = source => source.Name == "Zlink.Framework",
            Sample = static (ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllData,
            ActivityStopped = activity =>
            {
                if (!string.Equals(
                        activity.OperationName,
                        "zlink.submit.admission",
                        StringComparison.Ordinal))
                    return;

                var traceId = activity.GetTagItem("zlink.submit.operation_id") as string;
                var eventName = activity.GetTagItem("zlink.submit.event") as string;
                if (string.IsNullOrEmpty(traceId) || string.IsNullOrEmpty(eventName)) return;
                evidence.Admission(
                    traceId,
                    eventName,
                    activity.GetTagItem("zlink.submit.pending_waiters") as int? ?? 0,
                    activity.GetTagItem("zlink.submit.reservations") as int? ?? 0,
                    activity.GetTagItem("zlink.submit.callbacks") as int? ?? 0);
            }
        };
        ActivitySource.AddActivityListener(_listener);
    }

    public void Dispose() => _listener.Dispose();
}
