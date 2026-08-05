namespace Zlink.Framework.Contracts.Errors;

public sealed class ZLinkConfigurationException : InvalidOperationException
{
    internal ZLinkConfigurationException(string message)
        : base(message)
    {
    }
}
