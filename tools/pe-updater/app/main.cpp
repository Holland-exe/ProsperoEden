/*
 * PE Updater - a home screen app that updates ProsperoEden to the newest test build.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Built on ps5-native-app-boilerplate (BlackBearReloaded, GPL-3.0-or-later). The app runs in
 * the console's sandbox, so it does no file work itself: it hands the bundled payload
 * /app0/pe-updater.elf to the ELF loader on 127.0.0.1:9021 (as the boilerplate's
 * sandbox-elevation example does). The loader runs the payload with the same connection as its
 * output, so the payload's progress lines come back here and are shown on screen. The payload
 * also shows console notifications and writes /data/pe-updater/log.txt.
 */

#include "demo_renderer.hpp"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

namespace
{
struct NetSockaddrIn
{
    std::uint8_t length;
    std::uint8_t family;
    std::uint16_t port;
    std::uint32_t address;
    std::uint16_t virtual_port;
    std::uint8_t zero[6];
};

extern "C"
{
    int sceKernelOpen(const char *path, int flags, mode_t mode);
    int sceKernelClose(int descriptor);
    std::int64_t sceKernelRead(int descriptor, void *buffer, std::size_t length);
    int sceNetConnect(int socket, const void *address, std::uint32_t address_length);
    int sceNetSend(int socket, const void *data, std::size_t length, int flags);
    int sceNetRecv(int socket, void *data, std::size_t length, int flags);
    int sceNetSetsockopt(int socket, int level, int option, const void *value, std::uint32_t size);
    int sceNetSocket(const char *name, int domain, int type, int protocol);
    int sceNetSocketClose(int socket);
}

constexpr std::size_t kLines = 12;
constexpr std::size_t kWidth = 58;

std::mutex lock;
std::array<std::string, kLines> lines;
std::string headline = "STARTING";
bool finished = false;

// The bitmap font has a space, 0 to 9 and A to Y; everything else is drawn as a space.
std::string printable(const std::string &text)
{
    std::string out;
    for (const char c : text)
    {
        const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        out.push_back((up >= 'A' && up <= 'Y') || (up >= '0' && up <= '9') ? up : ' ');
        if (out.size() == kWidth)
            break;
    }
    return out;
}

void add_line(const std::string &text)
{
    std::string body = text;
    const std::string tag = "[pe-updater] ";
    if (body.rfind(tag, 0) == 0)
        body.erase(0, tag.size());
    if (body.rfind("notify: ", 0) == 0)
    {
        body.erase(0, 8);
        std::lock_guard guard{lock};
        headline = printable(body);
    }
    std::lock_guard guard{lock};
    for (std::size_t i = 0; i + 1 < kLines; ++i)
        lines[i] = lines[i + 1];
    lines[kLines - 1] = printable(body);
}

void set_headline(const char *text, bool done)
{
    std::lock_guard guard{lock};
    headline = printable(text);
    finished = done;
}

bool send_all(int socket, const std::uint8_t *data, std::size_t size)
{
    while (size > 0)
    {
        const int sent = sceNetSend(socket, data, size, 0);
        if (sent <= 0)
            return false;
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

void work()
{
    const int helper = sceKernelOpen("/app0/pe-updater.elf", O_RDONLY, 0);
    if (helper < 0)
    {
        set_headline("THE UPDATER PAYLOAD IS MISSING FROM THIS APP", true);
        return;
    }
    const int socket = sceNetSocket("pe_updater", 2, 1, 6);
    if (socket < 0)
    {
        (void)sceKernelClose(helper);
        set_headline("NETWORK SOCKET FAILED", true);
        return;
    }
    constexpr int socket_level = 0xffff;
    constexpr int timeout_us = 10 * 1000 * 1000;
    for (const int option : {0x1105, 0x1109}) // send, connect: the reply can take minutes
        (void)sceNetSetsockopt(socket, socket_level, option, &timeout_us, sizeof(timeout_us));
    constexpr std::uint16_t port = 9021;
    const NetSockaddrIn address{sizeof(NetSockaddrIn), 2,
                                static_cast<std::uint16_t>((port << 8) | (port >> 8)), 0x0100007f,
                                0, {0}};
    set_headline("STARTING THE UPDATER", false);
    if (sceNetConnect(socket, &address, sizeof(address)) < 0)
    {
        (void)sceNetSocketClose(socket);
        (void)sceKernelClose(helper);
        set_headline("NO ELF LOADER ON PORT 9021  START IT THEN REOPEN THIS APP", true);
        return;
    }
    std::array<std::uint8_t, 16384> buffer{};
    for (;;)
    {
        const auto count = sceKernelRead(helper, buffer.data(), buffer.size());
        if (count == 0)
            break;
        if (count < 0 || !send_all(socket, buffer.data(), static_cast<std::size_t>(count)))
        {
            (void)sceNetSocketClose(socket);
            (void)sceKernelClose(helper);
            set_headline("SENDING THE UPDATER FAILED", true);
            return;
        }
    }
    (void)sceKernelClose(helper);
    // The payload's output, line by line, until it ends and the loader closes the connection.
    std::string pending;
    char chunk[1024];
    for (;;)
    {
        const int got = sceNetRecv(socket, chunk, sizeof(chunk), 0);
        if (got <= 0)
            break;
        pending.append(chunk, static_cast<std::size_t>(got));
        for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n'))
        {
            add_line(pending.substr(0, end));
            pending.erase(0, end + 1);
        }
    }
    if (!pending.empty())
        add_line(pending);
    (void)sceNetSocketClose(socket);
    std::lock_guard guard{lock};
    finished = true;
}

void draw_scene(ps5::demo::Canvas &canvas) noexcept
{
    using ps5::demo::Color;
    std::lock_guard guard{lock};
    canvas.clear(Color::background);
    canvas.text(120, 80, "PE UPDATER", 12, Color::white);
    canvas.text(120, 200, "PROSPEROEDEN TEST BUILDS FROM GITHUB", 4, Color::cyan);
    canvas.rectangle(120, 260, 1680, 6, Color::white);
    canvas.text(120, 300, headline, 4, Color::yellow);
    canvas.rectangle(120, 370, 1680, 600, Color::panel);
    for (std::size_t i = 0; i < kLines; ++i)
        canvas.text(150, static_cast<unsigned>(395 + i * 47), lines[i], 3, Color::white);
    if (finished)
        canvas.text(120, 1000, "DONE  PRESS THE PS BUTTON AND CLOSE THIS APP", 4, Color::cyan);
}
} // namespace

int main()
{
    std::thread(work).detach();
    ps5::demo::run(draw_scene, "PE Updater");
}
