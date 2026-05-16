#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <atomic>

namespace fs = std::filesystem;

static std::atomic<bool> g_stop{false};

void handle_signal(int) {
    g_stop = true;
}

struct Config {
    fs::path public_dir = "/var/lib/rpicam-http";
    std::string image_name = "LAST_CAPTURE.jpg";
    int port = 8080;
};

void print_usage(const char *prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "Options:\n"
        << "  --public-dir PATH   Dossier contenant LAST_CAPTURE.jpg (defaut: /var/lib/rpicam-http)\n"
        << "  --image-name NAME   Nom image (defaut: LAST_CAPTURE.jpg)\n"
        << "  --port N            Port HTTP (defaut: 8080)\n";
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

        if (a == "--public-dir") {
            if (const char *v = need_value(a)) cfg.public_dir = v; else return false;
        } else if (a == "--image-name") {
            if (const char *v = need_value(a)) cfg.image_name = v; else return false;
        } else if (a == "--port") {
            if (const char *v = need_value(a)) cfg.port = std::stoi(v); else return false;
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

bool send_all(int fd, const char *data, size_t len) {
    while (len > 0) {
        ssize_t n = send(fd, data, len, 0);
        if (n <= 0) return false;
        data += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool send_all(int fd, const std::string &s) {
    return send_all(fd, s.data(), s.size());
}

std::string html_page(const std::string &image_name) {
    std::ostringstream html;
    html << "<!doctype html>\n"
         << "<html lang=\"fr\">\n"
         << "<head>\n"
         << "  <meta charset=\"utf-8\">\n"
         << "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
         << "  <title>Raspberry Pi Camera - Derniere capture</title>\n"
         << "  <style>\n"
         << "    body{font-family:Arial,Helvetica,sans-serif;background:#111;color:#eee;margin:0;padding:24px;}\n"
         << "    .wrap{max-width:1100px;margin:0 auto;}\n"
         << "    h1{margin:0 0 8px 0;font-size:28px;}\n"
         << "    p{color:#bbb;}\n"
         << "    .card{background:#1b1b1b;border:1px solid #333;border-radius:16px;padding:16px;}\n"
         << "    img{max-width:100%;height:auto;display:block;border-radius:12px;background:#000;}\n"
         << "    .meta{margin-top:12px;font-size:14px;color:#aaa;}\n"
         << "    code{background:#222;padding:2px 6px;border-radius:6px;}\n"
         << "  </style>\n"
         << "</head>\n"
         << "<body>\n"
         << "  <div class=\"wrap\">\n"
         << "    <h1>Derniere image capturee</h1>\n"
         << "    <p>Cette page recharge l'image toutes les 1 seconde.</p>\n"
         << "    <div class=\"card\">\n"
         << "      <img id=\"cam\" src=\"/" << image_name << "?t=0\" alt=\"Derniere capture\">\n"
         << "      <div class=\"meta\">Mise a jour navigateur: <span id=\"ts\">-</span><br>Fichier: <code>"
         << image_name << "</code></div>\n"
         << "    </div>\n"
         << "  </div>\n"
         << "  <script>\n"
         << "    const img = document.getElementById('cam');\n"
         << "    const ts = document.getElementById('ts');\n"
         << "    function refreshImage(){\n"
         << "      const now = Date.now();\n"
         << "      img.src = '/" << image_name << "?t=' + now;\n"
         << "      ts.textContent = new Date().toLocaleString();\n"
         << "    }\n"
         << "    refreshImage();\n"
         << "    setInterval(refreshImage, 1000);\n"
         << "  </script>\n"
         << "</body>\n"
         << "</html>\n";
    return html.str();
}

std::vector<char> read_file(const fs::path &path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {};
    return std::vector<char>((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
}

void send_response(int client_fd, int status, const std::string &status_text,
                   const std::string &content_type, const char *body, size_t body_len) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " " << status_text << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body_len << "\r\n"
        << "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
        << "Pragma: no-cache\r\n"
        << "Connection: close\r\n\r\n";
    send_all(client_fd, oss.str());
    if (body_len > 0) send_all(client_fd, body, body_len);
}

void send_string_response(int client_fd, int status, const std::string &status_text,
                          const std::string &content_type, const std::string &body) {
    send_response(client_fd, status, status_text, content_type, body.data(), body.size());
}

void handle_client(int client_fd, Config cfg) {
    std::string request;
    char buffer[4096];

    while (request.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            close(client_fd);
            return;
        }
        request.append(buffer, buffer + n);
        if (request.size() > 16384) break;
    }

    std::istringstream iss(request);
    std::string method, target, version;
    iss >> method >> target >> version;

    if (method != "GET") {
        send_string_response(client_fd, 405, "Method Not Allowed", "text/plain; charset=utf-8",
                             "Only GET is supported\n");
        close(client_fd);
        return;
    }

    std::string path = target;
    auto q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);

    if (path == "/" || path == "/index.html") {
        std::string body = html_page(cfg.image_name);
        send_string_response(client_fd, 200, "OK", "text/html; charset=utf-8", body);
    } else if (path == "/" + cfg.image_name) {
        fs::path img_path = cfg.public_dir / cfg.image_name;
        auto data = read_file(img_path);
        if (data.empty()) {
            send_string_response(client_fd, 404, "Not Found", "text/plain; charset=utf-8",
                                 "Image not found yet\n");
        } else {
            send_response(client_fd, 200, "OK", "image/jpeg", data.data(), data.size());
        }
    } else if (path == "/favicon.ico") {
        send_response(client_fd, 204, "No Content", "image/x-icon", "", 0);
    } else {
        send_string_response(client_fd, 404, "Not Found", "text/plain; charset=utf-8",
                             "Not found\n");
    }

    close(client_fd);
}

int main(int argc, char **argv) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    Config cfg;
    if (!parse_args(argc, argv, cfg)) {
        print_usage(argv[0]);
        return 1;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::perror("socket");
        return 1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::perror("setsockopt");
        close(server_fd);
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(cfg.port));

    if (bind(server_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 16) < 0) {
        std::perror("listen");
        close(server_fd);
        return 1;
    }

    std::cout << "HTTP server listening on port " << cfg.port << "\n";
    std::cout << "Serving image: " << (cfg.public_dir / cfg.image_name) << "\n";

    while (!g_stop.load()) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            std::perror("accept");
            break;
        }

        std::thread([client_fd, cfg]() {
            handle_client(client_fd, cfg);
        }).detach();
    }

    close(server_fd);
    std::cout << "HTTP server stopped\n";
    return 0;
}
