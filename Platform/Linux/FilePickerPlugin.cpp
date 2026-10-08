#include "NativeFilePickerFiles.hpp"
#include <DotNativePlugins.hpp>
#include <gtk/gtk.h>
#include <memory>
#include <string>
namespace {
struct State;
struct Pick {
    std::shared_ptr<State> state;
    GtkFileChooserNative *dialog;
    int64_t operation;
    std::shared_ptr<dotnative::Reply> reply;
};
struct State {
    dotnative_file_picker::Files files;
    Pick *active = nullptr;
    GtkWindow *parent = nullptr;
};
void Finish(Pick *pick, int response) {
    auto state = pick->state;
    auto *dialog = pick->dialog;
    auto reply = pick->reply;
    if (state->active == pick) {
        state->active = nullptr;
        if (response == GTK_RESPONSE_ACCEPT) {
            GFile *file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
            char *path = file ? g_file_get_path(file) : nullptr;
            if (path) {
                try {
                    reply->Success(state->files.Add(std::filesystem::path(path)));
                } catch (const std::exception &e) {
                    reply->Error("file_access_failed", e.what());
                }
            } else
                reply->Error("unsupported_provider", "The selected item is not a local file.");
            g_free(path);
            if (file)
                g_object_unref(file);
        } else
            reply->Success();
    }
    gtk_native_dialog_hide(GTK_NATIVE_DIALOG(dialog));
    g_object_unref(dialog);
}
void Response(GtkNativeDialog *, int response, gpointer data) {
    Finish(static_cast<Pick *>(data), response);
}
void DestroyPick(gpointer data, GClosure *) { delete static_cast<Pick *>(data); }
} // namespace
void FilePickerPlugin(const dotnative::PluginRegistrar &registrar) {
    auto state = std::make_shared<State>();
    state->parent = static_cast<GtkWindow *>(registrar.NativeWindow());
    auto &channel = registrar.Channel("dotnative.file-picker");
    channel.OnReset = [state] {
        if (state->active) {
            auto *pick = state->active;
            state->active = nullptr;
            gtk_native_dialog_hide(GTK_NATIVE_DIALOG(pick->dialog));
            g_object_unref(pick->dialog);
        }
        state->files.Reset();
    };
    channel.Handle("pick", [state](const dotnative::Value &args,
                                   std::shared_ptr<dotnative::Reply> reply) {
        if (state->active) {
            reply->Error("busy", "A file picker is already open.");
            return;
        }
        if (!state->parent) {
            reply->Error("unavailable", "No active desktop window.");
            return;
        }
        int64_t operation =
            std::get<int64_t>(args.As<dotnative::Value::Map>().at("operation").data);
        auto *dialog = gtk_file_chooser_native_new("Select a file", state->parent,
                                                   GTK_FILE_CHOOSER_ACTION_OPEN, "Open", "Cancel");
        gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), FALSE);
        auto *pick = new Pick{state, dialog, operation, reply};
        state->active = pick;
        reply->OnCancel = [state, pick] {
            if (state->active == pick) {
                state->active = nullptr;
                gtk_native_dialog_hide(GTK_NATIVE_DIALOG(pick->dialog));
                g_object_unref(pick->dialog);
            }
        };
        g_signal_connect_data(dialog, "response", G_CALLBACK(Response), pick, DestroyPick,
                              G_CONNECT_DEFAULT);
        gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
    });
    channel.Handle("cancelPick",
                   [state](const dotnative::Value &args, std::shared_ptr<dotnative::Reply> reply) {
                       const auto operation =
                           std::get<int64_t>(args.As<dotnative::Value::Map>().at("operation").data);
                       if (state->active && state->active->operation == operation) {
                           auto *pick = state->active;
                           state->active = nullptr;
                           pick->reply->Success();
                           gtk_native_dialog_hide(GTK_NATIVE_DIALOG(pick->dialog));
                           g_object_unref(pick->dialog);
                       }
                       reply->Success();
                   });
    for (const auto *method : {"openRead", "read", "closeStream", "releaseFile"})
        channel.Handle(method, [state, method](const auto &args, auto reply) {
            state->files.Perform(method, args, reply);
        });
}
