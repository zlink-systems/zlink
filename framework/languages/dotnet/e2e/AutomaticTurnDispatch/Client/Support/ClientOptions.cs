using Zlink.Framework.E2E.Configuration;
namespace AutomaticTurnDispatch.Client.Support;

internal sealed record ClientOptions(
    string SessionAStreamEndpoint,
    string SessionBStreamEndpoint,
    string Scenario,
    string RequestId,
    string SpotRid,
    string PlayAUrl = "",
    string PlayBUrl = "")
{
    public static ClientOptions Parse(string[] args)
        => E2eConfiguration.Load<ClientOptions>(args);
}
