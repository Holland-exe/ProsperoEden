// pe-updater - downloads the newest ProsperoEden test build and installs it in place.
// A payload for the console's ELF loader (payload manager, or port 9021).
// SPDX-License-Identifier: GPL-3.0-or-later
//
//  1. Reads SHA256SUMS from the ps5-drop branch: the ZIP's name and checksum.
//  2. Stops if that build is already installed, or if ProsperoEden is running.
//  3. Downloads the ZIP over HTTPS (libcurl, checked against the console's own certificate list;
//     the console's HTTP/2 library as a fallback) and checks its SHA-256.
//  4. Unpacks the app folder with ProsperoEden's validated extractor (self_update_helper).
//  5. Moves the installed app folder (the one /user/app/PPSA99008/mount.lnk points to) to
//     /data/pe-backup/<name>-<date>, and puts the new one in its place, so the home screen
//     icon opens the new build with no new registration. If anything fails, it puts the old
//     folder back.
// Every step is shown as a console notification and written to /data/pe-updater/log.txt.
#include "archive.hpp"
#include "files.hpp"
#include "self_update_sha256.h"

#include <curl/curl.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
int sceKernelSendNotificationRequest(int device, void *request, std::size_t size, int blocking);
int sceNetInit();
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int id);
int sceSslInit(std::size_t size);
int sceSslTerm(int id);
int sceHttp2Init(int net, int ssl, std::size_t size, int connections);
int sceHttp2Term(int id);
int sceHttp2CreateTemplate(int context, const char *agent, int version, int auto_proxy);
int sceHttp2DeleteTemplate(int id);
int sceHttp2CreateRequestWithURL(int template_id, const char *method, const char *url,
                                 std::uint64_t content_length);
int sceHttp2DeleteRequest(int id);
int sceHttp2SendRequest(int request, const void *data, std::size_t size);
int sceHttp2GetStatusCode(int request, int *status);
int sceHttp2ReadData(int request, void *data, std::size_t size);
}

namespace {
constexpr const char *kBase = "https://raw.githubusercontent.com/Holland-exe/ProsperoEden/ps5-drop/";
constexpr const char *kTitle = "PPSA99008";
constexpr const char *kWork = "/data/pe-updater";
constexpr const char *kBackups = "/data/pe-backup";
constexpr const char *kLink = "/user/app/PPSA99008/mount.lnk";
constexpr const char *kFallback = "/data/homebrew/PPSA99008";

FILE *log_file = nullptr;

void log_line(const char *format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::printf("[pe-updater] %s\n", text);
    if (log_file) {
        std::fprintf(log_file, "%s\n", text);
        std::fflush(log_file);
    }
}

bool exists_file(const char *path) {
    struct stat info {};
    return stat(path, &info) == 0;
}

struct NotificationRequest {
    std::uint8_t reserved[45];
    char message[3075];
};

void notify(const char *format, ...) {
    static NotificationRequest request;
    std::memset(&request, 0, sizeof(request));
    va_list args;
    va_start(args, format);
    std::vsnprintf(request.message, sizeof(request.message), format, args);
    va_end(args);
    log_line("notify: %s", request.message);
    (void)sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

// ------------------------------------------------------------------ HTTPS (sceHttp2)

struct Http {
    int net = -1, ssl = -1, context = -1, templ = -1;
    bool open() {
        sceNetInit();
        if ((net = sceNetPoolCreate("pe-updater", 64 * 1024, 0)) < 0) return false;
        if ((ssl = sceSslInit(512 * 1024)) < 0) return false;
        if ((context = sceHttp2Init(net, ssl, 512 * 1024, 2)) < 0) return false;
        if ((templ = sceHttp2CreateTemplate(context, "pe-updater/1.0", 3, 1)) < 0) return false;
        return true;
    }
    ~Http() {
        if (templ >= 0) sceHttp2DeleteTemplate(templ);
        if (context >= 0) sceHttp2Term(context);
        if (ssl >= 0) sceSslTerm(ssl);
        if (net >= 0) sceNetPoolDestroy(net);
    }
    // GETs url; each chunk goes to sink, which returns false to stop. Returns the HTTP status,
    // or a negative value when the request itself failed.
    template <typename Sink> int get(const std::string &url, Sink sink, std::uint64_t &bytes) {
        bytes = 0;
        const int request = sceHttp2CreateRequestWithURL(templ, "GET", url.c_str(), 0);
        if (request < 0) {
            log_line("http2: create request 0x%08x", static_cast<unsigned>(request));
            return request;
        }
        int status = -1;
        const int sent = sceHttp2SendRequest(request, nullptr, 0);
        const int got_status = sent == 0 ? sceHttp2GetStatusCode(request, &status) : 0;
        if (sent != 0 || got_status != 0) {
            log_line("http2: send 0x%08x, status 0x%08x", static_cast<unsigned>(sent), static_cast<unsigned>(got_status));
            sceHttp2DeleteRequest(request);
            return -2;
        }
        if (status == 200) {
            static char buffer[256 * 1024];
            for (;;) {
                const int got = sceHttp2ReadData(request, buffer, sizeof(buffer));
                if (got < 0) { status = -3; break; }
                if (got == 0) break;
                if (!sink(buffer, static_cast<std::size_t>(got))) { status = -4; break; }
                bytes += static_cast<std::uint64_t>(got);
            }
        }
        sceHttp2DeleteRequest(request);
        return status;
    }
};

// ------------------------------------------------------------------ HTTPS (libcurl)

constexpr const char *kCaList = "/system/common/cert/CA_LIST.cer";

template <typename Sink> std::size_t curl_sink(char *data, std::size_t size, std::size_t count, void *user) {
    return (*static_cast<Sink *>(user))(data, size * count) ? size * count : 0;
}

// GETs url with libcurl; returns the HTTP status, or a negative value when curl failed.
template <typename Sink> int curl_get(const std::string &url, Sink &sink, std::uint64_t &bytes) {
    bytes = 0;
    CURL *easy = curl_easy_init();
    if (!easy) {
        log_line("curl: init failed");
        return -10;
    }
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "pe-updater/1.1");
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 60L);
    if (exists_file(kCaList)) curl_easy_setopt(easy, CURLOPT_CAINFO, kCaList);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &curl_sink<Sink>);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &sink);
    const CURLcode result = curl_easy_perform(easy);
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    curl_off_t received = 0;
    curl_easy_getinfo(easy, CURLINFO_SIZE_DOWNLOAD_T, &received);
    bytes = static_cast<std::uint64_t>(received);
    curl_easy_cleanup(easy);
    if (result != CURLE_OK) {
        log_line("curl: %s (%d)", curl_easy_strerror(result), static_cast<int>(result));
        return -11;
    }
    return static_cast<int>(status);
}

// libcurl first; if it could not connect at all, the console's HTTP/2 library.
template <typename Sink, typename Reset>
int fetch(Http &http, bool &http_ready, const std::string &url, Sink sink, Reset reset, std::uint64_t &bytes) {
    int status = curl_get(url, sink, bytes);
    if (status == 200 || status > 0) return status;
    reset();
    if (!http_ready) {
        http_ready = http.open();
        if (!http_ready) {
            log_line("http2: the console's network library did not start");
            return status;
        }
    }
    log_line("trying the console's HTTP/2 library");
    return http.get(url, sink, bytes);
}

// ------------------------------------------------------------------ helpers

std::string trim(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' ||
                             text.back() == '\0' || text.back() == '\t'))
        text.pop_back();
    std::size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t')) ++start;
    return text.substr(start);
}

bool exists(const std::string &path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0;
}

bool title_running() {
    DIR *folder = opendir("/mnt/sandbox");
    if (!folder) return false;
    bool running = false;
    const std::string prefix = std::string(kTitle) + "_";
    while (dirent *entry = readdir(folder))
        if (std::strncmp(entry->d_name, prefix.c_str(), prefix.size()) == 0) running = true;
    closedir(folder);
    return running;
}

std::string read_text(const std::string &path) {
    std::string body;
    if (!self_update::read_small(path, 64 * 1024, body)) return {};
    return body;
}

bool write_text(const std::string &path, const std::string &text) {
    FILE *file = std::fopen(path.c_str(), "w");
    if (!file) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    return std::fclose(file) == 0 && ok;
}

std::string stamp() {
    char text[32];
    const std::time_t now = std::time(nullptr);
    std::tm parts {};
    localtime_r(&now, &parts);
    std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &parts);
    return text;
}

std::string base_name(const std::string &path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

int fail(const char *what) {
    notify("ProsperoEden update failed: %s. Nothing was changed. See %s/log.txt", what, kWork);
    return 1;
}
} // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    // Standard output is the loader's connection to PE Updater, which shows each line as it comes.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    mkdir(kWork, 0777);
    log_file = std::fopen((std::string(kWork) + "/log.txt").c_str(), "w");
    log_line("pe-updater started %s", stamp().c_str());

    if (title_running()) {
        notify("Close ProsperoEden first, then run the updater again.");
        return 1;
    }

    // Where the app is installed: the folder the home screen entry mounts.
    std::string target = trim(read_text(kLink));
    if (target.empty() || target[0] != '/' || !exists(target)) {
        log_line("mount.lnk gave '%s'; using %s", target.c_str(), kFallback);
        target = kFallback;
    }
    log_line("install folder: %s", target.c_str());
    // Only ever an app folder: named after the title, holding eboot.bin (when it exists), and
    // never the emulator's data folder, where the games, keys, firmware and saves are.
    if (target.find("..") != std::string::npos || target.rfind("/data/prosperoeden", 0) == 0 ||
        base_name(target).rfind(kTitle, 0) != 0 ||
        (exists(target) && !exists(target + "/eboot.bin")))
        return fail("the install folder does not look like the ProsperoEden app; nothing was touched");

    curl_global_init(CURL_GLOBAL_DEFAULT);
    log_line("certificates: %s", exists_file(kCaList) ? kCaList : "(none found)");
    Http http;
    bool http_ready = false;
    notify("ProsperoEden updater: checking for a new build...");

    // 1. SHA256SUMS: "<hash>  <zip name>"
    std::string sums;
    std::uint64_t bytes = 0;
    int status = fetch(http, http_ready, std::string(kBase) + "SHA256SUMS", [&](const char *data, std::size_t size) {
        sums.append(data, size);
        return sums.size() < 4096;
    }, [&] { sums.clear(); }, bytes);
    log_line("SHA256SUMS: status %d, %llu bytes", status, static_cast<unsigned long long>(bytes));
    if (status != 200) return fail("could not reach GitHub (is the PS5 online?)");
    const std::string first = trim(sums.substr(0, sums.find('\n')));
    const auto gap = first.find("  ");
    if (gap != 64 || first.size() < 70) return fail("the build list on GitHub is not as expected");
    const std::string want_hash = first.substr(0, 64);
    const std::string zip_name = trim(first.substr(gap + 2));
    if (zip_name.find('/') != std::string::npos || zip_name.size() < 5 ||
        zip_name.compare(zip_name.size() - 4, 4, ".zip") != 0)
        return fail("the build list names an unexpected file");
    log_line("latest: %s %s", zip_name.c_str(), want_hash.c_str());

    const std::string installed_file = std::string(kWork) + "/installed.txt";
    if (trim(read_text(installed_file)) == want_hash && exists(target + "/eboot.bin")) {
        notify("ProsperoEden is already up to date.");
        return 0;
    }

    // 2. Download the ZIP, hashing as it arrives.
    const std::string zip_path = std::string(kWork) + "/download.zip";
    self_update::remove_tree(zip_path);
    FILE *out = std::fopen(zip_path.c_str(), "wb");
    if (!out) return fail("could not write to /data");
    self_update_sha256 hash;
    self_update_sha256_start(&hash);
    notify("Downloading %s...", zip_name.c_str());
    status = fetch(http, http_ready, std::string(kBase) + zip_name, [&](const char *data, std::size_t size) {
        self_update_sha256_add(&hash, data, size);
        return std::fwrite(data, 1, size, out) == size;
    }, [&] {
        // A failed first attempt: start the file and the checksum again.
        std::fflush(out);
        if (ftruncate(fileno(out), 0) == 0) std::rewind(out);
        self_update_sha256_start(&hash);
    }, bytes);
    const bool closed = std::fclose(out) == 0;
    log_line("download: status %d, %llu bytes", status, static_cast<unsigned long long>(bytes));
    if (status != 200 || !closed) {
        self_update::remove_tree(zip_path);
        return fail("the download did not finish");
    }
    unsigned char digest[32];
    self_update_sha256_finish(&hash, digest);
    if (!self_update_sha256_matches(digest, want_hash.c_str())) {
        self_update::remove_tree(zip_path);
        return fail("the download's checksum does not match");
    }

    // 3. Unpack the app folder beside the install, on the same drive.
    const std::string staged = std::string(kWork) + "/new-" + kTitle;
    self_update::remove_tree(staged);
    self_update::ArchiveInfo info;
    std::string error;
    if (!self_update::inspect_archive(zip_path, kTitle, info, error)) {
        log_line("inspect: %s", error.c_str());
        return fail("the ZIP does not hold the app");
    }
    notify("Installing ProsperoEden (%zu files)...", info.files);
    std::atomic<bool> cancelled{false};
    std::atomic<std::uint64_t> written{0};
    if (!self_update::extract_archive(zip_path, kTitle, staged, cancelled, written, error)) {
        log_line("extract: %s", error.c_str());
        self_update::remove_tree(staged);
        return fail("unpacking failed");
    }
    if (!exists(staged + "/eboot.bin")) {
        self_update::remove_tree(staged);
        return fail("the unpacked app has no eboot.bin");
    }

    // 4. Old folder to the backups, new folder in its place.
    mkdir(kBackups, 0777);
    const std::string backup = std::string(kBackups) + "/" + base_name(target) + "-" + stamp();
    const bool had_old = exists(target);
    if (had_old && !self_update::move_tree(target, backup)) {
        self_update::remove_tree(staged);
        return fail("the installed app could not be moved to the backups");
    }
    if (!self_update::move_tree(staged, target)) {
        if (had_old) self_update::move_tree(backup, target);
        self_update::remove_tree(staged);
        return fail("the new app could not be put in place; the old one was restored");
    }
    self_update::remove_tree(zip_path);
    write_text(installed_file, want_hash + "\n");
    log_line("installed %s at %s; previous version in %s", zip_name.c_str(), target.c_str(),
             had_old ? backup.c_str() : "(none)");

    if (exists(kLink))
        notify("ProsperoEden updated. Open it from the home screen.");
    else
        notify("ProsperoEden updated. Run ShadowMountPlus once so it appears on the home screen.");
    if (log_file) std::fclose(log_file);
    return 0;
}
