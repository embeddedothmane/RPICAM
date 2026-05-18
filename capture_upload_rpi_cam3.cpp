#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

static std::atomic<bool> g_stop{false};

struct Config {
    fs::path baseDir = "/home/pi/captures_cam3";
    fs::path pendingDir = baseDir / "pending";
    fs::path uploadedDir = baseDir / "uploaded";
    fs::path failedDir = baseDir / "failed";
    std::string rcloneRemote = "gdrive:camera_uploads"; // ex: gdrive:camera_uploads
    int intervalSec = 5;

    // Camera Module 3 via rpicam-still
    int width = 2304;
    int height = 1296;
    int timeoutMs = 1200;         // laisse AE/AWB/AF converger
    int shutterUs = 0;            // 0 = auto
    int gain = 0;                 // 0 = auto
    bool autofocusOnCapture = true;
    bool verbose = true;
};

class WorkQueue {
public:
    void push(const fs::path &p) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(p);
        }
        cv_.notify_one();
    }

    bool pop(fs::path &out) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return g_stop.load() || !queue_.empty(); });
        if (queue_.empty()) return false;
        out = queue_.front();
        queue_.pop_front();
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<fs::path> queue_;
};

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

static std::string shellEscape(const std::string &s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

static int runCommand(const std::string &cmd) {
    return std::system(cmd.c_str());
}

static void ensureDirs(const Config &cfg) {
    fs::create_directories(cfg.pendingDir);
    fs::create_directories(cfg.uploadedDir);
    fs::create_directories(cfg.failedDir);
}

static bool captureImage(const Config &cfg, const fs::path &filePath) {
    // rpicam-still est l'outil officiel Raspberry Pi à partir de Bookworm.
    // On utilise autofocus en mode auto pour chaque prise.
    std::ostringstream cmd;
    cmd << "rpicam-still"
        << " --nopreview"
        << " --encoding jpg"
        << " --width " << cfg.width
        << " --height " << cfg.height
        << " --timeout " << cfg.timeoutMs;

    if (cfg.autofocusOnCapture) {
        cmd << " --autofocus-mode auto";
    }

    if (cfg.shutterUs > 0) {
        cmd << " --shutter " << cfg.shutterUs;
    }
    if (cfg.gain > 0) {
        cmd << " --gain " << cfg.gain;
    }

    cmd << " --output " << shellEscape(filePath.string());
    if (!cfg.verbose) {
        cmd << " >/dev/null 2>&1";
    }

    int rc = runCommand(cmd.str());
    return rc == 0 && fs::exists(filePath) && fs::file_size(filePath) > 0;
}

// static bool uploadWithRclone(const Config &cfg, const fs::path &filePath) {
//     // Upload vers n'importe quel drive supporté par rclone.
//     // copyto = envoie un fichier précis vers un chemin précis.
//     std::ostringstream remoteTarget;
//     remoteTarget << cfg.rcloneRemote;
//     if (!cfg.rcloneRemote.empty() && cfg.rcloneRemote.back() != '/') {
//         remoteTarget << '/';
//     }
//     remoteTarget << filePath.filename().string();

//     std::ostringstream cmd;
//     cmd << "rclone copyto "
//         << shellEscape(filePath.string()) << ' '
//         << shellEscape(remoteTarget.str())
//         << " --retries 3 --low-level-retries 3";

//     if (!cfg.verbose) {
//         cmd << " >/dev/null 2>&1";
//     }

//     int rc = runCommand(cmd.str());
//     return rc == 0;
// }

// static void preloadPending(const Config &cfg, WorkQueue &q) {
//     for (const auto &entry : fs::directory_iterator(cfg.pendingDir)) {
//         if (entry.is_regular_file() && entry.path().extension() == ".jpg") {
//             q.push(entry.path());
//         }
//     }
// }

static void captureThread(const Config &cfg, WorkQueue &q) {
    while (!g_stop.load()) {
        auto loopStart = std::chrono::steady_clock::now();
        fs::path out = cfg.pendingDir / ("img_" + nowTimestamp() + ".jpg");

        bool ok = captureImage(cfg, out);
        if (ok) {
            std::cout << "[CAPTURE] OK  " << out << std::endl;
            q.push(out);
        } else {
            std::cerr << "[CAPTURE] FAIL " << out << std::endl;
            if (fs::exists(out)) {
                std::error_code ec;
                fs::rename(out, cfg.failedDir / out.filename(), ec);
            }
        }

        auto elapsed = std::chrono::steady_clock::now() - loopStart;
        auto sleepFor = std::chrono::seconds(cfg.intervalSec) - elapsed;
        if (sleepFor > 0s) {
            for (auto slept = 0ms; slept < sleepFor && !g_stop.load(); slept += 200ms) {
                std::this_thread::sleep_for(200ms);
            }
        }
    }
}

// static void uploadThread(const Config &cfg, WorkQueue &q) {
//     while (!g_stop.load()) {
//         fs::path filePath;
//         if (!q.pop(filePath)) {
//             continue;
//         }
//         if (filePath.empty()) continue;
//         if (!fs::exists(filePath)) continue;

//         bool ok = uploadWithRclone(cfg, filePath);
//         if (ok) {
//             std::error_code ec;
//             fs::rename(filePath, cfg.uploadedDir / filePath.filename(), ec);
//             if (ec) {
//                 std::cerr << "[UPLOAD] OK mais déplacement impossible: " << ec.message() << std::endl;
//             } else {
//                 std::cout << "[UPLOAD] OK  " << filePath.filename() << std::endl;
//             }
//         } else {
//             std::cerr << "[UPLOAD] FAIL " << filePath.filename() << std::endl;
//             std::this_thread::sleep_for(15s);
//             if (!g_stop.load() && fs::exists(filePath)) {
//                 q.push(filePath); // retry plus tard
//             }
//         }
//     }
// }

static void signalHandler(int) {
    g_stop.store(true);
}

int main() {
    Config cfg;
    ensureDirs(cfg);

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout << "Base dir      : " << cfg.baseDir << std::endl;
    std::cout << "Pending dir   : " << cfg.pendingDir << std::endl;
    std::cout << "Uploaded dir  : " << cfg.uploadedDir << std::endl;
    std::cout << "Failed dir    : " << cfg.failedDir << std::endl;
    std::cout << "Rclone remote : " << cfg.rcloneRemote << std::endl;

    WorkQueue q;
    preloadPending(cfg, q);

    std::thread tCap(captureThread, std::cref(cfg), std::ref(q));
    std::thread tUp(uploadThread, std::cref(cfg), std::ref(q));

    tCap.join();
    tUp.join();
    return 0;
}
