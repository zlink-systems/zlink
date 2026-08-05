namespace Zlink.Framework.Runtime.Spots;

internal abstract class ZLinkSpotCall
{
    private readonly Func<
        string?,
        ZLinkMessage,
        TimeSpan,
        CancellationToken,
        ValueTask<ZLinkSpotCreateResult>> _submit;
    private string? _meshName;
    private ZLinkMessage _request = ZLinkMessage.Empty;
    private TimeSpan? _timeout;
    private bool _requestSet;
    private int _submitted;

    protected ZLinkSpotCall(
        Func<
            string?,
            ZLinkMessage,
            TimeSpan,
            CancellationToken,
            ValueTask<ZLinkSpotCreateResult>> submit)
    {
        _submit = submit;
    }

    protected void SetMesh(string value)
    {
        if (_meshName is not null) Duplicate("InMesh");
        _meshName = Required(value, nameof(value));
    }

    protected void SetRequest(ZLinkMessage value)
    {
        ArgumentNullException.ThrowIfNull(value);
        if (_requestSet) Duplicate("Request");
        _request = value;
        _requestSet = true;
    }

    protected void SetTimeout(TimeSpan value)
    {
        if (_timeout is not null) Duplicate("Timeout");
        if (value <= TimeSpan.Zero)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "Timeout must be positive.");
        _timeout = value;
    }

    protected ValueTask<ZLinkSpotCreateResult> SubmitAsync(
        TimeSpan defaultTimeout,
        CancellationToken cancellationToken)
    {
        if (Interlocked.Exchange(ref _submitted, 1) != 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "The User Spot call was already submitted.");
        return _submit(
            _meshName,
            _request,
            _timeout ?? defaultTimeout,
            cancellationToken);
    }

    private static string Required(string value, string parameter)
    {
        if (string.IsNullOrWhiteSpace(value)
            || System.Text.Encoding.UTF8.GetByteCount(value) > byte.MaxValue
            || value.Contains('\0'))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"{parameter} must be 1..255 UTF-8 bytes without NUL.");
        return value;
    }

    private static void Duplicate(string option) =>
        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.InvalidOperation,
            $"{option} was already configured.");
}

internal sealed class ZLinkSpotCreateCall(
    TimeSpan defaultTimeout,
    Func<
        string?,
        ZLinkMessage,
        TimeSpan,
        CancellationToken,
        ValueTask<ZLinkSpotCreateResult>> submit)
    : ZLinkSpotCall(submit), IZLinkSpotCreateCall
{
    private readonly ZLinkSerialTurn? _turn = ZLinkSerialTurn.Current;

    public IZLinkSpotCreateCall InMesh(string meshName)
    {
        SetMesh(meshName);
        return this;
    }

    public IZLinkSpotCreateCall Request(ZLinkMessage request)
    {
        SetRequest(request);
        return this;
    }

    public IZLinkSpotCreateCall Request<TRequest>(TRequest request)
    {
        SetRequest(ZLinkMessage.From(request));
        return this;
    }

    public IZLinkSpotCreateCall Timeout(TimeSpan timeout)
    {
        SetTimeout(timeout);
        return this;
    }

    public ValueTask<ZLinkSpotCreateResult> Async(
        CancellationToken cancellationToken = default) =>
        SubmitAsync(defaultTimeout, cancellationToken);

    public ValueTask<ZLinkSpotCreateResult> Yield(
        CancellationToken cancellationToken = default) =>
        ZLinkApplicationExecutionContext
            .RequireYieldTurn(_turn, "User Spot creation")
            .YieldFrameworkCallAsync(
                token => SubmitAsync(defaultTimeout, token),
                cancellationToken);
}

internal sealed class ZLinkSpotGetOrCreateCall(
    TimeSpan defaultTimeout,
    Func<
        string?,
        ZLinkMessage,
        TimeSpan,
        CancellationToken,
        ValueTask<ZLinkSpotCreateResult>> submit)
    : ZLinkSpotCall(submit), IZLinkSpotGetOrCreateCall
{
    private readonly ZLinkSerialTurn? _turn = ZLinkSerialTurn.Current;

    public IZLinkSpotGetOrCreateCall InMesh(string meshName)
    {
        SetMesh(meshName);
        return this;
    }

    public IZLinkSpotGetOrCreateCall Request(ZLinkMessage request)
    {
        SetRequest(request);
        return this;
    }

    public IZLinkSpotGetOrCreateCall Request<TRequest>(TRequest request)
    {
        SetRequest(ZLinkMessage.From(request));
        return this;
    }

    public IZLinkSpotGetOrCreateCall Timeout(TimeSpan timeout)
    {
        SetTimeout(timeout);
        return this;
    }

    public ValueTask<ZLinkSpotCreateResult> Async(
        CancellationToken cancellationToken = default) =>
        SubmitAsync(defaultTimeout, cancellationToken);

    public ValueTask<ZLinkSpotCreateResult> Yield(
        CancellationToken cancellationToken = default) =>
        ZLinkApplicationExecutionContext
            .RequireYieldTurn(_turn, "User Spot get-or-create")
            .YieldFrameworkCallAsync(
                token => SubmitAsync(defaultTimeout, token),
                cancellationToken);
}
