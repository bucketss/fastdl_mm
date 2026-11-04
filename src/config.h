#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct FastdlConfig {
    bool enabled = true;
    std::string bindAddress = "0.0.0.0";
    // 0 selects the game port number, on TCP.
    std::uint16_t port = 0;
    std::filesystem::path root;
    // Relative roots and log paths resolve against this, not the working
    // directory. Set from the plugin location at load time.
    std::filesystem::path baseDir;
    // Comma separated. "*" allows everything, empty allows nothing.
    std::string serveDirs = "sprites,sound,sounds,overviews,models,maps,gfx";
    std::string serveTypes = "bsp,nav,res,wad,mdl,spr,wav,mp3,bmp,tga,txt,htm,html,bz2";
    std::uint64_t maxFileBytes = 250ULL * 1024ULL * 1024ULL;
    unsigned int threads = 1;
    unsigned int maxConnectionsPerIp = 32;
    unsigned int requestsPerMinute = 500;
    unsigned int denialsPerMinute = 36;
    unsigned int blockSeconds = 180;
    std::filesystem::path logPath = "logs/fastdl/fastdl.log";
    unsigned int logAgeDays = 7;
};
