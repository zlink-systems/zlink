namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotPacketRegistry
{
    private readonly Dictionary<string, ZLinkSpotDescriptor> _descriptorsByName = new(StringComparer.Ordinal);
    private readonly List<ZLinkSpotPacketRegistration> _registrations = [];

    public bool HasPackets => _descriptorsByName.Count > 0;

    public void Add(Type handlerType)
    {
        Add(handlerType, null);
    }

    public void Add(Type handlerType, string? packetName)
    {
        _registrations.Add(new ZLinkSpotPacketRegistration(handlerType, packetName));
    }

    public void Add(Type spotType, System.Reflection.MethodInfo method, string? packetName)
    {
        _registrations.Add(new ZLinkSpotPacketRegistration(spotType, packetName, method));
    }

    public void Add(ZLinkScannedSpotHandler handler)
    {
        if (handler.Method is { } method)
            Add(handler.HandlerType, method, handler.PacketName);
        else
            Add(handler.HandlerType, handler.PacketName);
    }

    public void Bind(object spot)
    {
        foreach (var packet in _registrations)
        {
            var descriptor = packet.Method is { } method
                ? ZLinkSpotDescriptorFactory.CreateAttributedRequestDescriptor(
                    packet.HandlerType, method, packet.PacketName)
                : ZLinkSpotDescriptorFactory.CreatePacketDescriptor(
                    packet.HandlerType, spot.GetType(), packet.PacketName);
            if (!_descriptorsByName.TryAdd(descriptor.MessageName, descriptor))
                throw new ZLinkConfigurationException(
                    $"SPOT packet handler '{descriptor.MessageName}' is already registered.");
        }
    }

    public bool TryResolve(ZLinkEnvelopeHeader header, out ZLinkSpotDescriptor? descriptor)
    {
        return _descriptorsByName.TryGetValue(header.MessageName, out descriptor);
    }
}
