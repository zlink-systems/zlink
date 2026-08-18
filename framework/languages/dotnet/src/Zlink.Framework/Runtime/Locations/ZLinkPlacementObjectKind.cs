using System.Text.Json;
using System.Text.Json.Serialization;

namespace Zlink.Framework.Contracts.Locations;

/// <summary>
/// camelCase string enum converter shared by the Location Store's
/// canonical field types (21-location-runtime.md#2.4 pins lowercase
/// values like "actor"/"userSpot"/"active"/"reserved"). Scoped as a
/// type-level converter on individual enums rather than added to
/// ZLinkJsonSerializerOptions.Default, since that options object is
/// also used by unrelated wire codecs whose enum encoding isn't part of
/// this contract.
/// </summary>
internal sealed class ZLinkCamelCaseEnumJsonConverter<TEnum>
    : JsonStringEnumConverter<TEnum>
    where TEnum : struct, Enum
{
    public ZLinkCamelCaseEnumJsonConverter()
        : base(JsonNamingPolicy.CamelCase)
    {
    }
}

[JsonConverter(typeof(ZLinkCamelCaseEnumJsonConverter<ZLinkPlacementObjectKind>))]
internal enum ZLinkPlacementObjectKind
{
    Actor = 1,
    UserSpot = 2,
    InstanceSpot = 3
}
