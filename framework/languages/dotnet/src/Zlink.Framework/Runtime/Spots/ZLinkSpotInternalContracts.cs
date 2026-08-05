namespace Zlink.Framework.Contracts.Spots;

internal readonly record struct ZLinkSpotAcceptRejectResult(bool Accepted, ZLinkMessage? Reply)
{
    public static ZLinkSpotAcceptRejectResult Accept(ZLinkMessage? reply = null)
    {
        return new ZLinkSpotAcceptRejectResult(true, reply);
    }

    public static ZLinkSpotAcceptRejectResult Accept<TReply>(TReply reply)
    {
        return Accept(ZLinkMessage.From(reply));
    }

    public static ZLinkSpotAcceptRejectResult Reject(ZLinkMessage? reply = null)
    {
        return new ZLinkSpotAcceptRejectResult(false, reply);
    }

    public static ZLinkSpotAcceptRejectResult Reject<TReply>(TReply reply)
    {
        return Reject(ZLinkMessage.From(reply));
    }
}

internal readonly record struct ZLinkSpotInfo(string SpotId);
