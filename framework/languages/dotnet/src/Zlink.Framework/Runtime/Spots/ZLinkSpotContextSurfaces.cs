namespace Zlink.Framework.Runtime.Spots;

internal interface IZLinkSpotHandlerRegistrySink
{
    void AddPacket<THandler>() where THandler : class;

    void AddSubscribe<THandler>(string channelName, string topic) where THandler : class;

    void AddHandler<THandler>() where THandler : class;

    void AddHandler<THandler>(string packetName) where THandler : class;

    void AddActorPacket<THandler, TActor>()
        where THandler : class
        where TActor : IZLinkActor;

    void AddActorPacket<THandler, TActor>(string packetName)
        where THandler : class
        where TActor : IZLinkActor;
}

internal sealed class ZLinkSpotHandlerRegistrySurface(IZLinkSpotHandlerRegistrySink activation)
    : IZLinkSpotHandlerRegistry
{
    public void AddPacket<THandler>() where THandler : class
    {
        activation.AddPacket<THandler>();
    }

    public void AddSubscribe<THandler>(string channelName, string topic) where THandler : class
    {
        activation.AddSubscribe<THandler>(channelName, topic);
    }

    public void AddHandler<THandler>() where THandler : class
    {
        activation.AddHandler<THandler>();
    }

    public void AddHandler<THandler>(string packetName) where THandler : class
    {
        activation.AddHandler<THandler>(packetName);
    }

    public void AddActorPacket<THandler, TActor>()
        where THandler : class
        where TActor : IZLinkActor
    {
        activation.AddActorPacket<THandler, TActor>();
    }

    public void AddActorPacket<THandler, TActor>(string packetName)
        where THandler : class
        where TActor : IZLinkActor
    {
        activation.AddActorPacket<THandler, TActor>(packetName);
    }

}

internal sealed class ZLinkInstanceSpotHandlerRegistrySurface(
    IZLinkSpotHandlerRegistrySink activation) : IZLinkInstanceSpotHandlerRegistry
{
    public void AddPacket<THandler>() where THandler : class
    {
        activation.AddPacket<THandler>();
    }
}
