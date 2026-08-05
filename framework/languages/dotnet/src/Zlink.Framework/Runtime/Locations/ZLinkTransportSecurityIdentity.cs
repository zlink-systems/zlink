namespace Zlink.Framework.Runtime.Locations;

internal static class ZLinkTransportSecurityIdentity
{
    // No configurable transport authentication is exposed yet. Keep one
    // explicit identity value across topology descriptors instead of using
    // an empty value that cannot participate in admission comparison.
    internal const string Plaintext = "plaintext";

    internal static string ToAdmissionIdentity(string descriptorIdentity) =>
        string.Equals(descriptorIdentity, Plaintext, StringComparison.Ordinal)
            ? "none"
            : descriptorIdentity;
}
