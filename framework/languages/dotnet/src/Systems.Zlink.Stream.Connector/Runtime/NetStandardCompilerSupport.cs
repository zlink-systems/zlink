#if NETSTANDARD2_1
namespace System.Runtime.CompilerServices;

internal static class IsExternalInit { }

[AttributeUsage(AttributeTargets.All, AllowMultiple = true)]
internal sealed class CompilerFeatureRequiredAttribute(string featureName) : Attribute
{
    public string FeatureName { get; } = featureName;
}

[AttributeUsage(AttributeTargets.All)]
internal sealed class RequiredMemberAttribute : Attribute { }
#endif
