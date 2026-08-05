namespace Zlink.Framework.Runtime.Configuration;

internal static class ZLinkRequestTimeoutValidation
{
    public static void Validate(TimeSpan timeout, string name)
    {
        if (timeout <= TimeSpan.Zero) throw new ZLinkConfigurationException($"{name} must be greater than zero.");
    }
}