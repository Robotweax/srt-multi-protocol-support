// Independent pinned-Haivision reproducer. No host or adapter code is linked.
// Exit 0: complete transfer; 1: incomplete transfer; 2: setup failure.
#include <srt.h>
#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
void check(bool ok) { if (!ok) throw std::runtime_error(srt_getlasterror_str()); }
int main() {
    std::array<SRTSOCKET, 3> sockets{SRT_INVALID_SOCK, SRT_INVALID_SOCK, SRT_INVALID_SOCK};
    int result = 2;
    if (srt_startup() != 0) return result;
    try {
        sockets[0] = srt_create_socket(); sockets[1] = srt_create_socket();
        for (auto socket : {sockets[0], sockets[1]}) {
            check(socket != SRT_INVALID_SOCK);
            auto set = [socket](SRT_SOCKOPT opt, auto value) { check(srt_setsockflag(socket, opt, &value, sizeof(value)) == 0); };
            set(SRTO_TRANSTYPE, SRTT_FILE); set(SRTO_MESSAGEAPI, false);
            set(SRTO_SNDSYN, false); set(SRTO_RCVSYN, false);
            set(SRTO_MSS, 1500); set(SRTO_SNDBUF, 8*1024*1024); set(SRTO_RCVBUF, 8*1024*1024);
            set(SRTO_LINGER, linger{});
        }
        sockaddr_in6 address{}; address.sin6_family = AF_INET6; address.sin6_addr = in6addr_loopback;
        check(srt_bind(sockets[0], reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        check(srt_listen(sockets[0], 8) == 0); int size = sizeof(address);
        check(srt_getsockname(sockets[0], reinterpret_cast<sockaddr*>(&address), &size) == 0);
        check(srt_connect(sockets[1], reinterpret_cast<sockaddr*>(&address), size) == 0);
        auto deadline = Clock::now() + 5s;
        while (sockets[2] == SRT_INVALID_SOCK || srt_getsockstate(sockets[1]) != SRTS_CONNECTED) {
            if (Clock::now() >= deadline) throw std::runtime_error("Connection setup timed out");
            if (sockets[2] == SRT_INVALID_SOCK) sockets[2] = srt_accept(sockets[0], nullptr, nullptr);
            std::this_thread::sleep_for(1ms);
        }
        std::array<char, 32768> buffer{}; int sent = 0, received = 0;
        deadline = Clock::now() + 10s;
        while (received < int(buffer.size()) && Clock::now() < deadline) {
            if (sent < int(buffer.size())) {
                int n = srt_send(sockets[1], buffer.data()+sent, int(buffer.size())-sent);
                if (n > 0) sent += n;
            }
            int n = srt_recv(sockets[2], buffer.data(), int(buffer.size()));
            if (n > 0) received += n;
            std::this_thread::sleep_for(1ms);
        }
        std::cout << "sent=" << sent << " received=" << received << " caller_state=" << srt_getsockstate(sockets[1])
                  << " accepted_state=" << srt_getsockstate(sockets[2]) << '\n';
        result = received == int(buffer.size()) ? 0 : 1;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; }
    for (auto socket : sockets) if (socket != SRT_INVALID_SOCK) srt_close(socket);
    srt_cleanup(); return result;
}
