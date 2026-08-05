namespace ResilienceLifecycle.Client.Support;

internal sealed record LifecycleApiRes(
    string Operation,
    string[] ProviderAEvidence,
    string[] ProviderBEvidence);