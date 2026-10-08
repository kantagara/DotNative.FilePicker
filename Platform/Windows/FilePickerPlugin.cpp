#define NOMINMAX
#include "NativeFilePickerFiles.hpp"
#include <DotNativePlugins.hpp>
#include <memory>
#include <shobjidl.h>
#include <string>
#include <windows.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
namespace {
struct State {
    dotnative_file_picker::Files files;
    HWND parent = nullptr;
    bool picking = false;
    int64_t operation = 0;
};
} // namespace
void FilePickerPlugin(const dotnative::PluginRegistrar &registrar) {
    auto state = std::make_shared<State>();
    state->parent = static_cast<HWND>(registrar.NativeWindow());
    auto &channel = registrar.Channel("dotnative.file-picker");
    channel.OnReset = [state] {
        state->picking = false;
        state->files.Reset();
    };
    channel.Handle("pick", [state](const dotnative::Value &args,
                                   std::shared_ptr<dotnative::Reply> reply) {
        if (state->picking) {
            reply->Error("busy", "A file picker is already open.");
            return;
        }
        if (!state->parent || !IsWindow(state->parent)) {
            reply->Error("unavailable", "No active desktop window.");
            return;
        }
        state->picking = true;
        state->operation = std::get<int64_t>(args.As<dotnative::Value::Map>().at("operation").data);
        auto finish = [state, reply] {
            state->picking = false;
            state->operation = 0;
        };
        reply->OnCancel = finish;
        const auto init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool uninit = SUCCEEDED(init);
        if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
            finish();
            reply->Error("unavailable", "Cannot initialize the Windows file dialog.");
            return;
        }
        IFileOpenDialog *dialog = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&dialog));
        if (SUCCEEDED(hr)) {
            FILEOPENDIALOGOPTIONS options{};
            hr = dialog->GetOptions(&options);
            if (SUCCEEDED(hr))
                hr = dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
            if (SUCCEEDED(hr))
                hr = dialog->SetTitle(L"Select a file");
            if (SUCCEEDED(hr))
                hr = dialog->Show(state->parent);
            if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
                finish();
                reply->Success();
            } else if (SUCCEEDED(hr)) {
                IShellItem *item = nullptr;
                hr = dialog->GetResult(&item);
                PWSTR path = nullptr;
                if (SUCCEEDED(hr))
                    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
                if (SUCCEEDED(hr) && path) {
                    try {
                        reply->Success(state->files.Add(std::filesystem::path(path)));
                    } catch (const std::exception &e) {
                        reply->Error("file_access_failed", e.what());
                    }
                } else
                    reply->Error("file_access_failed",
                                 "Windows could not return the selected file path.");
                if (path)
                    CoTaskMemFree(path);
                if (item)
                    item->Release();
                finish();
            } else {
                finish();
                reply->Error("picker_failed", "Windows could not open the file dialog.");
            }
            dialog->Release();
        } else {
            finish();
            reply->Error("picker_failed", "Windows could not create the file dialog.");
        }
        if (uninit)
            CoUninitialize();
    });
    channel.Handle("cancelPick",
                   [state](const dotnative::Value &args, std::shared_ptr<dotnative::Reply> reply) {
                       const auto operation =
                           std::get<int64_t>(args.As<dotnative::Value::Map>().at("operation").data);
                       // IFileDialog::Show is modal on the renderer thread. Its own native Cancel
                       // button always completes the original request; the managed token observes
                       // cancellation after the modal call returns and releases any late selection.
                       (void)operation;
                       reply->Success();
                   });
    for (const auto *method : {"openRead", "read", "closeStream", "releaseFile"})
        channel.Handle(method, [state, method](const auto &args, auto reply) {
            state->files.Perform(method, args, reply);
        });
}
