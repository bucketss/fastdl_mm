#include "public_address.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

#include <algorithm>
#include <cstdlib>

bool parseIpv4(const std::string& text, std::uint32_t& address) {
    in_addr parsed{};
    if (inet_pton(AF_INET, text.c_str(), &parsed) != 1) return false;
    address = ntohl(parsed.s_addr);
    return true;
}

bool publicIpv4(std::uint32_t address) {
    const auto in = [address](std::uint32_t network, int bits) {
        const std::uint32_t mask = bits == 0 ? 0 : ~0U << (32 - bits);
        return (address & mask) == network;
    };
    return !(in(0x00000000, 8) || in(0x0A000000, 8) || in(0x64400000, 10) ||
        in(0x7F000000, 8) || in(0xA9FE0000, 16) || in(0xAC100000, 12) ||
        in(0xC0000000, 24) || in(0xC0A80000, 16) || in(0xC6120000, 15) ||
        in(0xE0000000, 3));
}

std::string formatIpv4(std::uint32_t address) {
    in_addr value{};
    value.s_addr = htonl(address);
    char buffer[INET_ADDRSTRLEN] = {};
    return inet_ntop(AF_INET, &value, buffer, sizeof(buffer)) ? buffer : "";
}

std::vector<std::string> localPublicIpv4() {
    std::vector<std::string> result;
    const auto add = [&result](std::uint32_t address) {
        if (!publicIpv4(address)) return;
        const auto text = formatIpv4(address);
        if (!text.empty() && std::find(result.begin(), result.end(), text) == result.end()) {
            result.push_back(text);
        }
    };
#ifdef _WIN32
    ULONG size = 16 * 1024;
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto* buffer = static_cast<IP_ADAPTER_ADDRESSES*>(std::malloc(size));
        if (buffer == nullptr) return result;
        const ULONG status = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, buffer, &size);
        if (status == ERROR_SUCCESS) {
            for (auto* adapter = buffer; adapter != nullptr; adapter = adapter->Next) {
                if (adapter->OperStatus != IfOperStatusUp) continue;
                for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
                     unicast = unicast->Next) {
                    const auto* address = unicast->Address.lpSockaddr;
                    if (address == nullptr || address->sa_family != AF_INET) continue;
                    add(ntohl(reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr));
                }
            }
        }
        std::free(buffer);
        if (status != ERROR_BUFFER_OVERFLOW) break;
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return result;
    for (auto* entry = list; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) continue;
        if ((entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_LOOPBACK) != 0) continue;
        add(ntohl(reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)->sin_addr.s_addr));
    }
    freeifaddrs(list);
#endif
    return result;
}
