using System.Diagnostics;
using Systems.Zlink;

internal sealed class SealedJoinObservation
{
    private readonly TestHostEventSink _sink;
    private readonly string _actorId;
    private readonly string _senderRid;
    private readonly string _senderRidHex;

    public SealedJoinObservation(TestHostEventSink sink, string actorId, string senderRid)
    {
        _sink = sink;
        _actorId = actorId;
        _senderRid = senderRid;
        _senderRidHex = RoutingId.From(senderRid).ToHex();
    }

    public void FailCompletion(string actorId, string nodeRid)
    {
        if (actorId != _actorId)
            return;
        _sink.Append($"late-actor-completion-throw|actor={actorId}|nodeRid={nodeRid}");
        var failure = new InvalidOperationException("cross-language Join completion failure");
        // The .NET Framework records no target_resume sample for a remote User
        // Spot Join; the completion callback runs only after the target took
        // the authority, so it marks the owner as resumed.
        _sink.Append($"late-actor-owner-resumed|nodeRid={nodeRid}");
        throw failure;
    }

    public void ObserveIngress(Activity activity)
    {
        if (
            (
                activity.GetTagItem("phase") as string != "received"
                && activity.OperationName != "zlink.dispatch_error"
            )
            || activity.GetTagItem("actor_id") as string != _actorId
        )
            return;
        var source = activity.GetTagItem("source_rid") as string;
        if (source != _senderRid && source != _senderRidHex)
            return;
        var kind = activity.GetTagItem("message_kind") as string switch
        {
            "send" => "actorSend",
            "request" => "actorRequest",
            _ => null,
        };
        if (kind is not null)
            _sink.Append(
                $"late-actor-owner-ingress|kind={kind}|source={_senderRid}|actor={_actorId}"
            );
    }
}
