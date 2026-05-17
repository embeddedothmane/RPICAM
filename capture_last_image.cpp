#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>

namespace fs = std::filesystem;

static std::atomic<bool> g_stop{false};

void handle_signal(int) {
    g_stop = true;
}

struct Config {
    fs::path output_dir = "/var/lib/rpicam-http";
    std::string output_name = "LAST_CAPTURE.jpg";
    fs::path baseDir = "/home/pi/captures_cam3";
    fs::path pendingDir = baseDir / "pending";
    fs::path uploadedDir = baseDir / "uploaded";
    fs::path failedDir = baseDir / "failed";
    int intervalSec = 5;

    int interval_ms = 5000;
    int width = 2304;//1920;
    int height = 1296;//1080;
    int timeout_ms = 700; // 1200;         // laisse AE/AWB/AF converger
    std::string autofocus_mode = "auto";
    std::string autofocus_range = "normal";
    std::string lens_position;

    int shutterUs = 0;            // 0 = auto
    int gain = 0;                 // 0 = auto
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

void print_usage(const char *prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "Options:\n"
        << "  --output-dir PATH        Dossier de sortie (defaut: /var/lib/rpicam-http)\n"
        << "  --output-name NAME       Nom du fichier final (defaut: LAST_CAPTURE.jpg)\n"
        << "  --interval-ms N          Intervalle entre captures (defaut: 5000)\n"
        << "  --width N                Largeur image (defaut: 2304)\n"
        << "  --height N               Hauteur image (defaut: 1296)\n"
        << "  --timeout-ms N           Temps preview/AF avant capture (defaut: 700)\n"
        << "  --autofocus-mode MODE    auto | continuous | manual (defaut: auto)\n"
        << "  --autofocus-range RANGE  normal | macro | full (defaut: normal)\n"
        << "  --lens-position VALUE    Position de lentille en dioptres si mode manual\n";
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
        } else if (a == "--help" || a == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "Option inconnue: " << a << "\n";
            return false;
        }
    }
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

    while (!g_stop.load()) {
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

    std::cout << "Capture daemon stopped\n";
    return 0;
}
