using System.Globalization;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text;
using System.Text.RegularExpressions;

internal static class PublicContractSnapshot
{
    public static string Render(IEnumerable<Assembly> assemblies)
    {
        var lines = new List<string>();
        var nullability = new NullabilityInfoContext();
        foreach (var assembly in assemblies
                     .Distinct()
                     .OrderBy(static item => item.GetName().Name, StringComparer.Ordinal))
        {
            lines.Add($"assembly {assembly.GetName().Name}");
            foreach (var forwarded in assembly.GetForwardedTypes()
                         .OrderBy(static item => item.FullName, StringComparer.Ordinal))
                lines.Add($"  forward {FormatType(forwarded)}");
            foreach (var type in assembly.GetExportedTypes()
                         .OrderBy(static item => item.FullName, StringComparer.Ordinal))
                RenderType(type, nullability, lines);
        }

        return string.Join('\n', lines) + "\n";
    }

    internal static string RenderTypes(IEnumerable<Type> types)
    {
        var lines = new List<string>();
        var nullability = new NullabilityInfoContext();
        foreach (var type in types.OrderBy(static item => item.FullName, StringComparer.Ordinal))
            RenderType(type, nullability, lines);
        return string.Join('\n', lines) + "\n";
    }

    private static void RenderType(
        Type type,
        NullabilityInfoContext nullability,
        ICollection<string> lines)
    {
        var kind = type.IsInterface
            ? "interface"
            : type.IsEnum
                ? "enum"
                : type.IsValueType
                    ? "struct"
                    : type.IsAbstract && type.IsSealed
                        ? "static-class"
                        : type.IsAbstract
                            ? "abstract-class"
                            : type.IsSealed
                                ? "sealed-class"
                                : "class";
        if (type.IsValueType && type.IsByRefLike) kind = $"ref-{kind}";
        if (type.IsValueType && HasAttribute(type, typeof(IsReadOnlyAttribute).FullName!))
            kind = $"readonly-{kind}";
        var required = HasAttribute(type, "System.Runtime.CompilerServices.RequiredMemberAttribute")
            ? " required-members"
            : string.Empty;
        var bases = new List<string>();
        if (type.BaseType is { } baseType && baseType != typeof(object) && baseType != typeof(ValueType))
            bases.Add(FormatType(baseType));
        bases.AddRange(type.GetInterfaces()
            .Where(static item => item.IsPublic || item.IsNestedPublic)
            .Select(static item => FormatType(item))
            .Order(StringComparer.Ordinal));
        lines.Add($"  type {kind} {FormatType(type)}{FormatBases(bases)}{required}");
        if (type.GetCustomAttribute<AttributeUsageAttribute>() is { } usage)
            lines.Add(
                $"    attribute-usage targets={usage.ValidOn} allow-multiple={FormatValue(usage.AllowMultiple)} inherited={FormatValue(usage.Inherited)}");
        RenderGenericConstraints(type.GetGenericArguments(), "    ", lines);

        foreach (var field in type.GetFields(BindingFlags.Public | BindingFlags.Static | BindingFlags.Instance |
                                             BindingFlags.DeclaredOnly)
                     .Where(static field => !field.IsSpecialName)
                     .OrderBy(FieldKey, StringComparer.Ordinal))
        {
            var modifiers = field.IsLiteral
                ? "const "
                : field.IsStatic
                    ? field.IsInitOnly ? "static readonly " : "static "
                    : field.IsInitOnly ? "readonly " : string.Empty;
            var value = field.IsLiteral ? $" = {FormatValue(field.GetRawConstantValue())}" : string.Empty;
            var fieldNullability = TryCreate(nullability, field);
            var requiredField = HasAttribute(field, "System.Runtime.CompilerServices.RequiredMemberAttribute")
                ? "required "
                : string.Empty;
            lines.Add(
                $"    field {requiredField}{modifiers}{FormatType(field.FieldType, fieldNullability)} {field.Name}{value}{FormatNullability(fieldNullability)}");
        }

        foreach (var constructor in type.GetConstructors(BindingFlags.Public | BindingFlags.Instance |
                                                          BindingFlags.DeclaredOnly)
                     .OrderBy(MethodKey, StringComparer.Ordinal))
            lines.Add($"    ctor {type.Name.Split('`')[0]}({FormatParameters(constructor, nullability)})");

        foreach (var property in type.GetProperties(BindingFlags.Public | BindingFlags.Static |
                                                    BindingFlags.Instance | BindingFlags.DeclaredOnly)
                     .OrderBy(PropertyKey, StringComparer.Ordinal))
        {
            var accessor = property.GetMethod ?? property.SetMethod;
            var staticText = accessor?.IsStatic == true ? "static " : string.Empty;
            var propertyNullability = TryCreate(nullability, property);
            var setter = property.SetMethod?.IsPublic == true
                ? IsInitOnly(property.SetMethod) ? "init; " : "set; "
                : string.Empty;
            var accessors = $"{{ {(property.GetMethod?.IsPublic == true ? "get; " : string.Empty)}{setter}}}";
            var index = property.GetIndexParameters().Length == 0
                ? property.Name
                : $"this[{FormatParameters(property.GetIndexParameters(), nullability)}]";
            var requiredProperty = HasAttribute(property, "System.Runtime.CompilerServices.RequiredMemberAttribute")
                ? "required "
                : string.Empty;
            lines.Add(
                $"    property {requiredProperty}{staticText}{FormatType(property.PropertyType, propertyNullability)} {index} {accessors}{FormatNullability(propertyNullability)}");
        }

        foreach (var @event in type.GetEvents(BindingFlags.Public | BindingFlags.Static | BindingFlags.Instance |
                                              BindingFlags.DeclaredOnly)
                     .OrderBy(static item => item.Name, StringComparer.Ordinal))
        {
            var staticText = @event.AddMethod?.IsStatic == true ? "static " : string.Empty;
            var eventNullability = TryCreate(nullability, @event);
            lines.Add(
                $"    event {staticText}{FormatType(@event.EventHandlerType!, eventNullability)} {@event.Name}{FormatNullability(eventNullability)}");
        }

        foreach (var method in type.GetMethods(BindingFlags.Public | BindingFlags.Static | BindingFlags.Instance |
                                               BindingFlags.DeclaredOnly)
                     .Where(static method => !IsAccessor(method))
                     .OrderBy(MethodKey, StringComparer.Ordinal))
        {
            var modifiers = method.IsStatic ? "static " : string.Empty;
            if (method.IsAbstract) modifiers += "abstract ";
            else if (method.GetBaseDefinition() != method)
                modifiers += method.IsFinal ? "sealed-override " : "override ";
            else if (method.IsVirtual) modifiers += "virtual ";
            var generic = method.IsGenericMethodDefinition
                ? $"<{string.Join(", ", method.GetGenericArguments().Select(static argument => argument.Name))}>"
                : string.Empty;
            var returnNullability = TryCreate(nullability, method.ReturnParameter);
            lines.Add(
                $"    method {modifiers}{FormatReturn(method, returnNullability)} {method.Name}{generic}({FormatParameters(method, nullability)}){FormatNullability(returnNullability, "return")}{FormatCustomModifiers(method.ReturnParameter)}");
            RenderGenericConstraints(method.GetGenericArguments(), "      ", lines);
        }
    }

    private static string FormatParameters(MethodBase method, NullabilityInfoContext nullability) =>
        FormatParameters(
            method.GetParameters(),
            nullability,
            method.IsDefined(typeof(ExtensionAttribute), inherit: false));

    private static string FormatParameters(
        IReadOnlyList<ParameterInfo> parameters,
        NullabilityInfoContext nullability,
        bool extensionMethod = false) =>
        string.Join(", ", parameters.Select((parameter, index) =>
        {
            var modifier = extensionMethod && index == 0
                ? "this "
                : parameter.GetCustomAttribute<ParamArrayAttribute>() is not null
                ? "params "
                : parameter.IsOut
                    ? "out "
                    : parameter.ParameterType.IsByRef
                        ? parameter.IsIn ? "in " : "ref "
                        : string.Empty;
            var defaultValue = parameter.HasDefaultValue
                ? $" = {FormatDefaultValue(parameter)}"
                : string.Empty;
            var parameterNullability = TryCreate(nullability, parameter);
            return $"{modifier}{FormatType(parameter.ParameterType, parameterNullability)} {parameter.Name}{defaultValue}{FormatNullability(parameterNullability)}{FormatCustomModifiers(parameter)}";
        }));

    private static string FormatDefaultValue(ParameterInfo parameter)
    {
        if (parameter.DefaultValue is null)
        {
            var parameterType = parameter.ParameterType.IsByRef
                ? parameter.ParameterType.GetElementType()!
                : parameter.ParameterType;
            if (parameterType.IsValueType && Nullable.GetUnderlyingType(parameterType) is null)
                return "default";
        }

        return FormatValue(parameter.DefaultValue);
    }

    private static void RenderGenericConstraints(
        IEnumerable<Type> arguments,
        string indent,
        ICollection<string> lines)
    {
        foreach (var argument in arguments.Where(static item => item.IsGenericParameter))
        {
            var constraints = new List<string>();
            var attributes = argument.GenericParameterAttributes;
            var variance = (attributes & GenericParameterAttributes.VarianceMask) switch
            {
                GenericParameterAttributes.Covariant => "out",
                GenericParameterAttributes.Contravariant => "in",
                _ => "invariant"
            };
            var nullableFlag = GenericNullableFlag(argument);
            lines.Add($"{indent}generic {argument.Name} variance={variance} nullable={FormatNullableFlag(nullableFlag)}");
            var unmanaged = HasAttribute(argument, "System.Runtime.CompilerServices.IsUnmanagedAttribute");
            if ((attributes & GenericParameterAttributes.ReferenceTypeConstraint) != 0)
                constraints.Add(nullableFlag == 2 ? "class?" : "class");
            if (unmanaged)
                constraints.Add("unmanaged");
            else if ((attributes & GenericParameterAttributes.NotNullableValueTypeConstraint) != 0)
                constraints.Add("struct");
            else if ((attributes & (GenericParameterAttributes.ReferenceTypeConstraint |
                                    GenericParameterAttributes.NotNullableValueTypeConstraint)) == 0
                     && nullableFlag == 1)
                constraints.Add("notnull");
            if ((attributes & GenericParameterAttributes.DefaultConstructorConstraint) != 0
                && !constraints.Contains("struct", StringComparer.Ordinal)
                && !constraints.Contains("unmanaged", StringComparer.Ordinal))
                constraints.Add("new()");
            constraints.AddRange(argument.GetGenericParameterConstraints()
                .Where(item => !unmanaged || item != typeof(ValueType))
                .Select(static item => FormatType(item)));
            if (constraints.Count > 0)
                lines.Add($"{indent}where {argument.Name} : {string.Join(", ", constraints)}");
        }
    }

    private static string FormatType(Type type, NullabilityInfo? nullability = null)
    {
        if (type.IsByRef) return FormatType(type.GetElementType()!, nullability?.ElementType ?? nullability);
        if (type.IsArray)
            return $"{FormatType(type.GetElementType()!, nullability?.ElementType)}[{new string(',', type.GetArrayRank() - 1)}]{NullableSuffix(type, nullability)}";
        if (type.IsPointer) return $"{FormatType(type.GetElementType()!)}*";
        if (type.IsGenericParameter) return type.Name + NullableSuffix(type, nullability);
        if (!type.IsGenericType) return (type.FullName ?? type.Name).Replace('+', '.') + NullableSuffix(type, nullability);

        var definition = type.GetGenericTypeDefinition();
        var name = Regex.Replace(
            (definition.FullName ?? definition.Name).Replace('+', '.'),
            "`[0-9]+",
            string.Empty);
        var arguments = type.GetGenericArguments();
        var nullableArguments = nullability?.GenericTypeArguments ?? [];
        var rendered = arguments.Select((argument, index) =>
            FormatType(argument, index < nullableArguments.Length ? nullableArguments[index] : null));
        return $"{name}<{string.Join(", ", rendered)}>{NullableSuffix(type, nullability)}";
    }

    private static string NullableSuffix(Type type, NullabilityInfo? nullability) =>
        !type.IsValueType && nullability?.ReadState == NullabilityState.Nullable ? "?" : string.Empty;

    private static NullabilityInfo? TryCreate(NullabilityInfoContext context, PropertyInfo property)
    {
        try { return context.Create(property); }
        catch { return null; }
    }

    private static NullabilityInfo? TryCreate(NullabilityInfoContext context, FieldInfo field)
    {
        try { return context.Create(field); }
        catch { return null; }
    }

    private static NullabilityInfo? TryCreate(NullabilityInfoContext context, EventInfo @event)
    {
        try { return context.Create(@event); }
        catch { return null; }
    }

    private static string FormatReturn(MethodInfo method, NullabilityInfo? nullability)
    {
        if (!method.ReturnType.IsByRef) return FormatType(method.ReturnType, nullability);
        var readOnly = method.ReturnParameter.GetRequiredCustomModifiers()
            .Any(static modifier => modifier.FullName is "System.Runtime.CompilerServices.IsReadOnlyAttribute"
                                                   or "System.Runtime.InteropServices.InAttribute");
        return $"ref{(readOnly ? " readonly" : string.Empty)} {FormatType(method.ReturnType, nullability)}";
    }

    private static string FormatNullability(NullabilityInfo? nullability, string prefix = "nullability")
    {
        if (nullability is null) return $" [{prefix}=unknown]";
        return $" [{prefix}=read:{FormatNullabilityState(nullability.ReadState)},write:{FormatNullabilityState(nullability.WriteState)}]";
    }

    private static string FormatNullabilityState(NullabilityState state) => state switch
    {
        NullabilityState.NotNull => "not-null",
        NullabilityState.Nullable => "nullable",
        _ => "unknown"
    };

    private static string FormatCustomModifiers(ParameterInfo parameter)
    {
        var required = parameter.GetRequiredCustomModifiers()
            .Select(static modifier => modifier.FullName ?? modifier.Name)
            .Order(StringComparer.Ordinal);
        var optional = parameter.GetOptionalCustomModifiers()
            .Select(static modifier => modifier.FullName ?? modifier.Name)
            .Order(StringComparer.Ordinal);
        var requiredText = string.Join(",", required);
        var optionalText = string.Join(",", optional);
        if (requiredText.Length == 0 && optionalText.Length == 0) return string.Empty;
        return $" [modreq={requiredText};modopt={optionalText}]";
    }

    private static bool IsInitOnly(MethodInfo setter) =>
        setter.ReturnParameter.GetRequiredCustomModifiers().Any(
            static modifier => modifier.FullName == "System.Runtime.CompilerServices.IsExternalInit");

    private static bool HasAttribute(MemberInfo member, string attributeFullName) =>
        member.CustomAttributes.Any(attribute => attribute.AttributeType.FullName == attributeFullName);

    private static bool HasAttribute(Type type, string attributeFullName) =>
        type.CustomAttributes.Any(attribute => attribute.AttributeType.FullName == attributeFullName);

    private static int? GenericNullableFlag(Type argument)
    {
        var attribute = argument.CustomAttributes.FirstOrDefault(
            static item => item.AttributeType.FullName == "System.Runtime.CompilerServices.NullableAttribute");
        if (attribute is null || attribute.ConstructorArguments.Count == 0) return null;
        var value = attribute.ConstructorArguments[0];
        if (value.Value is byte flag) return flag;
        if (value.Value is IReadOnlyCollection<CustomAttributeTypedArgument> flags
            && flags.FirstOrDefault().Value is byte first)
            return first;
        return null;
    }

    private static string FormatNullableFlag(int? flag) => flag switch
    {
        1 => "not-null",
        2 => "nullable",
        0 => "oblivious",
        _ => "unspecified"
    };

    private static NullabilityInfo? TryCreate(NullabilityInfoContext context, ParameterInfo parameter)
    {
        try { return context.Create(parameter); }
        catch { return null; }
    }

    private static string FormatValue(object? value) => value switch
    {
        null => "null",
        string text => $"\"{text.Replace("\\", "\\\\").Replace("\"", "\\\"")}\"",
        char character => $"'{character}'",
        bool boolean => boolean ? "true" : "false",
        Enum enumValue => $"{FormatType(enumValue.GetType())}.{enumValue}",
        IFormattable formattable => formattable.ToString(null, CultureInfo.InvariantCulture) ?? string.Empty,
        _ => value.ToString() ?? string.Empty
    };

    private static string FormatBases(IReadOnlyCollection<string> bases) =>
        bases.Count == 0 ? string.Empty : $" : {string.Join(", ", bases)}";

    private static bool IsAccessor(MethodInfo method) =>
        method.IsSpecialName && (method.Name.StartsWith("get_", StringComparison.Ordinal)
                                 || method.Name.StartsWith("set_", StringComparison.Ordinal)
                                 || method.Name.StartsWith("add_", StringComparison.Ordinal)
                                 || method.Name.StartsWith("remove_", StringComparison.Ordinal));

    private static string FieldKey(FieldInfo field) => $"{field.Name}:{FormatType(field.FieldType)}";
    private static string PropertyKey(PropertyInfo property) =>
        $"{property.Name}:{FormatType(property.PropertyType)}:{string.Join(',', property.GetIndexParameters().Select(static item => FormatType(item.ParameterType)))}";
    private static string MethodKey(MethodBase method) =>
        $"{method.Name}`{(method.IsGenericMethod ? method.GetGenericArguments().Length : 0)}({string.Join(',', method.GetParameters().Select(static item => FormatType(item.ParameterType)))})";
}
