# Contributing

Keep public interfaces, registration methods and native handlers readable with
separate declarations, expanded blocks and consistent indentation.

Every DI plugin exposes exactly one service accessor per target framework. Put
the `IServiceProvider` extension property inside `#if NET10_0_OR_GREATER` and its
`Plugin()` extension method inside `#else`. The `net10.0` assembly requires C# 14
and uses the property; `net9.0` uses the method. Resolve through
`GetRequiredService<T>()` each time; never cache resolved services globally.
NuGet selects the assembly according to the consuming application target.

Build and pack both targets with .NET 10 SDK. Source builds with .NET 9 SDK
build only `net9.0`. Keep the standard SDK language defaults. A `net10.0` build
with an older language override reports DNPLUGIN002 before compilation.

The project owns plugin metadata. `build/PluginAuthoring.targets` generates the
NuGet plugin manifest during packing; do not maintain a second JSON manifest.
The build helpers are authoring inputs and are not imported into consumers.
Native `buildTransitive` rules are packaged to link and register platform code
in applications, including through transitive package references.

Format C# and project XML with CSharpier, Swift with `swift-format`, Kotlin with
`ktfmt --kotlinlang-style`, and C/C++ with `clang-format`. Follow `.editorconfig`.
