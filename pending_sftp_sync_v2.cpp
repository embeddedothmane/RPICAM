#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>

namespace fs = std::filesystem;

static std::atomic<bool> g_stop{false};

void handle_signal(int) {
    g_stop = true;
}

struct Config {
    fs::path pending_dir = "/home/pi/captures_cam3/pending";
    fs::path uploaded_dir = "/home/pi/captures_cam3/uploaded";
    fs::path failed_dir = "/home/pi/captures_cam3/failed";

    std::string remote_host;
    int remote_port = 22;
    std::string remote_user;
    std::string remote_dir = "/srv/RPICAMM/upload";

    fs::path ssh_private_key = "/home/pi/.ssh/id_ed25519_capture_sync";
    fs::path ssh_public_key = "/home/pi/.ssh/id_ed25519_capture_sync.pub";
    fs::path ssh_known_hosts = "/home/pi/.ssh/known_hosts";
    std::string ssh_key_passphrase;

    int scan_interval_s = 2;
    int retry_wait_s = 10;
    int offline_wait_s = 15;
    int connect_timeout_s = 10;
    int transfer_timeout_s = 120;
    long low_speed_limit = 32;
    int low_speed_time_s = 30;
    int max_retries = 10;
    int stability_seconds = 5;

    bool verbose = true;
};

struct UploadResult {
    enum class Status {
        Success,
        NetworkDown,
        Failed
    };

    Status status = Status::Failed;
    CURLcode curl_code = CURLE_OK;
    std::string error_message;
};

struct PendingFile {
    fs::path path;
    fs::file_time_type mtime;
};

void print_usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "Required:\n"
        << "  --host HOST                VPS hostname or IP\n"
        << "  --user USER                VPS SSH user\n"
        << "Optional:\n"
        << "  --port N                   SFTP port (default: 22)\n"
        << "  --remote-dir PATH          Remote upload dir (default: /srv/RPICAMM/upload)\n"
        << "  --pending-dir PATH         Local pending dir\n"
        << "  --uploaded-dir PATH        Local uploaded dir\n"
        << "  --failed-dir PATH          Local failed dir\n"
        << "  --private-key PATH         SSH private key\n"
        << "  --public-key PATH          SSH public key\n"
        << "  --known-hosts PATH         OpenSSH known_hosts file\n"
        << "  --key-passphrase TEXT      Passphrase for private key\n"
        << "  --scan-interval-s N        Delay between scans when idle (default: 2)\n"
        << "  --retry-wait-s N           Delay before retry after non-network failure (default: 10)\n"
        << "  --offline-wait-s N         Delay while VPS/network is down (default: 15)\n"
        << "  --connect-timeout-s N      Connect timeout (default: 10)\n"
        << "  --transfer-timeout-s N     Transfer timeout (default: 120)\n"
        << "  --low-speed-limit N        Abort if slower than N bytes/s (default: 32)\n"
        << "  --low-speed-time-s N       Low-speed window in seconds (default: 30)\n"
        << "  --max-retries N            Max non-network retries before failed (default: 10)\n"
        << "  --stability-seconds N      Ignore files modified too recently (default: 5)\n"
        << "  --quiet                    Reduce logs\n";
}

bool parse_args(int argc, char** argv, Config& cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const std::string& opt) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Option sans valeur: " << opt << "\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--host") {
            if (const char* v = need_value(a)) cfg.remote_host = v; else return false;
        } else if (a == "--user") {
            if (const char* v = need_value(a)) cfg.remote_user = v; else return false;
        } else if (a == "--port") {
            if (const char* v = need_value(a)) cfg.remote_port = std::stoi(v); else return false;
        } else if (a == "--remote-dir") {
            if (const char* v = need_value(a)) cfg.remote_dir = v; else return false;
        } else if (a == "--pending-dir") {
            if (const char* v = need_value(a)) cfg.pending_dir = v; else return false;
        } else if (a == "--uploaded-dir") {
            if (const char* v = need_value(a)) cfg.uploaded_dir = v; else return false;
        } else if (a == "--failed-dir") {
            if (const char* v = need_value(a)) cfg.failed_dir = v; else return false;
        } else if (a == "--private-key") {
            if (const char* v = need_value(a)) cfg.ssh_private_key = v; else return false;
        } else if (a == "--public-key") {
            if (const char* v = need_value(a)) cfg.ssh_public_key = v; else return false;
        } else if (a == "--known-hosts") {
            if (const char* v = need_value(a)) cfg.ssh_known_hosts = v; else return false;
        } else if (a == "--key-passphrase") {
            if (const char* v = need_value(a)) cfg.ssh_key_passphrase = v; else return false;
        } else if (a == "--scan-interval-s") {
            if (const char* v = need_value(a)) cfg.scan_interval_s = std::stoi(v); else return false;
        } else if (a == "--retry-wait-s") {
            if (const char* v = need_value(a)) cfg.retry_wait_s = std::stoi(v); else return false;
        } else if (a == "--offline-wait-s") {
            if (const char* v = need_value(a)) cfg.offline_wait_s = std::stoi(v); else return false;
        } else if (a == "--connect-timeout-s") {
            if (const char* v = need_value(a)) cfg.connect_timeout_s = std::stoi(v); else return false;
        } else if (a == "--transfer-timeout-s") {
            if (const char* v = need_value(a)) cfg.transfer_timeout_s = std::stoi(v); else return false;
        } else if (a == "--low-speed-limit") {
            if (const char* v = need_value(a)) cfg.low_speed_limit = std::stol(v); else return false;
        } else if (a == "--low-speed-time-s") {
            if (const char* v = need_value(a)) cfg.low_speed_time_s = std::stoi(v); else return false;
        } else if (a == "--max-retries") {
            if (const char* v = need_value(a)) cfg.max_retries = std::stoi(v); else return false;
        } else if (a == "--stability-seconds") {
            if (const char* v = need_value(a)) cfg.stability_seconds = std::stoi(v); else return false;
        } else if (a == "--quiet") {
            cfg.verbose = false;
        } else if (a == "--help" || a == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "Option inconnue: " << a << "\n";
            return false;
        }
    }

    if (cfg.remote_host.empty() || cfg.remote_user.empty()) {
        std::cerr << "--host et --user sont obligatoires\n";
        return false;
    }
    return true;
}

void ensure_dirs(const Config& cfg) {
    std::error_code ec;
    fs::create_directories(cfg.pending_dir, ec);
    fs::create_directories(cfg.uploaded_dir, ec);
    fs::create_directories(cfg.failed_dir, ec);
}

bool safe_move(const fs::path& from, const fs::path& to, std::string& err) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);

    ec.clear();
    fs::rename(from, to, ec);
    if (!ec) return true;

    ec.clear();
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        err = ec.message();
        return false;
    }

    ec.clear();
    fs::remove(from, ec);
    if (ec) {
        err = ec.message();
        return false;
    }
    return true;
}

std::vector<PendingFile> scan_pending_files(const Config& cfg) {
    std::vector<PendingFile> files;
    std::error_code ec;
    auto now = fs::file_time_type::clock::now();
    auto min_age = std::chrono::seconds(cfg.stability_seconds);

    if (!fs::exists(cfg.pending_dir, ec)) return files;

    for (const auto& entry : fs::directory_iterator(cfg.pending_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) continue;

        const auto name = entry.path().filename().string();
        if (!name.empty() && name[0] == '.') continue;

        auto mtime = entry.last_write_time(ec);
        if (ec) continue;

        if ((now - mtime) < min_age) continue;

        files.push_back({entry.path(), mtime});
    }

    std::sort(files.begin(), files.end(), [](const PendingFile& a, const PendingFile& b) {
        if (a.mtime == b.mtime) {
            return a.path.filename().string() < b.path.filename().string();
        }
        return a.mtime < b.mtime;
    });

    return files;
}

bool is_network_error(CURLcode code) {
    switch (code) {
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_GOT_NOTHING:
            return true;
        default:
            return false;
    }
}

std::string url_encode(CURL* curl, const std::string& value) {
    char* escaped = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
    if (!escaped) return value;
    std::string result = escaped;
    curl_free(escaped);
    return result;
}

UploadResult upload_file_sftp(const Config& cfg, const fs::path& local_file) {
    UploadResult result;

    FILE* fp = std::fopen(local_file.c_str(), "rb");
    if (!fp) {
        result.status = UploadResult::Status::Failed;
        result.error_message = std::strerror(errno);
        return result;
    }

    std::error_code ec;
    auto file_size = fs::file_size(local_file, ec);
    if (ec) {
        std::fclose(fp);
        result.status = UploadResult::Status::Failed;
        result.error_message = ec.message();
        return result;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::fclose(fp);
        result.status = UploadResult::Status::Failed;
        result.error_message = "curl_easy_init failed";
        return result;
    }

    std::string error_buffer(CURL_ERROR_SIZE, '\0');
    std::string filename = local_file.filename().string();
    std::string encoded_filename = url_encode(curl, filename);

    std::string remote_dir = cfg.remote_dir;
    if (!remote_dir.empty() && remote_dir.back() != '/') {
        remote_dir += '/';
    }

    std::string url = "sftp://" + cfg.remote_host + ":" + std::to_string(cfg.remote_port)
                    + remote_dir + encoded_filename;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READDATA, fp);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(file_size));
    curl_easy_setopt(curl, CURLOPT_USERNAME, cfg.remote_user.c_str());
    curl_easy_setopt(curl, CURLOPT_SSH_AUTH_TYPES, CURLSSH_AUTH_PUBLICKEY);
    curl_easy_setopt(curl, CURLOPT_SSH_PRIVATE_KEYFILE, cfg.ssh_private_key.c_str());

    if (fs::exists(cfg.ssh_public_key)) {
        curl_easy_setopt(curl, CURLOPT_SSH_PUBLIC_KEYFILE, cfg.ssh_public_key.c_str());
    }
    if (!cfg.ssh_key_passphrase.empty()) {
        curl_easy_setopt(curl, CURLOPT_KEYPASSWD, cfg.ssh_key_passphrase.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_SSH_KNOWNHOSTS, cfg.ssh_known_hosts.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(cfg.connect_timeout_s));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(cfg.transfer_timeout_s));
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, cfg.low_speed_limit);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, static_cast<long>(cfg.low_speed_time_s));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer.data());
    curl_easy_setopt(curl, CURLOPT_FTP_CREATE_MISSING_DIRS, CURLFTP_CREATE_DIR_RETRY);

    CURLcode code = curl_easy_perform(curl);
    result.curl_code = code;

    if (code == CURLE_OK) {
        result.status = UploadResult::Status::Success;
    } else if (is_network_error(code)) {
        result.status = UploadResult::Status::NetworkDown;
        result.error_message = error_buffer.c_str();
    } else {
        result.status = UploadResult::Status::Failed;
        result.error_message = error_buffer.c_str();
    }

    curl_easy_cleanup(curl);
    std::fclose(fp);
    return result;
}

void sleep_interruptible(int seconds) {
    for (int i = 0; i < seconds && !g_stop.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

int main(int argc, char** argv) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    Config cfg;
    if (!parse_args(argc, argv, cfg)) {
        print_usage(argv[0]);
        return 1;
    }

    ensure_dirs(cfg);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    fs::path current_file;
    int current_attempts = 0;

    if (cfg.verbose) {
        std::cout << "pending_sftp_sync started\n";
        std::cout << "Pending:   " << cfg.pending_dir << "\n";
        std::cout << "Uploaded:  " << cfg.uploaded_dir << "\n";
        std::cout << "Failed:    " << cfg.failed_dir << "\n";
        std::cout << "Remote:    " << cfg.remote_user << '@' << cfg.remote_host << ':'
                  << cfg.remote_port << cfg.remote_dir << "\n";
        std::cout << "Stability: " << cfg.stability_seconds << "s\n";
        std::cout << "Rule: list -> filter recent files -> sort oldest first -> upload one -> repeat\n";
    }

    while (!g_stop.load()) {
        auto files = scan_pending_files(cfg);

        if (files.empty()) {
            current_file.clear();
            current_attempts = 0;
            sleep_interruptible(cfg.scan_interval_s);
            continue;
        }

        const auto& file = files.front();

        if (current_file != file.path) {
            current_file = file.path;
            current_attempts = 0;
        }

        if (cfg.verbose) {
            std::cout << "Uploading oldest eligible file: " << file.path
                      << " (attempts=" << current_attempts << ")\n";
        }

        UploadResult res = upload_file_sftp(cfg, file.path);

        if (res.status == UploadResult::Status::Success) {
            std::string err;
            fs::path target = cfg.uploaded_dir / file.path.filename();
            if (safe_move(file.path, target, err)) {
                if (cfg.verbose) {
                    std::cout << "Uploaded OK -> " << target << "\n";
                }
                current_file.clear();
                current_attempts = 0;
            } else {
                std::cerr << "Uploaded but local move to uploaded failed: " << err << "\n";
            }
            continue;
        }

        if (res.status == UploadResult::Status::NetworkDown) {
            if (cfg.verbose) {
                std::cerr << "Network/VPS unavailable for " << file.path << ": "
                          << static_cast<int>(res.curl_code) << " " << res.error_message << "\n";
                std::cerr << "Waiting " << cfg.offline_wait_s << "s before resume...\n";
            }
            sleep_interruptible(cfg.offline_wait_s);
            continue;
        }

        current_attempts += 1;
        std::cerr << "Upload failed for " << file.path << " (attempt " << current_attempts << "/"
                  << cfg.max_retries << "): " << static_cast<int>(res.curl_code) << " "
                  << res.error_message << "\n";

        if (current_attempts >= cfg.max_retries) {
            std::string err;
            fs::path target = cfg.failed_dir / file.path.filename();
            if (safe_move(file.path, target, err)) {
                std::cerr << "Moved to failed -> " << target << "\n";
                current_file.clear();
                current_attempts = 0;
            } else {
                std::cerr << "Could not move failed file: " << err << "\n";
            }
        } else {
            sleep_interruptible(cfg.retry_wait_s);
        }
    }

    curl_global_cleanup();
    if (cfg.verbose) {
        std::cout << "pending_sftp_sync stopped\n";
    }
    return 0;
}
