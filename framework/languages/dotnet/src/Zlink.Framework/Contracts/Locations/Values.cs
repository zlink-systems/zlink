namespace Zlink.Framework.Contracts.Locations;

/// <summary>
/// Numeric values are part of the serialized location contract and must remain
/// stable. Value 1 is reserved and is not a valid role.
/// </summary>
public enum ZLinkLocationRole : ushort
{
    Invalid = 0,
    Spot = 2,
    Router = 3,
    Dealer = 4,
    Pub = 5,
    Sub = 6
}
