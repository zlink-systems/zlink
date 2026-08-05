using Zlink.Framework.E2E.Configuration;

namespace SubmitAdmission.Client;

internal sealed record ClientOptions(
    string CallerUrl,
    string TargetUrl,
    string PublisherUrl,
    string CallerRid,
    string TargetRid,
    string ObjectClientRid,
    string Scenario)
{
    public static ClientOptions Parse(string[] args) =>
        E2eConfiguration.Load<ClientOptions>(args);
}
