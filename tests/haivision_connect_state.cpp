// Uses a private test-only core copy to pause the actual caller handshake after
// it clears m_bConnecting. The production module has no hook or pause.
#include <srt.h>
#ifndef _WIN32
#include <arpa/inet.h>
#endif
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace {
std::atomic<bool> armed{false}, entered{false}, resume{false}, expired{false};
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Session {
    SRTSOCKET listener = SRT_INVALID_SOCK, caller = SRT_INVALID_SOCK, accepted = SRT_INVALID_SOCK;
    Session() { require(srt_startup() == 0, "startup"); }
    ~Session() {
        resume = true;
        for (auto s : {accepted, caller, listener}) if (s != SRT_INVALID_SOCK) srt_close(s);
        srt_cleanup();
    }
};
}
extern "C" void srtmux_test_connect_transition() {
    if (!armed.exchange(false)) return;
    entered = true;
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (!resume.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    if (!resume.load()) expired = true;
}
int main() {
    try {
        Session session;
        session.listener = srt_create_socket(); session.caller = srt_create_socket();
        for (auto s : {session.listener, session.caller}) {
            const bool blocking = false;
            require(srt_setsockflag(s, SRTO_RCVSYN, &blocking, sizeof(blocking)) == 0, "nonblocking receive");
            require(srt_setsockflag(s, SRTO_SNDSYN, &blocking, sizeof(blocking)) == 0, "nonblocking send");
        }
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(srt_bind(session.listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind");
        require(srt_listen(session.listener, 8) == 0, "listen");
        int size = sizeof(address);
        require(srt_getsockname(session.listener, reinterpret_cast<sockaddr*>(&address), &size) == 0, "listener endpoint");
        armed = true;
        require(srt_connect(session.caller, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect");
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while (!entered.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        require(entered.load() && !expired.load(), "handshake did not reach paused transition hook");
        const auto during = srt_getsockstate(session.caller);
        resume = true;
        require(during == SRTS_CONNECTING || during == SRTS_CONNECTED, "successful handshake transiently reported broken");
        while (session.accepted == SRT_INVALID_SOCK && std::chrono::steady_clock::now() < deadline) {
            session.accepted = srt_accept(session.listener, nullptr, nullptr);
            if (session.accepted == SRT_INVALID_SOCK) {
                require(srt_getlasterror(nullptr) == SRT_EASYNCRCV, "accept");
                std::this_thread::yield();
            }
        }
        require(session.accepted != SRT_INVALID_SOCK, "accept deadline");
        while (srt_getsockstate(session.caller) == SRTS_CONNECTING && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        require(srt_getsockstate(session.caller) == SRTS_CONNECTED, "caller did not connect");
        std::array<char, 1316> input{}, output{}; input[0] = 42;
        require(srt_sendmsg(session.caller, input.data(), int(input.size()), -1, 0) == int(input.size()), "send");
        int received = -1;
        while (received < 0 && std::chrono::steady_clock::now() < deadline) {
            received = srt_recvmsg(session.accepted, output.data(), int(output.size()));
            if (received < 0) { require(srt_getlasterror(nullptr) == SRT_EASYNCRCV, "receive"); std::this_thread::yield(); }
        }
        require(received == int(input.size()) && input == output, "payload mismatch or deadline");
        std::cout << "Caller transition remains nonterminal and transfers payload\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
