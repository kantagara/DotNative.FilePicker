import UIKit
import UniformTypeIdentifiers

@MainActor final class FilePickerPlugin: NSObject, UIDocumentPickerDelegate {
    private static var instance: FilePickerPlugin?
    static func register() {
        if instance == nil {
            instance = FilePickerPlugin()
        }
    }
    private var pending: PluginReply?
    private var operation: Int64?
    private weak var picker: UIDocumentPickerViewController?
    private let files = FileResources()

    override init() {
        super.init()
        let channel = NativeChannels.channel("dotnative.file-picker")
        channel.onReset = {
            [self] in files.reset()
        }
        channel.handle("pick") {
            [self] args, reply in pick(args, reply)
        }
        channel.handle("cancelPick") {
            [self] args, reply in
            if let id = args.fields["operation"]?.integer, operation == id {
                pending?.success()
                pending = nil
                operation = nil
                picker?.dismiss(animated: true)
            }
            reply.success()
        }
        for method in ["openRead", "read", "closeStream", "releaseFile"] {
            channel.handle(method) {
                [self] arguments, reply in
                files.perform(method, arguments) {
                    result in
                    Task {
                        @MainActor in
                        switch result {
                        case .success(let value):
                            if !reply.success(value), method == "openRead" {
                                self.files.perform("closeStream", .map(["handle": value])) {
                                    _ in
                                }
                            }
                        case .failure(let error):
                            reply.failure("file_access_failed", error.localizedDescription)
                        }
                    }
                }
            }
        }
    }

    private func pick(_ args: PluginValue, _ reply: PluginReply) {
        guard pending == nil else {
            reply.failure("busy", "A picker is already open.")
            return
        }
        guard let presenter = NativeChannels.presenter, presenter.viewIfLoaded?.window != nil else {
            reply.failure("unavailable", "No active screen.")
            return
        }
        pending = reply
        operation = args.fields["operation"]?.integer
        let controller = UIDocumentPickerViewController(
            forOpeningContentTypes: [.item], asCopy: false)
        controller.allowsMultipleSelection = false
        controller.delegate = self
        picker = controller
        reply.onCancel = {
            [weak self, weak controller, weak reply] in
            if self?.pending === reply {
                self?.pending = nil
            }
            controller?.dismiss(animated: true)
        }
        presenter.present(controller, animated: true)
    }

    func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
        pending?.success()
        pending = nil
    }

    func documentPicker(
        _ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]
    ) {
        guard let reply = pending else {
            return
        }
        pending = nil
        guard let url = urls.first else {
            reply.success()
            return
        }
        files.register(url) {
            result in
            Task {
                @MainActor in
                switch result {
                case .success(let value):
                    if !reply.success(value) {
                        self.files.perform("releaseFile", value) {
                            _ in
                        }
                    }
                case .failure(let error):
                    reply.failure("file_access_failed", error.localizedDescription)
                }
            }
        }
    }
}
private final class FileResources {
    private struct File {
        let url: URL
        let scoped: Bool
    }
    private struct Reader {
        let handle: FileHandle
        let file: File
    }
    private let queue = DispatchQueue(label: "dotnative.file-picker.io")
    private var next: Int64 = 0
    private var files: [Int64: File] = [:]
    private var streams: [Int64: Reader] = [:]
    func reset() {
        queue.async {
            [self] in
            for stream in streams.values {
                try? stream.handle.close()
                if stream.file.scoped {
                    stream.file.url.stopAccessingSecurityScopedResource()
                }
            }
            for file in files.values where file.scoped {
                file.url.stopAccessingSecurityScopedResource()
            }
            streams.removeAll()
            files.removeAll()
        }
    }

    private func id() throws -> Int64 {
        guard next < Int64.max, files.count + streams.count < 256 else {
            throw CocoaError(.fileReadTooLarge)
        }
        next += 1
        return next
    }

    func register(_ url: URL, completion: @escaping (Result<PluginValue, Error>) -> Void) {
        queue.async {
            [self] in
            let scoped = url.startAccessingSecurityScopedResource()
            do {
                let info = try url.resourceValues(forKeys: [
                    .nameKey, .fileSizeKey, .isDirectoryKey,
                ])
                guard info.isDirectory != true else {
                    throw CocoaError(.fileReadUnsupportedScheme)
                }
                let handle = try id()
                files[handle] = File(url: url, scoped: scoped)
                completion(
                    .success(
                        .map([
                            "handle": .integer(handle),
                            "name": .string(info.name ?? url.lastPathComponent),
                            "length": info.fileSize.map {
                                .integer(Int64($0))
                            } ?? .null,
                        ])))
            } catch {
                if scoped {
                    url.stopAccessingSecurityScopedResource()
                }
                completion(.failure(error))
            }
        }
    }

    func perform(
        _ method: String, _ args: PluginValue,
        completion: @escaping (Result<PluginValue, Error>) -> Void
    ) {
        queue.async {
            [self] in
            do {
                guard let handle = args.fields["handle"]?.integer else {
                    throw CocoaError(.fileReadCorruptFile)
                }
                switch method {
                case "openRead":
                    guard let file = files[handle] else {
                        throw CocoaError(.fileNoSuchFile)
                    }
                    let stream = try id()
                    let scoped = file.url.startAccessingSecurityScopedResource()
                    do {
                        var failure: NSError?
                        var opened: Result<FileHandle, Error>?
                        NSFileCoordinator().coordinate(
                            readingItemAt: file.url, options: [], error: &failure
                        ) {
                            url in
                            opened = Result {
                                try FileHandle(forReadingFrom: url)
                            }
                        }
                        if let failure = failure {
                            throw failure
                        }
                        guard let opened = opened else {
                            throw CocoaError(.fileReadUnknown)
                        }
                        streams[stream] = Reader(
                            handle: try opened.get(), file: File(url: file.url, scoped: scoped))
                        completion(.success(.integer(stream)))
                    } catch {
                        if scoped {
                            file.url.stopAccessingSecurityScopedResource()
                        }
                        throw error
                    }
                case "read":
                    guard let stream = streams[handle], let count = args.fields["count"]?.integer,
                        count > 0,
                        count <= 65536
                    else {
                        throw CocoaError(.fileReadCorruptFile)
                    }
                    completion(
                        .success(.bytes(try stream.handle.read(upToCount: Int(count)) ?? Data())))
                case "closeStream":
                    if let stream = streams.removeValue(forKey: handle) {
                        defer {
                            if stream.file.scoped {
                                stream.file.url.stopAccessingSecurityScopedResource()
                            }
                        }
                        try stream.handle.close()
                    }
                    completion(.success(.null))
                case "releaseFile":
                    if let file = files.removeValue(forKey: handle), file.scoped {
                        file.url.stopAccessingSecurityScopedResource()
                    }
                    completion(.success(.null))
                default: throw CocoaError(.featureUnsupported)
                }
            } catch {
                completion(.failure(error))
            }
        }
    }
}
