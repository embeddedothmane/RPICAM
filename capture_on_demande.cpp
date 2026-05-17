#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <ctime>
#include <cstdio>

#include <mosquitto.h>

namespace fs = std::filesystem;

static std::atomic<bool> g_stop{false};

void handle_signal(int) {
    g_stop = true;
}

struct Config {
    // Chemins identiques au fichier capture_last_image.cpp
    fs::path output_dir = "/var/lib/rpicam-http";
    std::string output_name = "LAST_CAPTURE.jpg";
    fs::path baseDir = "/home/pi/captures_cam3";
    fs::path pendingDir = baseDir / "pending";
    fs::path uploadedDir = baseDir / "uploaded";
    fs::path failedDir = baseDir / "failed";
    int intervalSec = 5;

    // Paramètres capture identiques
    int interval_ms = 5000;
    int duration = 20;
    int width = 2304;
    int height = 1296;
    int timeout_ms = 700;
    std::string autofocus_mode = "auto";
    std::string autofocus_range = "normal";
    std::string lens_position;
    int shutterUs = 0;
    int gain = 0;

    // Paramètres MQTT
    std::string mqtt_host = "127.0.0.1";
    int mqtt_port = 1883;
    std::string mqtt_topic = "rpicam/capture/request";
    std::string mqtt_client_id = "capture_on_demande";
    std::string mqtt_username;
    std::string mqtt_password;
    int mqtt_qos = 1;
    int mqtt_keepalive = 60;
    bool mqtt_clean_session = true;
};

std::string shell_escape(const std::string &s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

static std::string nowTimestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
    localtime_r(&t, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d_%H%M%S")
        << '_' << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

void print_usage(const char *prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "Options capture (identiques a capture_last_image):\n"
        << "  --output-dir PATH        Dossier de sortie (defaut: /var/lib/rpicam-http)\n"
        << "  --output-name NAME       Nom du fichier final (defaut: LAST_CAPTURE.jpg)\n"
        << "  --interval-ms N          Intervalle entre captures (defaut: 5000)\n"
        << "  --durantion N            dure en minutees (defaut: 20)\n"
        << "  --width N                Largeur image (defaut: 2304)\n"
        << "  --height N               Hauteur image (defaut: 1296)\n"
        << "  --timeout-ms N           Temps preview/AF avant capture (defaut: 700)\n"
        << "  --autofocus-mode MODE    auto | continuous | manual (defaut: auto)\n"
        << "  --autofocus-range RANGE  normal | macro | full (defaut: normal)\n"
        << "  --lens-position VALUE    Position de lentille en dioptres si mode manual\n"
        << "\n"
        << "Options MQTT:\n"
        << "  --mqtt-host HOST         Serveur MQTT / mosquitto (defaut: 127.0.0.1)\n"
        << "  --mqtt-port PORT         Port MQTT (defaut: 1883)\n"
        << "  --mqtt-topic TOPIC       Topic d'abonnement (defaut: rpicam/capture/request)\n"
        << "  --mqtt-client-id ID      Client ID MQTT (defaut: capture_on_demande)\n"
        << "  --mqtt-username USER     Username MQTT optionnel\n"
        << "  --mqtt-password PASS     Password MQTT optionnel\n"
        << "  --mqtt-qos N             QoS MQTT 0..2 (defaut: 1)\n"
        << "  --mqtt-keepalive N       Keepalive en secondes (defaut: 60)\n";
}

bool parse_args(int argc, char **argv, Config &cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const std::string &opt) -> const char * {
            if (i + 1 >= argc) {
                std::cerr << "Option sans valeur: " << opt << "\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--output-dir") {
            if (const char *v = need_value(a)) cfg.output_dir = v; else return false;
        } else if (a == "--output-name") {
            if (const char *v = need_value(a)) cfg.output_name = v; else return false;
        } else if (a == "--interval-ms") {
            if (const char *v = need_value(a)) cfg.interval_ms = std::stoi(v); else return false;
        } else if (a == "--duration") {
            if (const char *v = need_value(a)) cfg.duration = std::stoi(v); else return false;
        } else if (a == "--width") {
            if (const char *v = need_value(a)) cfg.width = std::stoi(v); else return false;
        } else if (a == "--height") {
            if (const char *v = need_value(a)) cfg.height = std::stoi(v); else return false;
        } else if (a == "--timeout-ms") {
            if (const char *v = need_value(a)) cfg.timeout_ms = std::stoi(v); else return false;
        } else if (a == "--autofocus-mode") {
            if (const char *v = need_value(a)) cfg.autofocus_mode = v; else return false;
        } else if (a == "--autofocus-range") {
            if (const char *v = need_value(a)) cfg.autofocus_range = v; else return false;
        } else if (a == "--lens-position") {
            if (const char *v = need_value(a)) cfg.lens_position = v; else return false;
        } else if (a == "--mqtt-host") {
            if (const char *v = need_value(a)) cfg.mqtt_host = v; else return false;
        } else if (a == "--mqtt-port") {
            if (const char *v = need_value(a)) cfg.mqtt_port = std::stoi(v); else return false;
        } else if (a == "--mqtt-topic") {
            if (const char *v = need_value(a)) cfg.mqtt_topic = v; else return false;
        } else if (a == "--mqtt-client-id") {
            if (const char *v = need_value(a)) cfg.mqtt_client_id = v; else return false;
        } else if (a == "--mqtt-username") {
            if (const char *v = need_value(a)) cfg.mqtt_username = v; else return false;
        } else if (a == "--mqtt-password") {
            if (const char *v = need_value(a)) cfg.mqtt_password = v; else return false;
        } else if (a == "--mqtt-qos") {
            if (const char *v = need_value(a)) cfg.mqtt_qos = std::stoi(v); else return false;
        } else if (a == "--mqtt-keepalive") {
            if (const char *v = need_value(a)) cfg.mqtt_keepalive = std::stoi(v); else return false;
        } else if (a == "--help" || a == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "Option inconnue: " << a << "\n";
            return false;
        }
    }

    // Conserver les mêmes chemins de base que capture_last_image
    if (cfg.pendingDir.empty()) cfg.pendingDir = cfg.baseDir / "pending";
    if (cfg.uploadedDir.empty()) cfg.uploadedDir = cfg.baseDir / "uploaded";
    if (cfg.failedDir.empty()) cfg.failedDir = cfg.baseDir / "failed";
    return true;
}

std::string build_command(const Config &cfg, const fs::path &outfile) {
    std::ostringstream cmd;
    cmd << "rpicam-still"
        << " --nopreview"
        << " --encoding jpg"
        << " --timeout " << cfg.timeout_ms
        << " --width " << cfg.width
        << " --height " << cfg.height
        << " --autofocus-mode " << cfg.autofocus_mode;

    if (cfg.autofocus_mode != "manual") {
        cmd << " --autofocus-range " << cfg.autofocus_range;
        if (cfg.autofocus_mode == "auto") {
            cmd << " --autofocus-on-capture";
        }
    }

    if (!cfg.lens_position.empty()) {
        cmd << " --lens-position " << cfg.lens_position;
    }

    if (cfg.shutterUs > 0) {
        cmd << " --shutter " << cfg.shutterUs;
    }
    if (cfg.gain > 0) {
        cmd << " --gain " << cfg.gain;
    }

    cmd << " --output " << shell_escape(outfile.string());
    return cmd.str();
}

struct CaptureRequest {
    Config cfg;
    std::string rawPayload;
};

class RequestQueue {
public:
    void push(CaptureRequest req) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            q_.push_back(std::move(req));
        }
        cv_.notify_one();
    }

    bool pop(CaptureRequest &out) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]() { return g_stop.load() || !q_.empty(); });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<CaptureRequest> q_;
};

static bool extract_string_value(const std::string &json, const std::string &key, std::string &out) {
    std::regex rgx("\"" + key + "\"\\s*:\\s*\"((?:\\\\.|[^\"\\\\])*)\"");
    std::smatch m;
    if (!std::regex_search(json, m, rgx)) return false;
    out = m[1].str();
    return true;
}

static bool extract_int_value(const std::string &json, const std::string &key, int &out) {
    std::regex rgx("\"" + key + "\"\\s*:\\s*(-?\\d+)");
    std::smatch m;
    if (!std::regex_search(json, m, rgx)) return false;
    out = std::stoi(m[1].str());
    return true;
}

static bool extract_named_object(const std::string &json, const std::string &key, std::string &out) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    pos = json.find('{', pos);
    if (pos == std::string::npos) return false;

    int depth = 0;
    bool in_string = false;
    bool escape = false;
    std::size_t start = pos;

    for (std::size_t i = pos; i < json.size(); ++i) {
        char c = json[i];

        if (escape) {
            escape = false;
            continue;
        }
        if (c == '\\') {
            if (in_string) escape = true;
            continue;
        }
        if (c == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) continue;

        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                out = json.substr(start, i - start + 1);
                return true;
            }
        }
    }
    return false;
}

static void apply_json_overrides(const std::string &json, Config &cfg) {
    extract_int_value(json, "width", cfg.width);
    extract_int_value(json, "height", cfg.height);
    extract_int_value(json, "timeout_ms", cfg.timeout_ms);
    extract_int_value(json, "shutterUs", cfg.shutterUs);
    extract_int_value(json, "gain", cfg.gain);

    std::string s;
    if (extract_string_value(json, "autofocus_mode", s)) cfg.autofocus_mode = s;
    if (extract_string_value(json, "autofocus-range", s)) cfg.autofocus_range = s; // tolérance
    if (extract_string_value(json, "autofocus_range", s)) cfg.autofocus_range = s;
    if (extract_string_value(json, "lens_position", s)) cfg.lens_position = s;
}

static bool parse_capture_request(const std::string &payload, const Config &baseCfg, CaptureRequest &req, std::string &error) {
    std::string command;
    if (!extract_string_value(payload, "command", command)) {
        error = "JSON sans champ \"command\"";
        return false;
    }

    if (command != "capture_on_demand" && command != "capture" && command != "capture_now") {
        error = "Commande non supportee: " + command;
        return false;
    }

    req.cfg = baseCfg;
    req.rawPayload = payload;

    // Autoriser soit des paramètres à la racine, soit dans params { ... }
    apply_json_overrides(payload, req.cfg);

    std::string paramsObj;
    if (extract_named_object(payload, "params", paramsObj)) {
        apply_json_overrides(paramsObj, req.cfg);
    }

    return true;
}

struct AppContext {
    Config baseCfg;
    fs::path finalPath;
    RequestQueue queue;
};

static bool capture_once(const Config &cfg, const fs::path &finalPath) {
    std::error_code ec;
    fs::create_directories(cfg.pendingDir, ec);
    fs::create_directories(cfg.output_dir, ec);

    fs::path outfile = cfg.pendingDir / ("img_" + nowTimestamp() + ".jpg");
    std::string cmd = build_command(cfg, outfile);

    std::cout << "Capture command: " << cmd << "\n";
    int ret = std::system(cmd.c_str());

    if (ret == 0 && fs::exists(outfile) && fs::file_size(outfile) > 0) {
        ec.clear();
        fs::copy_file(outfile, finalPath, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "Copy failed to " << finalPath << " : " << ec.message() << "\n";
            return false;
        }

        auto now = std::chrono::system_clock::now();
        std::time_t tt = std::chrono::system_clock::to_time_t(now);
        std::cout << "Captured -> " << outfile << " at "
                  << std::put_time(std::localtime(&tt), "%F %T") << "\n";
        return true;
    }

    std::cerr << "Capture failed, exit code=" << ret << "\n";
    return false;
}

static void capture_worker(AppContext *app) {
    while (!g_stop.load()) {
        CaptureRequest req;
        if (!app->queue.pop(req)) {
            continue;
        }

        std::cout << "MQTT request received: " << req.rawPayload << "\n";
        capture_once(req.cfg, app->finalPath);
    }
}

static void on_connect(struct mosquitto *mosq, void *obj, int rc) {
    AppContext *app = static_cast<AppContext *>(obj);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "MQTT connect failed: " << mosquitto_connack_string(rc) << "\n";
        return;
    }

    int sub_rc = mosquitto_subscribe(mosq, nullptr, app->baseCfg.mqtt_topic.c_str(), app->baseCfg.mqtt_qos);
    if (sub_rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "MQTT subscribe failed: " << mosquitto_strerror(sub_rc) << "\n";
    } else {
        std::cout << "Subscribed to topic: " << app->baseCfg.mqtt_topic << "\n";
    }
}

static void on_disconnect(struct mosquitto *, void *, int rc) {
    std::cout << "MQTT disconnected, rc=" << rc << "\n";
}

static void on_message(struct mosquitto *, void *obj, const struct mosquitto_message *msg) {
    AppContext *app = static_cast<AppContext *>(obj);

    if (!msg || !msg->payload || msg->payloadlen <= 0) {
        std::cerr << "MQTT message vide ignore\n";
        return;
    }

    std::string payload(static_cast<const char *>(msg->payload),
                        static_cast<std::size_t>(msg->payloadlen));

    CaptureRequest req;
    std::string error;
    if (!parse_capture_request(payload, app->baseCfg, req, error)) {
        std::cerr << "MQTT JSON ignore: " << error << "\n";
        return;
    }

    app->queue.push(std::move(req));
}

int main(int argc, char **argv) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    Config cfg;
    if (!parse_args(argc, argv, cfg)) {
        print_usage(argv[0]);
        return 1;
    }

    std::error_code ec;
    fs::create_directories(cfg.output_dir, ec);
    fs::create_directories(cfg.pendingDir, ec);
    if (ec) {
        std::cerr << "Impossible de creer les dossiers : " << ec.message() << "\n";
        return 1;
    }

    // for auto capture
    std::error_code ec;
    fs::create_directories(cfg.output_dir, ec);
    if (ec) {
        std::cerr << "Impossible de creer le dossier " << cfg.output_dir
                  << " : " << ec.message() << "\n";
        return 1;
    }

    const fs::path final_path = cfg.output_dir / cfg.output_name;
    const fs::path temp_path = cfg.output_dir / (cfg.output_name + ".tmp");

    std::cout << "Capture daemon started\n";
    std::cout << "Output: " << final_path << "\n";
    std::cout << "Interval: " << cfg.interval_ms << " ms\n";
    std::cout << "Duration: " << cfg.duration << " ms\n";

    auto endTime = std::chrono::steady_clock::now() + std::chrono::minutes(cfg.duration);

    //while (!g_stop.load()) {
    while (!g_stop.load() && std::chrono::steady_clock::now() < endTime) {
        auto start = std::chrono::steady_clock::now();

        fs::path outfile = cfg.pendingDir / ("img_" + nowTimestamp() + ".jpg");
        std::string cmd = build_command(cfg, outfile);
        int ret = std::system(cmd.c_str());

        if (ret == 0 && fs::exists(outfile)) {
            // if (std::rename(outfile.c_str(), final_path.c_str()) != 0) {
            //     std::perror("rename");
            // } else {
                // Copie de final_path vers outfile
                std::error_code ec;
                fs::copy_file(outfile, final_path, fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    std::cerr << "Copy failed to " << outfile << " : " << ec.message() << "\n";
                }
                auto now = std::chrono::system_clock::now();
                std::time_t tt = std::chrono::system_clock::to_time_t(now);
                std::cout << "Captured -> " << outfile << " at "
                          << std::put_time(std::localtime(&tt), "%F %T") << "\n";
            // }
        } else {
            std::cerr << "Capture failed, exit code=" << ret << "\n";
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        int remaining = cfg.interval_ms - static_cast<int>(elapsed);
        if (remaining > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(remaining));
        }
    }
    //end for auto capture

    AppContext app;
    app.baseCfg = cfg;
    app.finalPath = cfg.output_dir / cfg.output_name;

    std::cout << "capture_on_demande started\n";
    std::cout << "Final image path: " << app.finalPath << "\n";
    std::cout << "Pending dir: " << cfg.pendingDir << "\n";
    std::cout << "MQTT broker: " << cfg.mqtt_host << ":" << cfg.mqtt_port << "\n";
    std::cout << "MQTT topic: " << cfg.mqtt_topic << "\n";

    mosquitto_lib_init();

    struct mosquitto *mosq = mosquitto_new(
        cfg.mqtt_client_id.empty() ? nullptr : cfg.mqtt_client_id.c_str(),
        cfg.mqtt_clean_session,
        &app
    );
    if (!mosq) {
        std::cerr << "mosquitto_new failed\n";
        mosquitto_lib_cleanup();
        return 1;
    }

    if (!cfg.mqtt_username.empty()) {
        int rc = mosquitto_username_pw_set(
            mosq,
            cfg.mqtt_username.c_str(),
            cfg.mqtt_password.empty() ? nullptr : cfg.mqtt_password.c_str()
        );
        if (rc != MOSQ_ERR_SUCCESS) {
            std::cerr << "mosquitto_username_pw_set failed: " << mosquitto_strerror(rc) << "\n";
            mosquitto_destroy(mosq);
            mosquitto_lib_cleanup();
            return 1;
        }
    }

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);
    mosquitto_message_callback_set(mosq, on_message);

    int rc = mosquitto_connect_async(mosq, cfg.mqtt_host.c_str(), cfg.mqtt_port, cfg.mqtt_keepalive);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "mosquitto_connect_async failed: " << mosquitto_strerror(rc) << "\n";
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }

    rc = mosquitto_loop_start(mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "mosquitto_loop_start failed: " << mosquitto_strerror(rc) << "\n";
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }

    std::thread worker(capture_worker, &app);

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    mosquitto_disconnect(mosq);
    mosquitto_loop_stop(mosq, true);

    app.queue.push(CaptureRequest{cfg, "{}"}); // réveille le worker
    if (worker.joinable()) worker.join();

    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();

    std::cout << "capture_on_demande stopped\n";
    return 0;
}
