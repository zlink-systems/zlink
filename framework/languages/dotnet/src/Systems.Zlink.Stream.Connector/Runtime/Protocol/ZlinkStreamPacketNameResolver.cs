using System.Collections.Concurrent;
using System.Reflection;

namespace Systems.Zlink.Stream.Connector.Runtime.Protocol;

internal sealed class ZlinkStreamPacketNameResolver : IZlinkStreamPacketNameResolver
{
    private static readonly ConcurrentDictionary<Type, string> Cache = new();

    /// <summary>
    ///     Resolves the packet name of <paramref name="payloadType" />: the name on its
    ///     <see cref="ZlinkStreamPacketNameAttribute" /> when there is one, otherwise the
    ///     simple type name (stream-connector spec §5).
    /// </summary>
    /// <remarks>
    ///     The lookup honours inheritance because
    ///     <see cref="ZlinkStreamPacketNameAttribute" /> declares itself inherited. Reading
    ///     it with <c>inherit: false</c> contradicted that declaration: a derived payload
    ///     type fell back to its own type name while the attribute said it carried the base
    ///     type's packet name.
    /// </remarks>
    public string Resolve(Type payloadType)
    {
        if (payloadType is null)
            throw new ArgumentNullException(nameof(payloadType));
        return Cache.GetOrAdd(
            payloadType,
            static type =>
                type.GetCustomAttribute<ZlinkStreamPacketNameAttribute>(true)?.Name ?? type.Name
        );
    }
}
