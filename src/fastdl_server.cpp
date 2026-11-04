#include "fastdl_server.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <new>
#include <sstream>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// Windows only: MHD's default SO_REUSEADDR has SO_REUSEPORT semantics there, so
// any local process could take the port. 0 requests SO_EXCLUSIVEADDRUSE. On
// Linux the same request is a no-op, and losing SO_REUSEADDR would make bind
// fail during TIME_WAIT after a restart.
#ifdef _WIN32
#define FASTDL_REUSE_OPTIONS MHD_OPTION_LISTENING_ADDRESS_REUSE, 0U,
#else
#define FASTDL_REUSE_OPTIONS
#endif

namespace {
// Escaped rather than dropped: these reach a log file, where a raw escape
// sequence would run against the terminal of whoever reads it.
std::string sanitize(std::string value, std::size_t limit) {
    if (value.size() > limit) value.resize(limit);
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size());
    for (const unsigned char c : value) {
        if (c < 0x20 || c == 0x7f) {
            out += "\\x";
            out += digits[c >> 4];
            out += digits[c & 0x0f];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

std::filesystem::path resolveAgainstBase(
    const std::filesystem::path& base, const std::filesystem::path& path) {
    const std::filesystem::path joined =
        (path.is_absolute() || base.empty()) ? path : base / path;
    // Config values use forward slashes, so a plain join mixes separators.
    auto normalized = joined.lexically_normal();
    normalized.make_preferred();
    return normalized;
}

// MHD's own fd responses hardcode a 1 MiB block and malloc it per response
// (response.c), which at the connection limit is 256 MiB of transient heap in a
// 32-bit process sharing its address space with HLDS. Reading the file
// ourselves keeps that to one block per transfer, and seeking 64-bit lifts the
// 2 GiB ceiling MHD's 32-bit lseek would otherwise impose.
constexpr std::size_t kFileBlockSize = 64 * 1024;

struct Transfer {
    int fd;
    RequestState* state;
};

void closeFd(int fd) {
#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
}

ssize_t fileReader(void* cls, std::uint64_t pos, char* buf, std::size_t max) {
    auto* transfer = static_cast<Transfer*>(cls);
    const int fd = transfer->fd;
#ifdef _WIN32
    if (_lseeki64(fd, static_cast<__int64>(pos), SEEK_SET) < 0) {
        return static_cast<ssize_t>(MHD_CONTENT_READER_END_WITH_ERROR);
    }
    const int count = _read(fd, buf, static_cast<unsigned int>(max));
#else
    if (lseek(fd, static_cast<off_t>(pos), SEEK_SET) < 0) {
        return static_cast<ssize_t>(MHD_CONTENT_READER_END_WITH_ERROR);
    }
    const ssize_t count = read(fd, buf, max);
#endif
    if (count == 0) return static_cast<ssize_t>(MHD_CONTENT_READER_END_OF_STREAM);
    if (count < 0) return static_cast<ssize_t>(MHD_CONTENT_READER_END_WITH_ERROR);
    transfer->state->sent += static_cast<std::uint64_t>(count);
    return static_cast<ssize_t>(count);
}

void fileReaderFree(void* cls) {
    auto* transfer = static_cast<Transfer*>(cls);
    closeFd(transfer->fd);
    delete transfer;
}

// Own stream so the formatting flags do not leak into the rest of the line.
std::string kilobytes(std::uint64_t bytes) {
    std::ostringstream value;
    value << std::fixed << std::setprecision(1)
          << (static_cast<double>(bytes) / 1024.0) << "kB";
    return value.str();
}
} // namespace

FastdlServer::~FastdlServer() {
    stop();
}

bool FastdlServer::start(const FastdlConfig& config, std::string& error) {
    stop();
    config_ = config;
    // Nothing to start, and nothing failed: running() reports false.
    if (!config_.enabled) return true;
    if (!resolver_.configure(resolveAgainstBase(config_.baseDir, config_.root),
            config_.serveDirs, config_.serveTypes, error)) {
        return false;
    }
    // Serving is the job; the log is diagnostics. An unwritable log directory
    // warns and disables logging rather than taking the whole server down.
    logBase_ = resolveAgainstBase(config_.baseDir, config_.logPath);
    logging_ = logger_.start(logBase_, config_.logAgeDays);
    for (auto* counter : {&requests_, &served_, &notFound_, &denied_, &errors_, &bytesSent_}) {
        counter->store(0);
    }
    connections_.store(0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config_.port);
    if (config_.bindAddress.empty() || config_.bindAddress == "0.0.0.0" ||
        config_.bindAddress == "*") {
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, config_.bindAddress.c_str(), &address.sin_addr) != 1) {
        logger_.stop();
        logging_ = false;
        error = "invalid IPv4 bind address";
        return false;
    }

    daemon_ = MHD_start_daemon(
        MHD_USE_SELECT_INTERNALLY | MHD_USE_PIPE_FOR_SHUTDOWN,
        config_.port,
        nullptr, nullptr,
        &FastdlServer::handleRequest, this,
        MHD_OPTION_SOCK_ADDR, &address,
        FASTDL_REUSE_OPTIONS
        MHD_OPTION_THREAD_POOL_SIZE, config_.threads,
        MHD_OPTION_CONNECTION_LIMIT, 256U,
        MHD_OPTION_PER_IP_CONNECTION_LIMIT, config_.maxConnectionsPerIp,
        MHD_OPTION_CONNECTION_TIMEOUT, 60U,
        MHD_OPTION_CONNECTION_MEMORY_LIMIT, static_cast<size_t>(64 * 1024),
        MHD_OPTION_NOTIFY_COMPLETED, &FastdlServer::requestCompleted, this,
        MHD_OPTION_NOTIFY_CONNECTION, &FastdlServer::connectionChanged, this,
        MHD_OPTION_END);
    if (daemon_ == nullptr) {
        logger_.stop();
        logging_ = false;
        error = "libmicrohttpd could not bind or start";
        return false;
    }
    return true;
}

void FastdlServer::stop() {
    if (daemon_ != nullptr) {
        MHD_stop_daemon(daemon_);
        daemon_ = nullptr;
    }
    logger_.stop();
    logging_ = false;
    limiter_.clear();
}

int FastdlServer::handleRequest(void* cls, MHD_Connection* connection, const char* url,
    const char* method, const char*, const char*, size_t* uploadDataSize,
    void** requestContext) {
    // MHD is C: an exception escaping this frame is undefined behaviour, so
    // allocation failure has to end the connection instead.
    try {
        auto* server = static_cast<FastdlServer*>(cls);
        if (*requestContext == nullptr) {
            auto* fresh = new RequestState();
            fresh->ip = clientIp(connection);
            fresh->method = sanitize(method ? method : "", 16);
            fresh->url = sanitize(url ? url : "", 2048);
            fresh->userAgent = sanitize(header(connection, MHD_HTTP_HEADER_USER_AGENT), 200);
            *requestContext = fresh;
            return MHD_YES;
        }
        auto* state = static_cast<RequestState*>(*requestContext);
        if (*uploadDataSize != 0) {
            *uploadDataSize = 0;
            return MHD_YES;
        }
        if (state->handled) return MHD_YES;
        state->handled = true;
        return server->dispatch(connection, *state);
    } catch (...) {
        return MHD_NO;
    }
}

void FastdlServer::requestCompleted(void* cls, MHD_Connection*, void** requestContext,
    MHD_RequestTerminationCode terminationCode) {
    auto* server = static_cast<FastdlServer*>(cls);
    auto* state = static_cast<RequestState*>(*requestContext);
    if (state != nullptr) {
        // Losing a log line is survivable; letting the exception reach MHD's C
        // frame is not, and the state still has to be freed either way.
        try {
            if (server != nullptr && state->handled) {
                server->count(*state);
                server->logCompleted(*state, terminationCode);
            }
        } catch (...) {
        }
        delete state;
    }
    *requestContext = nullptr;
}

void FastdlServer::connectionChanged(void* cls, MHD_Connection*, void**,
    MHD_ConnectionNotificationCode code) {
    auto* server = static_cast<FastdlServer*>(cls);
    if (code == MHD_CONNECTION_NOTIFY_STARTED) {
        server->connections_.fetch_add(1);
    } else if (code == MHD_CONNECTION_NOTIFY_CLOSED) {
        server->connections_.fetch_sub(1);
    }
}

void FastdlServer::count(const RequestState& state) {
    requests_.fetch_add(1);
    if (state.file) bytesSent_.fetch_add(state.sent);
    if (state.status == MHD_HTTP_OK) {
        served_.fetch_add(1);
    } else if (state.status == MHD_HTTP_NOT_FOUND) {
        notFound_.fetch_add(1);
    } else if (state.status == MHD_HTTP_FORBIDDEN) {
        denied_.fetch_add(1);
    } else {
        errors_.fetch_add(1);
    }
}

ServerStats FastdlServer::stats() const {
    ServerStats result;
    result.requests = requests_.load();
    result.served = served_.load();
    result.notFound = notFound_.load();
    result.denied = denied_.load();
    result.errors = errors_.load();
    result.bytesSent = bytesSent_.load();
    result.connections = connections_.load();
    return result;
}

int FastdlServer::dispatch(MHD_Connection* connection, RequestState& state) {
    if (state.method != MHD_HTTP_METHOD_GET && state.method != MHD_HTTP_METHOD_HEAD) {
        return deny(connection, state, "invalid method", true);
    }
    if (limiter_.blocked(state.ip)) {
        return deny(connection, state, "blocked", false);
    }
    // Steam downloader only, not configurable. Before the rate limiter so
    // traffic that can never be served does not consume the request window.
    if (!steamClient(state.userAgent)) {
        return deny(connection, state, "non-steam client denied", true);
    }
    if (limiter_.requestExceeded(state.ip, config_.requestsPerMinute)) {
        return deny(connection, state, "rate limit", false);
    }

    const auto file = resolver_.resolve(state.url.c_str(), config_.maxFileBytes);
    switch (file.status) {
        case ResolveStatus::Ok:
            break;
        // A file the server does not have is ordinary traffic: no denial
        // strike, or a normal join could block a legitimate client.
        case ResolveStatus::NotFound:
        case ResolveStatus::NotAFile:
            return respondText(
                connection, state, MHD_HTTP_NOT_FOUND, "404 Not Found\n", "not found");
        case ResolveStatus::TooLarge:
            return respondText(
                connection, state, MHD_HTTP_FORBIDDEN, "403 Forbidden\n", "file too large");
        case ResolveStatus::IoError:
            return respondText(connection, state, MHD_HTTP_INTERNAL_SERVER_ERROR,
                "500 Internal Server Error\n", "file error");
        case ResolveStatus::OutsideRoot:
            return deny(connection, state, "outside root", true);
        case ResolveStatus::DirectoryDenied:
            return deny(connection, state, "directory not served", true);
        case ResolveStatus::ExtensionDenied:
            return deny(connection, state, "invalid extension", true);
        case ResolveStatus::InvalidPath:
        default:
            return deny(connection, state, "invalid path", true);
    }

    const int queued = serveFile(connection, state, file);
    if (queued != MHD_YES) {
        return respondText(connection, state, MHD_HTTP_INTERNAL_SERVER_ERROR,
            "500 Internal Server Error\n", "open failed");
    }
    state.file = true;
    state.status = MHD_HTTP_OK;
    state.size = file.size;
    state.reason.clear();
    return MHD_YES;
}

int FastdlServer::deny(MHD_Connection* connection, RequestState& state,
    const char* reason, bool strike) {
    if (strike) {
        limiter_.recordDenial(state.ip, config_.denialsPerMinute, config_.blockSeconds);
    }
    return respondText(connection, state, MHD_HTTP_FORBIDDEN, "403 Forbidden\n", reason);
}

int FastdlServer::respondText(MHD_Connection* connection, RequestState& state,
    unsigned int status, const char* body, const char* reason) {
    state.status = status;
    state.size = std::strlen(body);
    state.reason = reason;

    auto* response = MHD_create_response_from_buffer(
        std::strlen(body), const_cast<char*>(body), MHD_RESPMEM_PERSISTENT);
    if (response == nullptr) return MHD_NO;
    MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, "text/plain; charset=utf-8");
    MHD_add_response_header(response, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
    const int result = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return result;
}

int FastdlServer::serveFile(
    MHD_Connection* connection, RequestState& state, const ResolvedFile& file) {
#ifdef _WIN32
    const int fd = _wopen(file.path.c_str(), _O_RDONLY | _O_BINARY | _O_NOINHERIT);
#else
    const int fd = open(file.path.c_str(), O_RDONLY | O_CLOEXEC);
#endif
    if (fd < 0) return MHD_NO;

    // Once the response exists it owns the descriptor: fileReaderFree closes it
    // when the transfer finishes or the connection dies.
    auto* transfer = new (std::nothrow) Transfer{fd, &state};
    if (transfer == nullptr) {
        closeFd(fd);
        return MHD_NO;
    }
    MHD_Response* response = MHD_create_response_from_callback(
        file.size, kFileBlockSize, &fileReader, transfer, &fileReaderFree);
    if (response == nullptr) {
        closeFd(fd);
        delete transfer;
        return MHD_NO;
    }
    MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, contentType(file.extension));
    MHD_add_response_header(response, MHD_HTTP_HEADER_CACHE_CONTROL, "public, max-age=86400");
    const int result = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return result;
}

std::string FastdlServer::clientIp(MHD_Connection* connection) {
    const auto* info = MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
    if (info == nullptr || info->client_addr == nullptr) return "unknown";
    // The daemon binds a sockaddr_in without MHD_USE_IPv6, so v4 is all that
    // can arrive here.
    char buffer[INET_ADDRSTRLEN] = {};
    const auto* address = info->client_addr;
    if (address->sa_family == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
        if (inet_ntop(AF_INET, &ipv4->sin_addr, buffer, sizeof(buffer))) return buffer;
    }
    return "unknown";
}

std::string FastdlServer::header(MHD_Connection* connection, const char* name) {
    const char* value = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, name);
    return value ? value : "";
}

bool FastdlServer::steamClient(const std::string& userAgent) {
    if (userAgent.rfind("Valve/Steam HTTP Client", 0) == 0) return true;
    if (userAgent.rfind("Steam/", 0) == 0) return true;
    return userAgent.find("Steam") != std::string::npos &&
        userAgent.find("HTTP") != std::string::npos &&
        userAgent.find("Client") != std::string::npos;
}

const char* FastdlServer::contentType(const std::string& extension) {
    if (extension == ".html" || extension == ".htm") return "text/html";
    if (extension == ".txt" || extension == ".res") return "text/plain";
    if (extension == ".wav") return "audio/wav";
    if (extension == ".mp3") return "audio/mpeg";
    if (extension == ".bmp") return "image/bmp";
    if (extension == ".tga") return "image/x-tga";
    if (extension == ".bz2") return "application/x-bzip2";
    if (extension == ".gz") return "application/gzip";
    return "application/octet-stream";
}

const char* FastdlServer::terminationName(MHD_RequestTerminationCode code) {
    switch (code) {
        case MHD_REQUEST_TERMINATED_COMPLETED_OK: return "ok";
        case MHD_REQUEST_TERMINATED_WITH_ERROR: return "error";
        case MHD_REQUEST_TERMINATED_TIMEOUT_REACHED: return "timeout";
        case MHD_REQUEST_TERMINATED_DAEMON_SHUTDOWN: return "shutdown";
        case MHD_REQUEST_TERMINATED_READ_ERROR: return "read error";
        case MHD_REQUEST_TERMINATED_CLIENT_ABORT: return "client abort";
        default: return "unknown";
    }
}

void FastdlServer::logCompleted(
    const RequestState& state, MHD_RequestTerminationCode code) {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream line;
    line << '[' << std::put_time(&local, "%Y-%m-%dT%H:%M:%S") << "] "
         << state.method << ' ' << state.url << " -> " << state.status << ' ';
    if (state.file && state.method == MHD_HTTP_METHOD_GET && state.sent != state.size) {
        line << kilobytes(state.sent) << '/';
    }
    line << kilobytes(state.size) << ' ' << terminationName(code)
         << " from " << state.ip << " [" << state.userAgent << ']';
    if (!state.reason.empty()) line << ' ' << state.reason;
    logger_.write(line.str());
}
