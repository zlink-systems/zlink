namespace Zlink.Framework.ContractTests.Support;

[AttributeUsage(AttributeTargets.Method, AllowMultiple = true)]
internal sealed class ContractExampleAttribute(params Type[] contracts) : Attribute
{
    public IReadOnlyCollection<Type> Contracts { get; } = contracts;
}
