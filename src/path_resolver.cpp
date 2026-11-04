#include "path_resolver.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <system_error>
#include <vector>

namespace {
std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool componentEqual(const std::filesystem::path& left, const std::filesystem::path& right) {
#ifdef _WIN32
    return lowerAscii(left.u8string()) == lowerAscii(right.u8string());
#else
    return left == right;
#endif
}

bool missing(const std::error_code& ec) {
    return ec == std::errc::no_such_file_or_directory ||
        ec == std::errc::not_a_directory ||
        ec == std::errc::filename_too_long ||
        ec == std::errc::invalid_argument ||
        ec == std::errc::too_many_symbolic_link_levels;
}
} // namespace

#ifndef _WIN32
bool PathResolver::foldCase(
    const std::filesystem::path& relative, std::filesystem::path& folded) const {
    folded = root_;
    for (const auto& component : relative) {
        std::error_code ec;
        const auto exact = folded / component;
        if (std::filesystem::exists(std::filesystem::symlink_status(exact, ec))) {
            folded = exact;
            continue;
        }
        const auto wanted = lowerAscii(component.u8string());
        std::filesystem::path match;
        for (std::filesystem::directory_iterator it(folded, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (lowerAscii(it->path().filename().u8string()) != wanted) continue;
            if (!match.empty()) return false;
            match = it->path();
        }
        if (ec || match.empty()) return false;
        folded = match;
    }
    return true;
}
#endif

std::vector<std::string> PathResolver::splitList(const std::string& value) {
    std::vector<std::string> items;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto comma = value.find(',', start);
        const auto end = (comma == std::string::npos ? value.size() : comma);
        auto item = value.substr(start, end - start);
        const auto first = item.find_first_not_of(" \t");
        if (first != std::string::npos) {
            const auto last = item.find_last_not_of(" \t");
            items.push_back(lowerAscii(item.substr(first, last - first + 1)));
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return items;
}

bool PathResolver::configure(const std::filesystem::path& root, const std::string& dirs,
    const std::string& types, std::string& error) {
    std::error_code ec;
    auto absolute = std::filesystem::absolute(root, ec);
    if (ec) {
        error = "could not make root absolute: " + ec.message();
        return false;
    }
    auto canonical = std::filesystem::canonical(absolute, ec);
    if (ec || !std::filesystem::is_directory(canonical, ec)) {
        error = "root is not an accessible directory";
        return false;
    }
    root_ = canonical;

    dirs_ = splitList(dirs);
    allDirs_ = std::find(dirs_.begin(), dirs_.end(), "*") != dirs_.end();

    types_.clear();
    allTypes_ = false;
    for (const auto& item : splitList(types)) {
        if (item == "*") {
            allTypes_ = true;
            continue;
        }
        types_.push_back(item.front() == '.' ? item : "." + item);
    }
    return true;
}

bool PathResolver::allowedDirectory(const std::string& name) const {
    if (allDirs_) return true;
    return std::find(dirs_.begin(), dirs_.end(), name) != dirs_.end();
}

// MHD unescapes before invoking the handler, so the URL arrives decoded.
// Decoding again would make file names containing a literal '%' unreachable.
bool PathResolver::validate(const char* rawUrl, std::filesystem::path& relative) {
    if (rawUrl == nullptr) return false;
    const std::string input(rawUrl);
    if (input.empty() || input.size() > 2048 || input.front() != '/') return false;

    for (const char character : input) {
        const unsigned char value = static_cast<unsigned char>(character);
        if (value == 0 || value < 0x20 || value == 0x7f || value == '\\' ||
            value == ':' || value == '?' || value == '#') {
            return false;
        }
    }

    std::vector<std::string> components;
    std::size_t start = 1;
    while (start <= input.size()) {
        const auto slash = input.find('/', start);
        const auto end = (slash == std::string::npos ? input.size() : slash);
        const auto component = input.substr(start, end - start);
        // Empty components are repeated separators; collapsing them is what
        // makes a trailing slash on sv_downloadurl harmless, since the client
        // appends resource names verbatim ("//maps/foo.bsp").
        if (!component.empty()) {
            if (component == "." || component == ".." || component.size() > 255) {
                return false;
            }
            components.push_back(component);
        }
        if (components.size() > 12) return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (components.empty()) return false;

    relative.clear();
    for (const auto& component : components) relative /= std::filesystem::u8path(component);
    return true;
}

bool PathResolver::containedBy(
    const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() || !componentEqual(*rootIt, *candidateIt)) {
            return false;
        }
    }
    return true;
}

bool PathResolver::allowedExtension(const std::string& extension) const {
    if (allTypes_) return true;
    return std::find(types_.begin(), types_.end(), extension) != types_.end();
}

// Lowercase path components below the root. Containment must already hold.
std::vector<std::string> PathResolver::componentsUnder(
    const std::filesystem::path& root, const std::filesystem::path& candidate) {
    std::vector<std::string> parts;
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end()) return parts;
    }
    for (; candidateIt != candidate.end(); ++candidateIt) {
        parts.push_back(lowerAscii(candidateIt->u8string()));
    }
    return parts;
}

ResolvedFile PathResolver::resolve(const char* rawUrl, std::uint64_t maxBytes) const {
    ResolvedFile result;
    std::filesystem::path relative;
    if (!validate(rawUrl, relative)) return result;

    std::error_code ec;
    auto candidate = std::filesystem::canonical(root_ / relative, ec);
#ifndef _WIN32
    if (ec == std::errc::no_such_file_or_directory) {
        std::filesystem::path folded;
        if (foldCase(relative, folded)) {
            ec.clear();
            candidate = std::filesystem::canonical(folded, ec);
        }
    }
#endif
    if (ec) {
        result.status = missing(ec) ? ResolveStatus::NotFound : ResolveStatus::IoError;
        return result;
    }
    if (!containedBy(root_, candidate)) {
        result.status = ResolveStatus::OutsideRoot;
        return result;
    }
    // One directory_entry caches the query that the type and size checks below
    // would otherwise each repeat as their own stat.
    const std::filesystem::directory_entry entry(candidate, ec);
    if (ec) {
        result.status = ResolveStatus::IoError;
        return result;
    }
    if (!entry.is_regular_file(ec) || ec) {
        result.status = ResolveStatus::NotAFile;
        return result;
    }

    // Canonical path, not the requested one: a junction inside a served
    // directory could otherwise reach one that is not.
    const auto parts = componentsUnder(root_, candidate);
    result.extension = lowerAscii(candidate.extension().u8string());
    const bool rootWad = parts.size() == 1 && result.extension == ".wad";
    if (!rootWad && (parts.size() < 2 || !allowedDirectory(parts.front()))) {
        result.status = ResolveStatus::DirectoryDenied;
        return result;
    }

    if (!allowedExtension(result.extension)) {
        result.status = ResolveStatus::ExtensionDenied;
        return result;
    }
    result.size = entry.file_size(ec);
    if (ec) {
        result.status = ResolveStatus::IoError;
        return result;
    }
    if (result.size > maxBytes) {
        result.status = ResolveStatus::TooLarge;
        return result;
    }
    result.status = ResolveStatus::Ok;
    result.path = candidate;
    return result;
}
