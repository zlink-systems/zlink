using System.Diagnostics.Metrics;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;

namespace Bingo.Server.Configuration;

public static class BingoMetricsRegistration
{
    public static IServiceCollection AddBingoMetrics(this IServiceCollection services)
    {
        services.AddSingleton<BingoMetricsListener>();
        services.AddSingleton<IHostedService>(static provider =>
            provider.GetRequiredService<BingoMetricsListener>());
        return services;
    }
}

public sealed class BingoMetricsListener(
    ILogger<BingoMetricsListener> logger) : IHostedService, IDisposable
{
    // The framework exposes diagnostics through the standard .NET meter.
    // It does not add a zlink-specific public monitoring interface.
    private const string FrameworkMeterName = "zlink.framework";

    private readonly MeterListener _listener = new();
    private PeriodicTimer? _observableTimer;
    private CancellationTokenSource? _stop;
    private Task? _poll;

    public Task StartAsync(CancellationToken cancellationToken)
    {
        _listener.InstrumentPublished = static (instrument, listener) =>
        {
            if (instrument.Meter.Name == FrameworkMeterName)
                listener.EnableMeasurementEvents(instrument);
        };
        _listener.SetMeasurementEventCallback<long>(LogLong);
        _listener.SetMeasurementEventCallback<double>(LogDouble);
        _listener.Start();

        _stop = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        _observableTimer = new PeriodicTimer(TimeSpan.FromSeconds(1));
        _poll = PollObservableMetricsAsync(_stop.Token);
        return Task.CompletedTask;
    }

    public async Task StopAsync(CancellationToken cancellationToken)
    {
        if (_stop is null) return;
        await _stop.CancelAsync().ConfigureAwait(false);
        if (_poll is not null)
            try
            {
                await _poll.WaitAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (_stop.IsCancellationRequested)
            {
            }
    }

    public void Dispose()
    {
        _stop?.Dispose();
        _observableTimer?.Dispose();
        _listener.Dispose();
    }

    private async Task PollObservableMetricsAsync(CancellationToken cancellationToken)
    {
        try
        {
            while (_observableTimer is not null
                   && await _observableTimer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false))
                _listener.RecordObservableInstruments();
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
    }

    private void LogLong(
        Instrument instrument,
        long value,
        ReadOnlySpan<KeyValuePair<string, object?>> tags,
        object? state) => Log(instrument, value, tags);

    private void LogDouble(
        Instrument instrument,
        double value,
        ReadOnlySpan<KeyValuePair<string, object?>> tags,
        object? state) => Log(instrument, value, tags);

    private void Log<T>(
        Instrument instrument,
        T value,
        ReadOnlySpan<KeyValuePair<string, object?>> tags)
    {
        if (!logger.IsEnabled(LogLevel.Information)) return;
        var attributes = string.Join(
            ",",
            tags.ToArray().Select(static tag => $"{tag.Key}={tag.Value}"));
        logger.LogInformation(
            "zlink metric name={MetricName} value={MetricValue} tags={MetricTags}",
            instrument.Name,
            value,
            attributes);
    }
}
