/*
FxSound - single-instance command forwarding for Linux.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/
/*
 * JUCE detects an already-running FxSound with an InterProcessLock, but on
 * Linux JUCE 6 cannot forward the new command line to it
 * (MessageManager::broadcastMessage() is unimplemented), so commands such as
 * `FxSound --power off` or `FxSound --status` would silently do nothing.
 *
 * This replaces the transport with a Unix domain socket in $XDG_RUNTIME_DIR:
 *  - the running instance accepts one connection per command on a background
 *    thread, runs the command on the message thread, and writes back any
 *    output (used by --status);
 *  - a newly launched instance connects, sends its command line, prints the
 *    reply and exits.
 *
 * Delivery is confirmed: the reply starts with ACCEPTED, or with BUSY if the
 * running instance is already quitting. Anything else (refused connection,
 * no reply) means "not delivered", and the new instance retries/takes over.
 */
#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include <optional>
#include <future>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>

class FxInstanceChannel
{
public:
    // Runs `command` on the message thread; returns text to send back, or
    // nullopt to refuse it (e.g. the app is quitting).
    using Handler = std::function<std::optional<String>(const String& command)>;
    static constexpr char ACCEPTED = '\x01';
    static constexpr char BUSY = '\x02';

    static std::string socketPath()
    {
        auto runtime = SystemStats::getEnvironmentVariable("XDG_RUNTIME_DIR", {});
        if (runtime.isNotEmpty() && File(runtime).isDirectory())
            return (runtime + "/fxsound.sock").toStdString();
        return "/tmp/fxsound-" + std::to_string((int)getuid()) + ".sock";
    }

    // Server: call only while holding the single-instance lock.
    explicit FxInstanceChannel(Handler handler) : handler_(std::move(handler))
    {
        auto path = socketPath();
        sockaddr_un addr {};
        if (path.size() >= sizeof(addr.sun_path)) return;
        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        unlink(path.c_str());   // stale socket from a crashed instance; we hold the lock
        fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return;
        if (bind(fd_, (sockaddr*)&addr, sizeof(addr)) != 0 || ::listen(fd_, 4) != 0) {
            close(fd_); fd_ = -1; return;
        }
        chmod(path.c_str(), 0600);
        struct stat st {};
        if (stat(path.c_str(), &st) == 0) { inode_ = st.st_ino; device_ = st.st_dev; }
        thread_ = std::thread([this] { serve(); });
    }

    ~FxInstanceChannel()
    {
        quit_ = true;
        if (fd_ >= 0) shutdown(fd_, SHUT_RDWR);   // wakes accept()
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) {
            close(fd_);
            // Only remove the path if it is still our socket: a newer instance
            // may already have bound a fresh socket there while we shut down.
            auto path = socketPath();
            struct stat st {};
            if (stat(path.c_str(), &st) == 0 && st.st_ino == inode_ && st.st_dev == device_)
                unlink(path.c_str());
        }
    }

    // Client: returns true only if a running instance accepted the command line.
    static bool forward(const String& commandline, String& reply)
    {
        auto path = socketPath();
        sockaddr_un addr {};
        if (path.size() >= sizeof(addr.sun_path)) return false;
        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return false;
        if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) { close(fd); return false; }
        std::string out = commandline.toStdString();
        bool ok = writeAll(fd, out) && shutdown(fd, SHUT_WR) == 0;
        std::string in = ok ? readAll(fd, 5000) : std::string();
        close(fd);
        if (in.empty() || in[0] != ACCEPTED)
            return false;   // refused, busy (quitting) or no answer
        reply = String::fromUTF8(in.c_str() + 1);
        return true;
    }

private:
    void serve()
    {
        while (!quit_) {
            int c = accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (c < 0) { if (quit_) break; Thread::sleep(50); continue; }
            String command = String::fromUTF8(readAll(c, 2000).c_str());
            // Run on the message thread and wait for the result.
            auto done = std::make_shared<std::promise<std::optional<String>>>();
            auto result = done->get_future();
            MessageManager::callAsync([this, done, command] {
                std::optional<String> text;
                if (!quit_ && !MessageManager::getInstance()->hasStopMessageBeenSent())
                    text = handler_(command);
                done->set_value(text);
            });
            if (result.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
                auto text = result.get();
                writeAll(c, text ? std::string(1, ACCEPTED) + text->toStdString() : std::string(1, BUSY));
            }
            close(c);
        }
    }

    static bool writeAll(int fd, const std::string& data)
    {
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
            if (n <= 0) return false;
            off += (size_t)n;
        }
        return true;
    }

    static std::string readAll(int fd, int timeout_ms)
    {
        std::string data;
        char buf[4096];
        for (;;) {
            pollfd p { fd, POLLIN, 0 };
            if (poll(&p, 1, timeout_ms) <= 0) break;
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            data.append(buf, (size_t)n);
            if (data.size() > (1 << 20)) break;
        }
        return data;
    }

    Handler handler_;
    int fd_ = -1;
    ino_t inode_ = 0;
    dev_t device_ = 0;
    std::atomic<bool> quit_ { false };
    std::thread thread_;
};
