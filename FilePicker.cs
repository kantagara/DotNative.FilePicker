using DotNative.Plugins;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.DependencyInjection.Extensions;

namespace DotNative.FilePicker;

public interface IFilePicker
{
    Task<PickedFile?> PickAsync(CancellationToken cancellationToken = default);
}

public static class FilePickerServices
{
    public static IServiceCollection AddFilePicker(this IServiceCollection services)
    {
        services.TryAddSingleton<IFilePicker, ChannelFilePicker>();
        return services;
    }
}

internal sealed class ChannelFilePicker(IPlatformChannels channels) : IFilePicker
{
    private static long _operation;

    public async Task<PickedFile?> PickAsync(CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var channel = channels.Get("dotnative.file-picker");
        var operation = Interlocked.Increment(ref _operation);
        var arguments = new Dictionary<string, object?> { ["operation"] = operation };
        // Keep receiving the original result after cancellation: a selected file
        // can race the cancel message, and its handle still needs releasing.
        var pending = channel.InvokeAsync("pick", arguments);
        using var registration = cancellationToken.Register(() =>
            _ = CancelPick(channel, arguments)
        );
        var result = await pending.ConfigureAwait(false);
        if (result is null)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return null;
        }
        if (
            result is not Dictionary<string, object?> map
            || map["handle"] is not long handle
            || map["name"] is not string name
        )
            throw new InvalidDataException("Invalid file picker result.");
        var file = new PickedFile(channel, handle, name, map.GetValueOrDefault("length") as long?);
        if (cancellationToken.IsCancellationRequested)
        {
            await file.DisposeAsync();
            cancellationToken.ThrowIfCancellationRequested();
        }
        return file;
    }

    private static async Task CancelPick(
        MethodChannel channel,
        Dictionary<string, object?> arguments
    )
    {
        try
        {
            await channel.InvokeAsync("cancelPick", arguments).ConfigureAwait(false);
        }
        catch (Exception)
        { /* Original request owns transport/session failure reporting. */
        }
    }
}

public sealed class PickedFile : IAsyncDisposable
{
    private readonly MethodChannel channel;

    private readonly long handle;

    internal PickedFile(MethodChannel channel, long handle, string name, long? length) =>
        (this.channel, this.handle, Name, Length) = (channel, handle, name, length);

    private int _disposed;

    public string Name { get; }

    public long? Length { get; }

    public async Task<Stream> OpenReadAsync(CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        // Once a resource-creating request is sent, receive its handle before
        // observing cancellation so a late native success cannot leak a stream.
        cancellationToken.ThrowIfCancellationRequested();
        var result = await channel.InvokeAsync("openRead", Args(handle)).ConfigureAwait(false);
        if (result is not long stream)
            throw new InvalidDataException("Invalid file stream handle.");
        var reader = new PluginReadStream(channel, stream);
        if (cancellationToken.IsCancellationRequested)
        {
            await reader.DisposeAsync();
            cancellationToken.ThrowIfCancellationRequested();
        }
        return reader;
    }

    internal static Dictionary<string, object?> Args(long id) => new() { ["handle"] = id };

    internal static async Task Release(MethodChannel channel, string method, long handle)
    {
        try
        {
            await channel.InvokeAsync(method, Args(handle)).ConfigureAwait(false);
        }
        catch (ObjectDisposedException)
        { /* Session reset owns remaining native resources. */
        }
        catch (PluginException error) when (error.Code == "disconnected") { }
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) == 0)
            await Release(channel, "releaseFile", handle).ConfigureAwait(false);
    }
}

internal sealed class PluginReadStream(MethodChannel channel, long handle) : Stream
{
    private readonly SemaphoreSlim _gate = new(1, 1);

    private bool _disposed;

    public override bool CanRead => !_disposed;

    public override bool CanSeek => false;

    public override bool CanWrite => false;

    public override long Length => throw new NotSupportedException();

    public override long Position
    {
        get => throw new NotSupportedException();
        set => throw new NotSupportedException();
    }

    public override int Read(byte[] buffer, int offset, int count) =>
        throw new NotSupportedException("Use ReadAsync for native plugin streams.");

    public override Task<int> ReadAsync(
        byte[] buffer,
        int offset,
        int count,
        CancellationToken cancellationToken
    ) => ReadAsync(buffer.AsMemory(offset, count), cancellationToken).AsTask();

    public override async ValueTask<int> ReadAsync(
        Memory<byte> buffer,
        CancellationToken cancellationToken = default
    )
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (buffer.Length == 0)
                return 0;
            // Do not cancel an in-flight read: discarding returned bytes would
            // advance the native stream and silently corrupt the next read.
            var args = PickedFile.Args(handle);
            args["count"] = Math.Min(buffer.Length, 65536);
            var result = await channel.InvokeAsync("read", args).ConfigureAwait(false);
            if (result is not byte[] bytes || bytes.Length > Math.Min(buffer.Length, 65536))
                throw new InvalidDataException("Invalid file read result.");
            bytes.CopyTo(buffer);
            return bytes.Length;
        }
        finally
        {
            _gate.Release();
        }
    }

    public override async ValueTask DisposeAsync()
    {
        await _gate.WaitAsync().ConfigureAwait(false);
        try
        {
            if (!_disposed)
            {
                _disposed = true;
                await PickedFile.Release(channel, "closeStream", handle).ConfigureAwait(false);
            }
        }
        finally
        {
            _gate.Release();
        }
        GC.SuppressFinalize(this);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing && !_disposed)
            throw new NotSupportedException("Use await using for plugin streams.");
        base.Dispose(disposing);
    }

    public override void Flush() => throw new NotSupportedException();

    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();

    public override void SetLength(long value) => throw new NotSupportedException();

    public override void Write(byte[] buffer, int offset, int count) =>
        throw new NotSupportedException();
}
