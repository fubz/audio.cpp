#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace minitts::server {

struct ServerModelConfig {
    std::string id;
    std::filesystem::path path;
    std::string family;
    std::string task = "tts";
    std::string mode = "offline";
    bool lazy = false;
    int idle_timeout_s = -1;  // <0 inherits ServerConfig::idle_timeout_s; 0 disables idle unload
    std::optional<std::string> config_id;
    std::optional<std::string> weight_id;
    std::unordered_map<std::string, std::string> load_options;
    std::unordered_map<std::string, std::string> session_options;
};

struct ServerConfig {
    std::string host = "127.0.0.1";
    int port = 8080;
    int device = 0;
    int threads = 1;
    bool lazy_load = false;
    int idle_timeout_s = 0;    // default per-model idle unload (seconds); 0 = never unload
    int reaper_interval_s = 10;  // how often the idle reaper scans loaded models
    std::vector<ServerModelConfig> models;
};

ServerConfig load_server_config(const std::filesystem::path & path);

}  // namespace minitts::server
