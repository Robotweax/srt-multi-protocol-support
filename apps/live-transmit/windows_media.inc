// Included inside live_transmit's private namespace.
struct SocketDescriptor {
    SOCKET value = INVALID_SOCKET;
    ~SocketDescriptor() { if (value != INVALID_SOCKET) closesocket(value); }
    void nonblocking() {
        u_long enabled = 1;
        if (ioctlsocket(value, FIONBIO, &enabled)) system_error("Nonblocking UDP socket");
    }
    void wait(bool input, std::chrono::milliseconds timeout) const {
        WSAPOLLFD event{value, static_cast<short>(input ? POLLRDNORM : POLLWRNORM), 0};
        if (WSAPoll(&event, 1, static_cast<int>(timeout.count())) == SOCKET_ERROR) system_error("UDP poll");
    }
};
bool would_block() { return WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINTR; }

class FileMedium final : public Medium {
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    bool input_, borrowed_, pipe_;
public:
    FileMedium(const EndpointOptions& endpoint, bool input) : input_(input), borrowed_(endpoint.path == "-") {
        if (borrowed_) handle_ = GetStdHandle(input ? STD_INPUT_HANDLE : STD_OUTPUT_HANDLE);
        else {
            handle_ = CreateFileW(std::filesystem::path(endpoint.path).c_str(), input ? GENERIC_READ : GENERIC_WRITE,
                FILE_SHARE_READ, nullptr, input ? OPEN_EXISTING : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        if (!handle_ || handle_ == INVALID_HANDLE_VALUE) throw std::system_error(GetLastError(), std::system_category(), "Open media file");
        const auto type = GetFileType(handle_);
        pipe_ = type == FILE_TYPE_PIPE;
        if (!borrowed_ && (type != FILE_TYPE_DISK || (!input && !SetEndOfFile(handle_)))) {
            CloseHandle(handle_);
            throw std::runtime_error("Media path must be a regular file accessible for the selected operation");
        }
    }
    ~FileMedium() override { if (!borrowed_) CloseHandle(handle_); }
    ReadResult read(std::span<std::byte> buffer) override {
        DWORD count = 0;
        if (pipe_) {
            DWORD available = 0;
            if (!PeekNamedPipe(handle_, nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) return {ReadResult::State::eof};
                throw std::system_error(GetLastError(), std::system_category(), "Inspect input pipe");
            }
            if (!available) return {ReadResult::State::would_block};
        }
        if (!ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) return {ReadResult::State::eof};
            throw std::system_error(GetLastError(), std::system_category(), "Read media");
        }
        return {count ? ReadResult::State::packet : ReadResult::State::eof, count};
    }
    Write write(std::span<const std::byte> bytes) override {
        while (!bytes.empty()) {
            DWORD count = 0;
            if (!WriteFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || !count)
                throw std::system_error(GetLastError(), std::system_category(), "Write media");
            bytes = bytes.subspan(count);
        }
        return Write::done;
    }
    bool pausable() const override { return input_; }
    void wait(std::chrono::milliseconds timeout) override { if (pipe_) std::this_thread::sleep_for(timeout); }
};
