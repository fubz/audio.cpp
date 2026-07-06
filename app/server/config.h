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
    int idle_exit_after_s = 0;   // >0: once all models have unloaded and the server has been
                                 // idle this long, exit(0) to fully release the CUDA context (the
                                 // GPU only drops to its low-power state on process exit — ggml
                                 // holds the primary context process-wide). Requires a container
                                 // restart policy + idle_timeout_s > 0; set it above idle_timeout_s.
    int max_resident_models = 0;  // >0: cap how many models stay loaded (in VRAM) at once. Before
                                  // loading a model the least-recently-used resident model(s) are
                                  // unloaded to honor the cap; a model with an in-flight request is
                                  // never evicted. 0 = unlimited (default). Set to 1 on a small or
                                  // shared GPU where two large models can't co-reside — turns a
                                  // cudaMalloc OOM-on-load into a clean swap.
    std::vector<ServerModelConfig> models;
};

ServerConfig load_server_config(const std::filesystem::path & path);

}  // namespace minitts::server
