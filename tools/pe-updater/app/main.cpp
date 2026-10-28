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
 *
 * The boilerplate links only the header-only parts of the C++ library, so this file uses fixed
 * buffers and no threads: each redraw (four a second) does the next small piece of the work.
 */

#include "demo_renderer.hpp"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
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
constexpr int kSocketLevel = 0xffff;
constexpr int kSendTimeout = 0x1105, kReceiveTimeout = 0x1106, kConnectTimeout = 0x1109;
// sceNetRecv's answer when the receive timeout passed with nothing to read.
constexpr int kNetWouldBlock = static_cast<int>(0x80410123);

enum class Stage
{
    start,
    reading,
    done
};

Stage stage = Stage::start;
int connection = -1;
char lines[kLines][kWidth + 1];
char headline[kWidth + 1] = "STARTING";
char pending[2048];
std::size_t pending_size = 0;

// The bitmap font has a space, 0 to 9 and A to Y; everything else is drawn as a space.
void printable(char (&out)[kWidth + 1], const char *text, std::size_t length)
{
    std::size_t n = 0;
    for (std::size_t i = 0; i < length && n < kWidth; ++i)
    {
        const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
        out[n++] = (up >= 'A' && up <= 'Y') || (up >= '0' && up <= '9') ? up : ' ';
    }
    out[n] = '\0';
}

void set_headline(const char *text)
{
    printable(headline, text, std::strlen(text));
}

void add_line(const char *text, std::size_t length)
{
    const char tag[] = "[pe-updater] ";
    if (length >= sizeof(tag) - 1 && std::memcmp(text, tag, sizeof(tag) - 1) == 0)
    {
        text += sizeof(tag) - 1;
        length -= sizeof(tag) - 1;
    }
    const char note[] = "notify: ";
    if (length >= sizeof(note) - 1 && std::memcmp(text, note, sizeof(note) - 1) == 0)
    {
        text += sizeof(note) - 1;
        length -= sizeof(note) - 1;
        printable(headline, text, length);
    }
    for (std::size_t i = 0; i + 1 < kLines; ++i)
        std::memcpy(lines[i], lines[i + 1], sizeof(lines[i]));
    printable(lines[kLines - 1], text, length);
}

void finish(const char *message)
{
    if (connection >= 0)
        (void)sceNetSocketClose(connection);
    connection = -1;
    if (message != nullptr)
        set_headline(message);
    stage = Stage::done;
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

// Connects to the ELF loader and hands it the bundled payload.
void start()
{
    const int helper = sceKernelOpen("/app0/pe-updater.elf", O_RDONLY, 0);
    if (helper < 0)
        return finish("THE UPDATER PAYLOAD IS MISSING FROM THIS APP");
    connection = sceNetSocket("pe_updater", 2, 1, 6);
    if (connection < 0)
    {
        (void)sceKernelClose(helper);
        return finish("NETWORK SOCKET FAILED");
    }
    const int long_wait = 10 * 1000 * 1000;
    (void)sceNetSetsockopt(connection, kSocketLevel, kSendTimeout, &long_wait, sizeof(long_wait));
    (void)sceNetSetsockopt(connection, kSocketLevel, kConnectTimeout, &long_wait, sizeof(long_wait));
    constexpr std::uint16_t port = 9021;
    const NetSockaddrIn address{sizeof(NetSockaddrIn), 2,
                                static_cast<std::uint16_t>((port << 8) | (port >> 8)), 0x0100007f,
                                0, {0}};
    if (sceNetConnect(connection, &address, sizeof(address)) < 0)
    {
        (void)sceKernelClose(helper);
        return finish("NO ELF LOADER ON PORT 9021  START IT THEN REOPEN THIS APP");
    }
    static std::uint8_t buffer[16384];
    for (;;)
    {
        const auto count = sceKernelRead(helper, buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0 || !send_all(connection, buffer, static_cast<std::size_t>(count)))
        {
            (void)sceKernelClose(helper);
            return finish("SENDING THE UPDATER FAILED");
        }
    }
    (void)sceKernelClose(helper);
    // From here each redraw reads what has arrived, waiting at most 50 ms.
    const int short_wait = 50 * 1000;
    (void)sceNetSetsockopt(connection, kSocketLevel, kReceiveTimeout, &short_wait, sizeof(short_wait));
    set_headline("UPDATER STARTED");
    stage = Stage::reading;
}

// Reads the payload's output, if any arrived, and turns whole lines into screen lines.
void read_progress()
{
    const int got = sceNetRecv(connection, pending + pending_size, sizeof(pending) - pending_size, 0);
    if (got == kNetWouldBlock || got == -1)
        return;
    if (got <= 0)
    {
        if (pending_size > 0)
            add_line(pending, pending_size);
        pending_size = 0;
        return finish(nullptr);
    }
    pending_size += static_cast<std::size_t>(got);
    std::size_t begin = 0;
    for (std::size_t i = 0; i < pending_size; ++i)
    {
        if (pending[i] == '\n')
        {
            add_line(pending + begin, i - begin);
            begin = i + 1;
        }
    }
    std::memmove(pending, pending + begin, pending_size - begin);
    pending_size -= begin;
    if (pending_size == sizeof(pending))
    {
        add_line(pending, pending_size);
        pending_size = 0;
    }
}

void draw_scene(ps5::demo::Canvas &canvas) noexcept
{
    using ps5::demo::Color;
    if (stage == Stage::start)
        start();
    else if (stage == Stage::reading)
        read_progress();

    canvas.clear(Color::background);
    canvas.text(120, 80, "PE UPDATER", 12, Color::white);
    canvas.text(120, 200, "PROSPEROEDEN TEST BUILDS FROM GITHUB", 4, Color::cyan);
    canvas.rectangle(120, 260, 1680, 6, Color::white);
    canvas.text(120, 300, headline, 4, Color::yellow);
    canvas.rectangle(120, 370, 1680, 600, Color::panel);
    for (std::size_t i = 0; i < kLines; ++i)
        canvas.text(150, static_cast<unsigned>(395 + i * 47), lines[i], 3, Color::white);
    if (stage == Stage::done)
        canvas.text(120, 1000, "DONE  PRESS THE PS BUTTON AND CLOSE THIS APP", 4, Color::cyan);
}
} // namespace

int main()
{
    ps5::demo::run(draw_scene, "PE Updater");
}
