using System.ComponentModel;

namespace System.Runtime.CompilerServices
{
    /// <summary>
    ///     Marker the C# compiler needs for <c>init</c> accessors and positional records.
    ///     Unity's .NET Standard 2.1 profile does not define it, so the package supplies its
    ///     own internal copy.
    /// </summary>
    [EditorBrowsable(EditorBrowsableState.Never)]
    internal static class IsExternalInit
    {
    }
}
