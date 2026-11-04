#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "config_parser.h"
#include "fastdl_server.h"
#include "logger.h"
#include "path_resolver.h"
#include "rate_limiter.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
int failures = 0;

// Local "YYYY-MM-DD" offset by whole days, matching the logger's naming.
std::string dayStamp(int offsetDays) {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    local.tm_mday += offsetDays;
    local.tm_isdst = -1;
    std::mktime(&local);
    std::array<char, 40> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%04d-%02d-%02d",
        local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return buffer.data();
}

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void touch(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << contents;
}

#ifdef _WIN32
using RawSocket = SOCKET;
void closeRaw(RawSocket sock) { closesocket(sock); }
#else
using RawSocket = int;
constexpr RawSocket INVALID_SOCKET = -1;
void closeRaw(RawSocket sock) { close(sock); }
#endif

// Writes a request line verbatim and returns the status code, or 0 if nothing
// usable came back. Everything above the socket -- libmicrohttpd's own request
// line parser included -- is under test, which is why this cannot be done
// through PathResolver alone.
int rawStatus(std::uint16_t port, const std::string& requestLine) {
    const RawSocket sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) return 0;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closeRaw(sock);
        return 0;
    }

    const std::string request = requestLine +
        "\r\nHost: 127.0.0.1\r\nUser-Agent: Valve/Steam HTTP Client 1.0\r\n"
        "Connection: close\r\n\r\n";
    ::send(sock, request.data(), static_cast<int>(request.size()), 0);

    // The status line can be split across reads, so collect until end of line.
    std::string head;
    char buffer[256];
    while (head.find('\n') == std::string::npos && head.size() < 1024) {
        const auto received = ::recv(sock, buffer, sizeof(buffer), 0);
        if (received <= 0) break;
        head.append(buffer, static_cast<std::size_t>(received));
    }
    closeRaw(sock);

    const auto space = head.find(' ');
    if (head.rfind("HTTP/", 0) != 0 || space == std::string::npos) return 0;
    return std::atoi(head.c_str() + space + 1);
}
} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "fastdl_mm_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    touch(root / "maps/test.bsp", "bsp");
    touch(root / "maps/test.bsp.bz2", "compressed");
    touch(root / "maps/secret.dll", "no");
    touch(root / "addons/config.txt", "private");
    touch(root / "liblist.gam", "top level");
    touch(root / "top.txt", "top level");
    touch(root / "custom.wad", "wad");

    const std::string types = "bsp,nav,res,wad,mdl,spr,wav,mp3,bmp,tga,txt,htm,html,gz,bz2";

    PathResolver resolver;
    std::string error;
    check(resolver.configure(root, "maps,models,sound", types, error),
        "configure test root");
    check(resolver.resolve("/maps/test.bsp", 1024).status == ResolveStatus::Ok,
        "allow bsp");
    check(resolver.resolve("/maps/test.bsp.bz2", 1024).status == ResolveStatus::Ok,
        "allow bz2");
    check(resolver.resolve("/maps/secret.dll", 1024).status ==
        ResolveStatus::ExtensionDenied, "deny executable extension");
    check(resolver.resolve("/maps/test.bsp", 2).status == ResolveStatus::TooLarge,
        "enforce maximum file size");
    check(resolver.resolve("/../secret.bsp", 1024).status == ResolveStatus::InvalidPath,
        "deny dot-dot");
    // MHD unescapes before the handler, so "%2e%2e" is already ".." here.
    check(resolver.resolve("/maps/../secret.bsp", 1024).status ==
        ResolveStatus::InvalidPath, "deny dot-dot after unescaping");
    // A literal '%' is an ordinary file name character, not an escape.
    check(resolver.resolve("/maps/100%.bsp", 1024).status ==
        ResolveStatus::NotFound, "percent is not decoded a second time");
    check(resolver.resolve("/maps\\test.bsp", 1024).status ==
        ResolveStatus::InvalidPath, "deny backslash");
    check(resolver.resolve("/maps", 1024).status == ResolveStatus::NotAFile,
        "directory is not a file");
    // Repeated separators collapse, so a trailing slash on sv_downloadurl costs
    // nothing: the client appends resource names verbatim.
    check(resolver.resolve("//maps/test.bsp", 1024).status == ResolveStatus::Ok,
        "double slash from a trailing slash still resolves");
    check(resolver.resolve("/maps//test.bsp", 1024).status == ResolveStatus::Ok,
        "repeated separator anywhere is collapsed");
    check(resolver.resolve("///maps///test.bsp", 1024).status == ResolveStatus::Ok,
        "runs of separators are collapsed");
    check(resolver.resolve("/maps/", 1024).status == ResolveStatus::NotAFile,
        "trailing slash names the directory, not an invalid path");
    // Collapsing weakens nothing: a path is still only its components.
    check(resolver.resolve("/maps/..//test.bsp", 1024).status ==
        ResolveStatus::InvalidPath, "dot-dot beside a separator is still refused");
    check(resolver.resolve("//", 1024).status == ResolveStatus::InvalidPath,
        "separators alone name nothing");
    check(resolver.resolve("/", 1024).status == ResolveStatus::InvalidPath,
        "root alone names nothing");
    check(resolver.resolve("//addons/config.txt", 1024).status ==
        ResolveStatus::DirectoryDenied, "collapsing does not bypass the directory list");
    check(resolver.resolve("/maps/missing.wad", 1024).status ==
        ResolveStatus::NotFound, "missing file is identifiable");

    // Directory allowlist.
    check(resolver.resolve("/addons/config.txt", 1024).status ==
        ResolveStatus::DirectoryDenied, "deny directory off the list");
    check(resolver.resolve("/liblist.gam", 1024).status ==
        ResolveStatus::DirectoryDenied, "deny root-level file");
    check(resolver.resolve("/top.txt", 1024).status ==
        ResolveStatus::DirectoryDenied, "deny root-level file with allowed type");
    check(resolver.resolve("/custom.wad", 1024).status == ResolveStatus::Ok,
        "serve root-level wad");
#ifdef _WIN32
    check(resolver.resolve("/MAPS/test.bsp", 1024).status == ResolveStatus::Ok,
        "directory match is case insensitive");
#else
    // Case sensitive filesystem: the miss happens at canonical(), before the
    // allowlist is consulted.
    check(resolver.resolve("/MAPS/test.bsp", 1024).status == ResolveStatus::NotFound,
        "case-mismatched directory does not resolve");
#endif

    // Type allowlist is driven by the list, not a hardcoded table.
    PathResolver narrow;
    check(narrow.configure(root, "maps", "bsp", error), "configure narrow resolver");
    check(narrow.resolve("/maps/test.bsp", 1024).status == ResolveStatus::Ok,
        "narrow list allows its one type");
    check(narrow.resolve("/maps/test.bsp.bz2", 1024).status ==
        ResolveStatus::ExtensionDenied, "narrow list denies everything else");
    check(narrow.resolve("/custom.wad", 1024).status == ResolveStatus::ExtensionDenied,
        "root-level wad still needs wad in the type list");

    // Dotted entries and "*" wildcards.
    PathResolver dotted;
    check(dotted.configure(root, "*", ".bsp, .txt", error), "configure dotted resolver");
    check(dotted.resolve("/addons/config.txt", 1024).status == ResolveStatus::Ok,
        "wildcard directory allows any subdirectory");
    check(dotted.resolve("/top.txt", 1024).status == ResolveStatus::DirectoryDenied,
        "wildcard still refuses root-level files");
    check(dotted.resolve("/maps/secret.dll", 1024).status ==
        ResolveStatus::ExtensionDenied, "dotted type entries parse");

    // Empty lists deny rather than allow.
    PathResolver closed;
    check(closed.configure(root, "", "", error), "configure empty resolver");
    check(closed.resolve("/maps/test.bsp", 1024).status == ResolveStatus::DirectoryDenied,
        "empty directory list serves nothing");

    // Config parser.
    {
        std::string name;
        std::string value;
        const auto parse = [&](const char* line) {
            name.clear();
            value.clear();
            return config::parseAssignment(line, name, value);
        };

        check(parse("fastdl_port \"27015\"") && name == "fastdl_port" && value == "27015",
            "quoted assignment");
        check(parse("fastdl_port 27015") && value == "27015", "bare assignment");
        check(parse("  fastdl_port   \"27015\"  ") && value == "27015",
            "surrounding whitespace");
        check(parse("fastdl_port \"27015\" // trailing note") && value == "27015",
            "trailing comment is removed");

        // A "//" inside quotes is data. Stripping comments before parsing quotes
        // truncated these.
        check(parse("fastdl_log \"logs//fastdl.log\"") && value == "logs//fastdl.log",
            "double slash inside a quoted value survives");
        check(parse("fastdl_x \"http://example.com/a\"") && value == "http://example.com/a",
            "url in a quoted value survives");
        check(parse("fastdl_x \"http://example.com/a\" // note") &&
            value == "http://example.com/a",
            "comment after a value containing slashes still strips");

        check(!parse("// fastdl_port \"27015\""), "comment-only line is not an assignment");
        check(!parse(""), "blank line");
        check(!parse("   \t  "), "whitespace-only line");
        check(!parse("fastdl_port"), "name with no value");
        check(!parse("fastdl_port \"27015"), "unterminated quote");
        check(!parse("fastdl_port \"27015\" junk"), "trailing junk after a quoted value");
        check(parse("fastdl_port \"27015\"\r") && value == "27015", "CRLF line ending");

        // Comment-only lines must strip to nothing, or loading a generated
        // config warns about every line in its comment block.
        check(config::trim(config::stripComment("// fastdl_log_age  keep (default \"7\")")).empty(),
            "generated comment line strips to nothing");
        check(config::stripComment("fastdl_log \"a//b\"") == "fastdl_log \"a//b\"",
            "stripComment leaves a quoted double slash alone");
    }

    RateLimiter limiter;
    check(!limiter.requestExceeded("127.0.0.1", 2), "first request allowed");
    check(!limiter.requestExceeded("127.0.0.1", 2), "second request allowed");
    check(limiter.requestExceeded("127.0.0.1", 2), "third request limited");
    // A rejected request must not be recorded, or a retrying client keeps its
    // own window full and never drains back under the limit.
    for (int i = 0; i < 50; ++i) limiter.requestExceeded("127.0.0.1", 2);
    check(!limiter.requestExceeded("127.0.0.1", 3), "rejections did not grow the window");
    check(limiter.requestExceeded("127.0.0.1", 3), "limit applies again once served");
    limiter.recordDenial("127.0.0.2", 2, 180);
    check(!limiter.blocked("127.0.0.2"), "first denial does not block");
    limiter.recordDenial("127.0.0.2", 2, 180);
    check(limiter.blocked("127.0.0.2"), "denial threshold blocks");
    check(limiter.unblock("127.0.0.2"), "explicit unblock");
    check(!limiter.blocked("127.0.0.2"), "unblocked client stays clear");

    // Daily rotation and retention. Retention removes files, so check it keeps
    // what it should and leaves unrelated names alone.
    const auto logDir = std::filesystem::temp_directory_path() / "fastdl_mm_tests_log";
    std::filesystem::remove_all(logDir, ec);
    std::filesystem::create_directories(logDir, ec);
    const auto base = logDir / "fastdl.log";
    for (const int offset : {0, -2, -6, -7, -30}) {
        touch(logDir / ("fastdl-" + dayStamp(offset) + ".log"), "old\n");
    }
    touch(logDir / "fastdl.log", "not dated\n");
    touch(logDir / "other-2020-01-01.log", "unrelated\n");
    touch(logDir / "fastdl-notadate.log", "unrelated\n");

    {
        AsyncLogger logger;
        check(logger.start(base, 7), "logger starts");
        check(logger.pathForToday() == logDir / ("fastdl-" + dayStamp(0) + ".log"),
            "today's file is date stamped");
        logger.write("entry");
        logger.stop();
    }

    const auto exists = [&logDir](const std::string& name) {
        return std::filesystem::exists(logDir / name);
    };
    check(exists("fastdl-" + dayStamp(0) + ".log"), "keep today");
    check(exists("fastdl-" + dayStamp(-2) + ".log"), "keep within retention");
    check(exists("fastdl-" + dayStamp(-6) + ".log"), "keep oldest retained day");
    check(!exists("fastdl-" + dayStamp(-7) + ".log"), "drop the day past retention");
    check(!exists("fastdl-" + dayStamp(-30) + ".log"), "drop long expired");
    check(exists("fastdl.log"), "leave undated file alone");
    check(exists("other-2020-01-01.log"), "leave other prefixes alone");
    check(exists("fastdl-notadate.log"), "leave unparseable stamps alone");

    {
        std::ifstream today(logDir / ("fastdl-" + dayStamp(0) + ".log"));
        std::string contents((std::istreambuf_iterator<char>(today)),
            std::istreambuf_iterator<char>());
        check(contents.find("entry") != std::string::npos, "entry reaches today's file");
    }

    {
        AsyncLogger logger;
        check(logger.start(base, 0), "logger starts with retention disabled");
        logger.write("entry");
        logger.stop();
    }
    check(exists("fastdl-" + dayStamp(-30) + ".log") == false, "already removed stays removed");
    touch(logDir / ("fastdl-" + dayStamp(-30) + ".log"), "old\n");
    {
        AsyncLogger logger;
        check(logger.start(base, 0), "logger restarts with retention disabled");
        logger.write("entry");
        logger.stop();
    }
    check(exists("fastdl-" + dayStamp(-30) + ".log"),
        "retention 0 keeps everything");

    // A log file that cannot be opened reports the failure, and stays inert
    // afterwards: no thread is running, so writes must be discarded rather than
    // queued. The caller keeps serving with logging switched off.
    {
        const auto blockedDir =
            std::filesystem::temp_directory_path() / "fastdl_mm_tests_blocked";
        std::filesystem::remove_all(blockedDir, ec);
        std::filesystem::create_directories(blockedDir, ec);
        // Occupy today's name with a directory so the open cannot succeed.
        std::filesystem::create_directory(
            blockedDir / ("fastdl-" + dayStamp(0) + ".log"), ec);
        AsyncLogger logger;
        check(!logger.start(blockedDir / "fastdl.log", 7),
            "unwritable log path fails the start");
        for (int i = 0; i < 10000; ++i) logger.write("entry");
        logger.stop();
        check(std::filesystem::is_directory(
                  blockedDir / ("fastdl-" + dayStamp(0) + ".log"), ec),
            "writes after a failed start create no log file");
        std::filesystem::remove_all(blockedDir, ec);
    }

    // A directory that cannot be created is the same failure one level up: this
    // is what an HLDS root the server account cannot write looks like.
    {
        const auto blockedDir =
            std::filesystem::temp_directory_path() / "fastdl_mm_tests_blocked_parent";
        std::filesystem::remove_all(blockedDir, ec);
        std::filesystem::create_directories(blockedDir, ec);
        // A file where the log's parent directory needs to be.
        touch(blockedDir / "notadir", "occupied\n");
        AsyncLogger logger;
        check(!logger.start(blockedDir / "notadir" / "fastdl.log", 7),
            "uncreatable log directory fails the start");
        logger.write("entry");
        logger.stop();
        std::filesystem::remove_all(blockedDir, ec);
    }

    // Over a real socket, because the defect this guards against is in the
    // request line and never reaches a handler. The Steam client does not
    // percent-encode, so an asset with a space in its name arrives as
    // "GET /models/momoko/shu (2).mdl HTTP/1.1".
    {
#ifdef _WIN32
        WSADATA winsock{};
        WSAStartup(MAKEWORD(2, 2), &winsock);
#endif
        touch(root / "models/momoko/shu (2).mdl", "model");

        FastdlConfig config;
        config.bindAddress = "127.0.0.1";
        config.port = 18973;
        config.root = root;
        config.baseDir = root;
        config.serveDirs = "maps,models";
        config.serveTypes = types;
        config.logPath = root / "logs" / "fastdl.log";
        config.logAgeDays = 1;

        FastdlServer server;
        std::string serverError;
        const bool started = server.start(config, serverError);
        check(started, "test server binds the loopback port");
        if (started) {
            check(rawStatus(config.port, "GET /maps/test.bsp HTTP/1.1") == 200,
                "an ordinary request line is unaffected");
            check(rawStatus(config.port,
                "GET /models/momoko/shu%20(2).mdl HTTP/1.1") == 200,
                "an encoded space is served");
            check(rawStatus(config.port,
                "GET /models/momoko/shu (2).mdl HTTP/1.1") == 200,
                "an unencoded space in the request target is served");
            // Splitting on the first space used to turn this request into the
            // one below it, which must stay a miss.
            check(rawStatus(config.port, "GET /models/momoko/shu HTTP/1.1") == 404,
                "the truncated name is still not a file");
            server.stop();
        }
    }

    std::filesystem::remove_all(logDir, ec);
    std::filesystem::remove_all(root, ec);
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all tests passed\n";
    return 0;
}
