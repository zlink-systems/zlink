namespace Zlink.Framework.Contracts.Configuration;

public interface IZLinkEndpointConnections
{
    void Connect(string endpoint);

    void Disconnect(string endpoint);

    IReadOnlyList<string> ListConnections();
}
