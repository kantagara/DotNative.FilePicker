#pragma once
#include <DotNativePlugins.hpp>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <vector>
namespace dotnative_file_picker {
class Files {
    struct File {
        std::filesystem::path path;
        std::string name;
        int64_t length;
    };
    std::map<int64_t, File> files;
    std::map<int64_t, std::unique_ptr<std::ifstream>> streams;
    int64_t next = 0;
    int64_t Id() {
        if (next == INT64_MAX || files.size() + streams.size() >= 256)
            throw std::runtime_error("File picker resource limit reached");
        return ++next;
    }

  public:
    dotnative::Value Add(const std::filesystem::path &path) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec)
            throw std::runtime_error("Selected path is not a readable local file");
        auto size = std::filesystem::file_size(path, ec);
        if (ec || size > uint64_t(INT64_MAX))
            throw std::runtime_error("Cannot read selected file metadata");
        auto id = Id();
        auto name = path.filename().u8string();
        files.emplace(id, File{path, std::string(name.begin(), name.end()), int64_t(size)});
        return dotnative::Value::Map{
            {"handle", id}, {"name", files.at(id).name}, {"length", files.at(id).length}};
    }
    void Perform(const std::string &method, const dotnative::Value &arguments,
                 const std::shared_ptr<dotnative::Reply> &reply) {
        try {
            const auto &a = arguments.As<dotnative::Value::Map>();
            const auto id = std::get<int64_t>(a.at("handle").data);
            if (method == "openRead") {
                auto it = files.find(id);
                if (it == files.end())
                    throw std::runtime_error("Selected file was released");
                auto stream = std::make_unique<std::ifstream>(it->second.path, std::ios::binary);
                if (!*stream)
                    throw std::runtime_error("Cannot open selected file");
                auto sid = Id();
                streams.emplace(sid, std::move(stream));
                reply->Success(sid);
                return;
            }
            if (method == "read") {
                auto it = streams.find(id);
                if (it == streams.end())
                    throw std::runtime_error("File stream is closed");
                const auto count = std::get<int64_t>(a.at("count").data);
                if (count < 1 || count > 65536)
                    throw std::runtime_error("Invalid file read length");
                std::vector<uint8_t> bytes(static_cast<std::size_t>(count), uint8_t{});
                it->second->read(reinterpret_cast<char *>(bytes.data()),
                                 std::streamsize(bytes.size()));
                bytes.resize(size_t(it->second->gcount()));
                reply->Success(std::move(bytes));
                return;
            }
            if (method == "closeStream") {
                streams.erase(id);
                reply->Success();
                return;
            }
            if (method == "releaseFile") {
                files.erase(id);
                reply->Success();
                return;
            }
            reply->Error("not_implemented", "Unknown file picker method");
        } catch (const std::exception &e) {
            reply->Error("file_access_failed", e.what());
        }
    }
    void Reset() {
        streams.clear();
        files.clear();
    }
};
} // namespace dotnative_file_picker
