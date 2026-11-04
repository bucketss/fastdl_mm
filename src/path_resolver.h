#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

enum class ResolveStatus {
    Ok,
    InvalidPath,
    OutsideRoot,
    NotFound,
    NotAFile,
    DirectoryDenied,
    ExtensionDenied,
    TooLarge,
    IoError
};

struct ResolvedFile {
    ResolveStatus status = ResolveStatus::InvalidPath;
    std::filesystem::path path;
    std::uint64_t size = 0;
    std::string extension;
};

class PathResolver {
public:
    // dirs and types are comma separated. "*" allows everything; an empty list
    // allows nothing. Type entries may be written with or without a leading dot.
    bool configure(const std::filesystem::path& root, const std::string& dirs,
        const std::string& types, std::string& error);
    ResolvedFile resolve(const char* rawUrl, std::uint64_t maxBytes) const;
    const std::filesystem::path& root() const { return root_; }

    static std::vector<std::string> splitList(const std::string& value);

private:
    static bool validate(const char* rawUrl, std::filesystem::path& relative);
    static bool containedBy(
        const std::filesystem::path& root, const std::filesystem::path& candidate);
    static std::vector<std::string> componentsUnder(
        const std::filesystem::path& root, const std::filesystem::path& candidate);
    bool allowedDirectory(const std::string& name) const;
    bool allowedExtension(const std::string& extension) const;
#ifndef _WIN32
    bool foldCase(const std::filesystem::path& relative, std::filesystem::path& folded) const;
#endif

    std::filesystem::path root_;
    std::vector<std::string> dirs_;
    std::vector<std::string> types_;
    bool allDirs_ = false;
    bool allTypes_ = false;
};
