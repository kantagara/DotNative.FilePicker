# DotNative.FilePicker

A source-based channel plugin for Android, iOS, macOS, Windows, and Linux. It uses `ACTION_OPEN_DOCUMENT` on Android, `UIDocumentPickerViewController` on iOS, AppKit `NSOpenPanel` on macOS, Windows `IFileOpenDialog`, and GTK4 `GtkFileChooserNative` on Linux. Files are exposed as scoped, read-only streams; the core renderer has no file-picker-specific code.

```csharp
using DotNative.FilePicker;

builder.Services.AddFilePicker();
```

Inject `IFilePicker` into a screen or service after referencing this package:

```csharp
await using var file = await picker.PickAsync(cancellationToken);
if (file is null)
    return; // The user dismissed the picker.

await using var input = await file.OpenReadAsync(cancellationToken);
var buffer = new byte[4096];
var count = await input.ReadAsync(buffer, cancellationToken);
```

`PickedFile` exposes `Name` and nullable `Length`. It does not expose a device path: Android document providers return URIs, and a path on a phone is not a path on the remote development host. Dispose files and streams with `await using`. Reads are asynchronous, sequential and limited to 64 KiB per channel request. An in-flight read completes before cancellation is observed on a subsequent operation, so returned bytes are never silently discarded.

The system picker grants access to the selected document. This plugin does not request broad filesystem permission or persist access across sessions. iOS security-scoped access and native streams are released explicitly or when the session ends. Native file I/O runs on a worker queue; presentation and replies use the platform main thread.

User dismissal returns `null`. Cancellation via the supplied token throws `OperationCanceledException`. Missing plugins, inaccessible documents and unavailable presenters produce `PluginException` with a stable `Code`. The application should display those errors; an access denial does not abort the Rust engine.

## Building

The package includes native sources, a `dotnative/plugin.json` manifest and `buildTransitive` targets. The CLI discovers package plugins from restored `project.assets.json` and selects platform sources for the target. DotNative.App compiles/stages transitive inputs during publish. Repository previews also discover source `ProjectReference`s. No per-app source edits are needed for implementation selection.

The C# facade and native implementations use the same channel name directly:
`dotnative.file-picker`. Channel files and code generation are not required.

Method names remain strings inside the plugin implementation. Application code sees only `IFilePicker`; later contract generation can replace the internal method strings without changing the transport.

Current scope: one local file of any type and a read-only stream. Type filters, multiple selection, and persistent grants are not included. Windows uses a modal native dialog; cancellation cannot dismiss that dialog until it returns. Linux requires an active GTK4 window and local filesystem selection. An unavailable presenter, non-local document provider, or native access failure is returned as a `PluginException`, never as an empty successful selection. Native source edits require restarting `dotnative run`; C# UI edits use the existing hot reload session.

| Platform | Picker | File access | Verification |
| --- | --- | --- | --- |
| Android | System document provider | Provider URI, scoped for the operation | Existing Android plugin fixture; device/OEM behavior may vary |
| iOS | Document picker | Security-scoped URL and coordinated reads | Existing iOS source/build path; physical-device behavior needs separate validation |
| macOS | AppKit `NSOpenPanel` sheet | Security-scoped URL and coordinated reads | Apple Silicon app preview selected a fixture and read its bytes through the managed stream |
| Windows | Modal `IFileOpenDialog` | Local path, scoped stream | Windows 11 ARM64 native harness selected a fixture, read it through the plugin handlers, and released the stream; full renderer could not run because the VM lacks Rust |
| Linux | GTK4 `GtkFileChooserNative` | Local path, scoped stream | Debian 12 ARM64 GTK4 4.8.3 Xvfb app preview selected a fixture and read its bytes through the NativeAOT DevHost; NuGet manifest discovery and native archive build passed |

## Service access

Import `DotNative.FilePicker` to access the plugin through `IServiceProvider`:

```csharp
using DotNative.FilePicker;

var plugin = services.FilePicker;
```

The getter calls `GetRequiredService<IFilePicker>()` on every access, preserving
DI lifetimes and the usual missing-registration error. Register the plugin with
`AddFilePicker(...)` before building the provider.

A `net10.0` application uses the property syntax with C# 14 or later. A
`net9.0` application uses only the method equivalent:

```csharp
var plugin = services.FilePicker();
```

The package contains separate `net9.0` and `net10.0` assemblies. NuGet selects
the assembly matching the application target framework. `NET10_0_OR_GREATER`
selects the property; the `#else` branch selects the method.

Build and pack both targets with .NET 10 SDK. A source build using .NET 9 SDK
builds only `net9.0`; it does not produce the .NET 10 assembly.
