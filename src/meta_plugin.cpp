#ifdef _WIN32
#include <winsock2.h>
#endif

#include <extdll.h>
#include <meta_api.h>

// Valve's HLSDK pulls minmax.h in through extdll.h, which defines min, max and
// clamp as macros and breaks the std:: versions at their call sites. ReGameDLL's
// headers keep them in mathlib.h, which extdll.h does not include, so this is a
// no-op there.
#undef min
#undef max
#undef clamp

#include "config.h"
#include "config_parser.h"
#include "fastdl_server.h"
#include "public_address.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#ifndef FASTDL_MM_VERSION
#define FASTDL_MM_VERSION "dev"
#endif

enginefuncs_t g_engfuncs;
globalvars_t* gpGlobals = nullptr;
meta_globals_t* gpMetaGlobals = nullptr;
gamedll_funcs_t* gpGamedllFuncs = nullptr;
mutil_funcs_t* gpMetaUtilFuncs = nullptr;

namespace {
FastdlServer g_server;
bool g_registered = false;
bool g_startPending = false;
std::filesystem::path g_baseDir;
std::filesystem::path g_configPath;
std::string g_checkedUrl;
float g_urlCheckAt = 0.0f;
bool g_urlCheckChangedOnly = false;
std::string g_autoUrl;

cvar_t cvarEnabled = {"fastdl_enabled", "1", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarBind = {"fastdl_bind", "0.0.0.0", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarPort = {"fastdl_port", "0", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarRoot = {"fastdl_root", "cstrike", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarServeDirs = {"fastdl_serve_dirs",
    "sprites,sound,sounds,overviews,models,maps,gfx", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarServeTypes = {"fastdl_serve_types",
    "bsp,nav,res,wad,mdl,spr,wav,mp3,bmp,tga,txt,htm,html,bz2",
    FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarMaxFileMb = {"fastdl_max_file_mb", "250", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarThreads = {"fastdl_threads", "1", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarConnections = {"fastdl_max_connections_ip", "32", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarRequests = {"fastdl_requests_minute", "500", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarDenials = {"fastdl_denials_minute", "36", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarBlockSeconds = {"fastdl_block_seconds", "180", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarLog = {"fastdl_log", "logs/fastdl/fastdl.log", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarLogAge = {"fastdl_log_age", "7", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarAutoUrl = {"fastdl_autourl", "0", FCVAR_SERVER, 0.0f, nullptr};

// Registration order, and the order a generated config is written in. The help
// text is only used for the comment block in a generated config.
struct Setting {
    cvar_t* cvar;
    const char* help;
};

const Setting g_settings[] = {
    {&cvarEnabled,      "toggle fastdl hosting"},
    {&cvarBind,         "listen address"},
    {&cvarPort,         "listen port (TCP); 0 uses the game port number"},
    {&cvarRoot,         "game mod directory"},
    {&cvarServeDirs,    "subdirectories served, comma separated"},
    {&cvarServeTypes,   "extensions served, comma separated"},
    {&cvarMaxFileMb,    "refuse files larger than this"},
    {&cvarThreads,      "HTTP worker threads; raise only for slow disks"},
    {&cvarConnections,  "concurrent connections per IP"},
    {&cvarRequests,     "requests per minute per IP"},
    {&cvarDenials,      "denials per minute before a temporary block"},
    {&cvarBlockSeconds, "how long a block lasts, in seconds"},
    {&cvarLog,          "log base path; one file per day"},
    {&cvarLogAge,       "days of logs to keep, 0 keeps all"},
    {&cvarAutoUrl,      "set an empty sv_downloadurl from this machine's public IPv4"}
};

unsigned int boundedUnsigned(const char* name, unsigned int minimum, unsigned int maximum) {
    const float value = g_engfuncs.pfnCVarGetFloat(name);
    if (!std::isfinite(value)) return minimum;
    return static_cast<unsigned int>(std::clamp(value,
        static_cast<float>(minimum), static_cast<float>(maximum)));
}

// The port HLDS serves the game on. Managed hosts allocate and firewall ports
// by number regardless of protocol, so TCP on the UDP game port number is the
// likeliest to be reachable without a second allocation. ReHLDS sets hostport
// from -port at startup (net_ws.cpp); "port" is the fallback.
unsigned int gamePort() {
    for (const char* name : {"hostport", "port"}) {
        const float value = g_engfuncs.pfnCVarGetFloat(name);
        if (value >= 1.0f && value <= 65535.0f) return static_cast<unsigned int>(value);
    }
    return 27015;
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// Relative roots and log paths resolve against the HLDS directory, not the
// working directory a service wrapper is free to change. The plugin sits at
// <base>/<mod>/addons/..., so walk up to the mod directory.
std::filesystem::path resolveBaseDir() {
    std::error_code ec;
    char gameDir[512] = {};
    g_engfuncs.pfnGetGameDir(gameDir);
    const auto modName =
        lowerAscii(std::filesystem::u8path(gameDir).filename().u8string());

    const char* pluginPath =
        gpMetaUtilFuncs != nullptr ? gpMetaUtilFuncs->pfnGetPluginPath(PLID) : nullptr;
    if (pluginPath != nullptr && !modName.empty()) {
        const auto absolute =
            std::filesystem::absolute(std::filesystem::u8path(pluginPath), ec);
        if (!ec) {
            for (auto dir = absolute.parent_path();; dir = dir.parent_path()) {
                if (lowerAscii(dir.filename().u8string()) == modName) {
                    // canonical() also settles drive letter case.
                    auto base = std::filesystem::canonical(dir.parent_path(), ec);
                    return ec ? dir.parent_path() : base;
                }
                if (dir == dir.parent_path()) break;
            }
        }
    }
    const auto working = std::filesystem::current_path(ec);
    if (ec) return {};
    auto canonical = std::filesystem::canonical(working, ec);
    return ec ? working : canonical;
}

FastdlConfig readConfig() {
    FastdlConfig config;
    config.enabled = g_engfuncs.pfnCVarGetFloat("fastdl_enabled") != 0.0f;
    config.bindAddress = g_engfuncs.pfnCVarGetString("fastdl_bind");
    const unsigned int port = boundedUnsigned("fastdl_port", 0, 65535);
    config.port = static_cast<std::uint16_t>(port == 0 ? gamePort() : port);
    config.root = std::filesystem::u8path(g_engfuncs.pfnCVarGetString("fastdl_root"));
    config.baseDir = g_baseDir;
    config.serveDirs = g_engfuncs.pfnCVarGetString("fastdl_serve_dirs");
    config.serveTypes = g_engfuncs.pfnCVarGetString("fastdl_serve_types");
    config.maxFileBytes =
        static_cast<std::uint64_t>(boundedUnsigned("fastdl_max_file_mb", 1, 2048)) *
        1024ULL * 1024ULL;
    // Workers exist to overlap blocking file reads, not to add CPU: connections
    // are pinned per worker, so one uncached read stalls only its own share.
    // Cached reads never block, so more than a few is pure scheduler contention
    // against the single game thread.
    config.threads = boundedUnsigned("fastdl_threads", 1, 4);
    config.maxConnectionsPerIp =
        boundedUnsigned("fastdl_max_connections_ip", 1, 256);
    config.requestsPerMinute = boundedUnsigned("fastdl_requests_minute", 1, 10000);
    config.denialsPerMinute = boundedUnsigned("fastdl_denials_minute", 1, 1000);
    config.blockSeconds = boundedUnsigned("fastdl_block_seconds", 1, 86400);
    config.logPath = std::filesystem::u8path(g_engfuncs.pfnCVarGetString("fastdl_log"));
    config.logAgeDays = boundedUnsigned("fastdl_log_age", 0, 3650);
    return config;
}

void print(const std::string& text) {
    g_engfuncs.pfnServerPrint(("[FastDL] " + text + "\n").c_str());
}

// An sv_downloadurl nothing is serving costs each client a connect timeout per
// file before it falls back, which looks like a broken download. Whether the
// URL refers to this machine cannot be checked -- the engine only knows its
// local address -- so warn rather than edit the cvar on a guess.
void warnIfDownloadUrlSet() {
    const std::string url = g_engfuncs.pfnCVarGetString("sv_downloadurl");
    if (url.empty()) return;
    print("not serving, but sv_downloadurl is set (" + url +
        ") - clients will stall on it before falling back");
}

std::vector<std::string> downloadUrlProblems(const std::string& url, unsigned int port) {
    std::vector<std::string> problems;
    if (url.empty()) {
        problems.push_back("sv_downloadurl is empty - clients will use slow download");
        return problems;
    }
    const auto lower = lowerAscii(url);
    unsigned int defaultPort = 80;
    std::size_t authorityStart = 0;
    if (lower.rfind("http://", 0) == 0) {
        authorityStart = 7;
    } else if (lower.rfind("https://", 0) == 0) {
        authorityStart = 8;
        defaultPort = 443;
        problems.push_back("sv_downloadurl uses https, this plugin serves plain http");
    } else {
        problems.push_back("sv_downloadurl does not start with http://");
        return problems;
    }
    const auto slash = url.find('/', authorityStart);
    const auto authority = url.substr(authorityStart,
        slash == std::string::npos ? std::string::npos : slash - authorityStart);
    const auto path = slash == std::string::npos ? std::string() : url.substr(slash);
    unsigned int urlPort = defaultPort;
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos) {
        const auto digits = authority.substr(colon + 1);
        if (digits.empty() || digits.size() > 5 ||
            !std::all_of(digits.begin(), digits.end(),
                [](unsigned char c) { return std::isdigit(c) != 0; })) {
            problems.push_back("sv_downloadurl has an invalid port");
            return problems;
        }
        urlPort = static_cast<unsigned int>(std::stoul(digits));
    }
    if (urlPort != port) {
        problems.push_back("sv_downloadurl points at port " + std::to_string(urlPort) +
            ", fastdl listens on " + std::to_string(port));
    }
    if (path.find_first_not_of('/') != std::string::npos) {
        problems.push_back("sv_downloadurl has a path (" + path +
            "), fastdl serves fastdl_root at /");
    }
    return problems;
}

void checkDownloadUrl() {
    const std::string url = g_engfuncs.pfnCVarGetString("sv_downloadurl");
    g_checkedUrl = url;
    const auto problems = downloadUrlProblems(url, g_server.config().port);
    if (problems.empty()) {
        print("sv_downloadurl " + url + " matches the fastdl port");
        return;
    }
    for (const auto& problem : problems) print("WARNING: " + problem);
}

bool explicitAddress(const std::string& text, const char* name,
    std::string& address, std::string& why) {
    std::uint32_t value = 0;
    if (!parseIpv4(text, value) || value == 0) return false;
    if (publicIpv4(value)) {
        address = formatIpv4(value);
    } else {
        why = std::string(name) + " is " + text + ", a private address";
    }
    return true;
}

bool autoUrlAddress(std::string& address, std::string& why) {
    if (explicitAddress(g_engfuncs.pfnCVarGetString("ip"), "ip", address, why) ||
        explicitAddress(g_server.config().bindAddress, "fastdl_bind", address, why)) {
        return !address.empty();
    }
    const auto found = localPublicIpv4();
    if (found.size() == 1) {
        address = found.front();
        return true;
    }
    if (found.empty()) {
        why = "no public IPv4 address on this machine";
    } else {
        why = "several public IPv4 addresses (";
        for (std::size_t i = 0; i < found.size(); ++i) {
            why += (i ? ", " : "") + found[i];
        }
        why += "), set ip or sv_downloadurl";
    }
    return false;
}

void applyAutoUrl(bool report) {
    if (g_engfuncs.pfnCVarGetFloat("fastdl_autourl") == 0.0f) return;
    const std::string current = g_engfuncs.pfnCVarGetString("sv_downloadurl");
    if (!current.empty() && current != g_autoUrl) return;
    std::string address;
    std::string why;
    if (!autoUrlAddress(address, why)) {
        if (report) print("fastdl_autourl: " + why);
        return;
    }
    const auto url = "http://" + address + ":" + std::to_string(g_server.config().port) + "/";
    if (url == current) return;
    g_engfuncs.pfnCVarSetString("sv_downloadurl", url.c_str());
    g_autoUrl = url;
    print("fastdl_autourl set sv_downloadurl to " + url);
}

void clearAutoUrl() {
    if (!g_autoUrl.empty() && g_autoUrl == g_engfuncs.pfnCVarGetString("sv_downloadurl")) {
        g_engfuncs.pfnCVarSetString("sv_downloadurl", "");
        print("fastdl_autourl cleared sv_downloadurl");
    }
    g_autoUrl.clear();
}

void printLogState() {
    if (!g_server.logging()) {
        print("NOT logging: could not open " + g_server.logPath().u8string() +
            " (check the directory exists and the server account can write it)");
        return;
    }
    print("log " + g_server.logPath().u8string() + ", keeping " +
        (g_server.config().logAgeDays == 0
            ? std::string("all days")
            : std::to_string(g_server.config().logAgeDays) + " days"));
}

void startServer() {
    const auto config = readConfig();
    if (!config.enabled) {
        g_server.stop();
        print("disabled");
        clearAutoUrl();
        warnIfDownloadUrlSet();
        return;
    }
    std::string error;
    if (!g_server.start(config, error)) {
        print("start failed: " + error);
        clearAutoUrl();
        warnIfDownloadUrlSet();
        return;
    }
    const bool followsGamePort = g_engfuncs.pfnCVarGetFloat("fastdl_port") == 0.0f;
    print("listening on " + config.bindAddress + ":" + std::to_string(config.port) +
        (followsGamePort ? " (game port)" : "") +
        ", threads=" + std::to_string(config.threads) +
        ", root=" + g_server.root().u8string());
    print("serving " + config.serveDirs);
    printLogState();
    g_urlCheckAt = gpGlobals->time + 5.0f;
}

void applyConfigFile();

void commandRestart() {
    print("restarting");
    applyConfigFile();
    startServer();
}

std::string megabytes(std::uint64_t bytes) {
    std::ostringstream value;
    value << std::fixed << std::setprecision(1)
          << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MB";
    return value.str();
}

void commandBlocked() {
    const auto blocked = g_server.blocked();
    if (blocked.empty()) {
        print("no blocked IPs");
        return;
    }
    for (const auto& block : blocked) {
        print(block.ip + " blocked for " + std::to_string(block.secondsLeft) + "s");
    }
}

void commandStatus() {
    if (!g_server.running()) {
        print("stopped");
        return;
    }
    const auto& config = g_server.config();
    print("running on " + config.bindAddress + ":" + std::to_string(config.port) +
        ", root=" + g_server.root().u8string());
    print("dirs=" + config.serveDirs);
    print("types=" + config.serveTypes);
    print("threads=" + std::to_string(config.threads) +
        ", conn/ip=" + std::to_string(config.maxConnectionsPerIp) +
        ", req/min=" + std::to_string(config.requestsPerMinute) +
        ", denials/min=" + std::to_string(config.denialsPerMinute) +
        ", block=" + std::to_string(config.blockSeconds) + "s");
    const auto stats = g_server.stats();
    print("connections=" + std::to_string(stats.connections) +
        ", requests=" + std::to_string(stats.requests) +
        ", served=" + std::to_string(stats.served) +
        ", 404=" + std::to_string(stats.notFound) +
        ", denied=" + std::to_string(stats.denied) +
        ", errors=" + std::to_string(stats.errors) +
        ", sent=" + megabytes(stats.bytesSent) +
        ", blocked=" + std::to_string(g_server.blocked().size()));
    printLogState();
    checkDownloadUrl();
}

void commandUnblock() {
    if (g_engfuncs.pfnCmd_Argc() != 2) {
        print("usage: fastdl_unblock <ip>");
        return;
    }
    const std::string ip = g_engfuncs.pfnCmd_Argv(1);
    print(g_server.unblock(ip) ? "unblocked " + ip : "no state for " + ip);
}

// Values are read back from the cvars, so a generated file always matches what
// the plugin actually registered. Call only after registration.
bool writeDefaultConfig(const std::filesystem::path& path, std::string& error) {
    std::size_t column = 0;
    for (const auto& setting : g_settings) {
        column = std::max(column, std::strlen(setting.cvar->name));
    }
    column += 4;
    const int width = static_cast<int>(column);

    std::ofstream out(path);
    if (!out) {
        error = "could not create " + path.generic_u8string();
        return false;
    }
    for (const auto& setting : g_settings) {
        out << std::left << std::setw(width) << setting.cvar->name
            << '"' << setting.cvar->string << "\"\n";
    }
    out << "\n// remember to set sv_downloadurl to \"http://your-ip:port/\"\n"
           "\n// CVARs:\n";
    for (const auto& setting : g_settings) {
        out << "// " << std::left << std::setw(width) << setting.cvar->name
            << setting.help << " (default \"" << setting.cvar->string << "\")\n";
    }
    out.flush();
    if (!out) {
        error = "could not write " + path.generic_u8string();
        return false;
    }
    return true;
}

// Bad lines are reported and skipped, not fatal: failing the whole file would
// silently revert every setting -- including the port -- to its compiled-in
// default whenever the config is newer than the binary beside it.
bool loadConfigFile(const std::filesystem::path& path, unsigned int& loaded,
    unsigned int& skipped, std::string& error) {
    static const std::unordered_set<std::string> allowedNames = {
        "fastdl_enabled", "fastdl_bind", "fastdl_port", "fastdl_root",
        "fastdl_serve_dirs", "fastdl_serve_types",
        "fastdl_max_file_mb", "fastdl_threads",
        "fastdl_max_connections_ip", "fastdl_requests_minute",
        "fastdl_denials_minute", "fastdl_block_seconds", "fastdl_log",
        "fastdl_log_age", "fastdl_autourl"
    };

    std::ifstream configFile(path);
    if (!configFile) {
        error = "could not open " + path.generic_u8string();
        return false;
    }

    loaded = 0;
    skipped = 0;
    std::string line;
    unsigned int lineNumber = 0;
    while (std::getline(configFile, line)) {
        ++lineNumber;
        std::string name;
        std::string value;
        if (!config::parseAssignment(line, name, value)) {
            if (!config::trim(config::stripComment(line)).empty()) {
                print("config: skipping invalid assignment on line " +
                    std::to_string(lineNumber));
                ++skipped;
            }
            continue;
        }
        if (allowedNames.find(name) == allowedNames.end()) {
            print("config: skipping unknown setting '" + name + "' on line " +
                std::to_string(lineNumber));
            ++skipped;
            continue;
        }
        g_engfuncs.pfnCVarSetString(name.c_str(), value.c_str());
        ++loaded;
    }
    if (configFile.bad()) {
        error = "failed while reading " + path.generic_u8string();
        return false;
    }
    return true;
}

void applyConfigFile() {
    print("loading config " + g_configPath.generic_u8string());
    unsigned int loaded = 0;
    unsigned int skipped = 0;
    std::string configError;
    if (!loadConfigFile(g_configPath, loaded, skipped, configError)) {
        print("config error: " + configError);
        return;
    }
    print("config applied: " + std::to_string(loaded) + " settings" +
        (skipped != 0 ? ", " + std::to_string(skipped) + " skipped" : "") +
        ", port=" + std::string(g_engfuncs.pfnCVarGetString("fastdl_port")) +
        ", root=" + g_engfuncs.pfnCVarGetString("fastdl_root"));
}

void registerPlugin() {
    if (g_registered) return;
    for (const auto& setting : g_settings) g_engfuncs.pfnCVarRegister(setting.cvar);
    g_engfuncs.pfnAddServerCommand("fastdl_restart", &commandRestart);
    g_engfuncs.pfnAddServerCommand("fastdl_status", &commandStatus);
    g_engfuncs.pfnAddServerCommand("fastdl_unblock", &commandUnblock);
    g_engfuncs.pfnAddServerCommand("fastdl_blocked", &commandBlocked);
    g_baseDir = resolveBaseDir();
    print("base directory " + g_baseDir.u8string());
    const char* pluginPath =
        gpMetaUtilFuncs != nullptr ? gpMetaUtilFuncs->pfnGetPluginPath(PLID) : nullptr;
    g_configPath =
        std::filesystem::u8path(pluginPath ? pluginPath : "addons/fastdl/fastdl_mm.dll")
            .parent_path() / "fastdl_mm.cfg";
    // Only when genuinely absent: a file we cannot stat is never overwritten.
    std::error_code existsError;
    if (!std::filesystem::exists(g_configPath, existsError) && !existsError) {
        std::string writeError;
        if (writeDefaultConfig(g_configPath, writeError)) {
            print("no config found, wrote defaults");
        } else {
            print("no config found and " + writeError);
        }
    }
    applyConfigFile();
    g_registered = true;
    g_startPending = true;
}

void gameInit() {
    registerPlugin();
    gpMetaGlobals->mres = MRES_IGNORED;
}

void startFrame() {
    if (g_startPending) {
        g_startPending = false;
        startServer();
    }
    if (g_urlCheckAt > 0.0f && gpGlobals->time >= g_urlCheckAt) {
        g_urlCheckAt = 0.0f;
        if (g_server.running()) applyAutoUrl(!g_urlCheckChangedOnly);
        if (g_server.running() &&
            (!g_urlCheckChangedOnly ||
                g_engfuncs.pfnCVarGetString("sv_downloadurl") != g_checkedUrl)) {
            checkDownloadUrl();
        }
        g_urlCheckChangedOnly = false;
    }
    gpMetaGlobals->mres = MRES_IGNORED;
}

void serverActivate(edict_t*, int, int) {
    if (g_server.running()) {
        if (g_urlCheckAt == 0.0f) g_urlCheckChangedOnly = true;
        g_urlCheckAt = gpGlobals->time + 5.0f;
    }
    gpMetaGlobals->mres = MRES_IGNORED;
}

DLL_FUNCTIONS functions = {};

int getEntityApi2(DLL_FUNCTIONS* table, int* version) {
    if (table == nullptr || version == nullptr) return FALSE;
    if (*version != INTERFACE_VERSION) {
        *version = INTERFACE_VERSION;
        return FALSE;
    }
    functions.pfnGameInit = &gameInit;
    functions.pfnStartFrame = &startFrame;
    functions.pfnServerActivate = &serverActivate;
    std::memcpy(table, &functions, sizeof(functions));
    return TRUE;
}
} // namespace

plugin_info_t Plugin_info = {
    META_INTERFACE_VERSION,
    "FastDL",
    FASTDL_MM_VERSION,
    __DATE__,
    "servor",
    "",
    "FASTDL",
    PT_STARTUP,
    // Never unloadable. Cvar_RegisterVariable links our cvar_t structs into the
    // engine's list by pointer and there is no way to take them back out, and
    // AddServerCommand keeps handlers pointing into this DLL. Unmapping it --
    // by "meta unload" or by Metamod's own reload when the file on disk is
    // newer -- would leave the engine reading and calling freed memory.
    PT_NEVER
};

C_DLLEXPORT void WINAPI GiveFnptrsToDll(
    enginefuncs_t* engineFunctions, globalvars_t* globals) {
    std::memcpy(&g_engfuncs, engineFunctions, sizeof(g_engfuncs));
    gpGlobals = globals;
}

C_DLLEXPORT int Meta_Query(
    char*, plugin_info_t** pluginInfo, mutil_funcs_t* utilityFunctions) {
    if (pluginInfo == nullptr || utilityFunctions == nullptr) return FALSE;
    *pluginInfo = &Plugin_info;
    gpMetaUtilFuncs = utilityFunctions;
    return TRUE;
}

META_FUNCTIONS metaFunctions = {
    nullptr,
    nullptr,
    &getEntityApi2,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr
};

C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME, META_FUNCTIONS* functionTable,
    meta_globals_t* globals, gamedll_funcs_t* gameFunctions) {
    if (functionTable == nullptr || globals == nullptr) return FALSE;
    gpMetaGlobals = globals;
    gpGamedllFuncs = gameFunctions;
    std::memcpy(functionTable, &metaFunctions, sizeof(metaFunctions));
    return TRUE;
}

C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME, PL_UNLOAD_REASON) {
    g_server.stop();
    return TRUE;
}
