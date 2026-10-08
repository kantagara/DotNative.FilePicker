using System;
using Microsoft.Extensions.DependencyInjection;

namespace DotNative.FilePicker;

public static class FilePickerServiceProviderExtensions
{
#if NET10_0_OR_GREATER
    extension(IServiceProvider services)
    {
        /// <summary>Resolves the registered plugin using the provider's DI lifetime.</summary>
        public IFilePicker FilePicker => services.GetRequiredService<IFilePicker>();
    }
#else
    /// <summary>Resolves the registered plugin using the provider's DI lifetime.</summary>
    public static IFilePicker FilePicker(this IServiceProvider services) =>
        services.GetRequiredService<IFilePicker>();
#endif
}
