#pragma once

#include <cstdint>
#include <string>
#include <vector>

bool parseIpv4(const std::string& text, std::uint32_t& address);
bool publicIpv4(std::uint32_t address);
std::string formatIpv4(std::uint32_t address);
std::vector<std::string> localPublicIpv4();
