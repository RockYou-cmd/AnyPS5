#include "SceTypes.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

extern "C" {
int APS5_VABI sceNetInit_nid_postfix();
int APS5_VABI sceNetSocket(const char*, int, int, int);
int APS5_VABI sceNetBind_nid_postfix(int, const void*, std::uint32_t);
int APS5_VABI sceNetListen(int, int);
int APS5_VABI sceNetGetsockname(int, void*, std::uint32_t*);
int APS5_VABI sceNetConnect(int, const void*, std::uint32_t);
int APS5_VABI sceNetAccept(int, void*, std::uint32_t*);
int APS5_VABI sceNetSocketClose(int);
int APS5_VABI sceNetSetsockopt(int, int, int, const void*, std::uint32_t);
int APS5_VABI sceNetGetsockopt(int, int, int, void*, std::uint32_t*);
std::int64_t APS5_VABI sceNetRecv(int, void*, std::size_t, int);
int* APS5_VABI sceNetErrnoLoc();
int APS5_VABI sceNetEpollCreate(const char*, int);
int APS5_VABI sceNetEpollControl(int, int, int, const NetEpollEvent*);
int APS5_VABI sceNetEpollWait(int, NetEpollEvent*, int, int);
int APS5_VABI sceNetEpollDestroy(int);
}

static void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

static bool Failed(std::int64_t result, int error) {
    return result == static_cast<int>(0x80410100u | static_cast<unsigned>(error)) && *sceNetErrnoLoc() == error;
}

struct BoundSocket {
    int descriptor;
    std::array<std::uint8_t, 16> address;
};

static BoundSocket BindLoopback() {
    BoundSocket socket{sceNetSocket("connect-test", 2, 1, 6), {16, 2, 0, 0, 127, 0, 0, 1}};
    Require(socket.descriptor >= 0, "create bound socket");
    Require(sceNetBind_nid_postfix(socket.descriptor, socket.address.data(), socket.address.size()) == 0, "bind loopback");
    std::uint32_t size = socket.address.size();
    Require(sceNetGetsockname(socket.descriptor, socket.address.data(), &size) == 0 && size == socket.address.size(), "get bound address");
    return socket;
}

static int NonblockingSocket() {
    const int socket = sceNetSocket("connect-test", 2, 1, 6);
    Require(socket >= 0, "create client socket");
    const int enabled = 1;
    Require(sceNetSetsockopt(socket, 0xffff, 0x1200, &enabled, sizeof(enabled)) == 0, "enable nonblocking");
    return socket;
}

static std::uint32_t WaitForConnection(int socket) {
    const int epoll = sceNetEpollCreate("connect-test", 0);
    Require(epoll >= 0, "create epoll");
    NetEpollEvent registration{};
    registration.events = 4;
    registration.ident = static_cast<std::uint64_t>(socket);
    Require(sceNetEpollControl(epoll, 1, socket, &registration) == 0, "register connection");
    NetEpollEvent ready{};
    const int count = sceNetEpollWait(epoll, &ready, 1, 5000000);
    Require(sceNetEpollDestroy(epoll) == 0, "destroy epoll");
    Require(count == 1 && ready.ident == static_cast<std::uint64_t>(socket), "connection readiness within five seconds");
    return ready.events;
}

static void CheckSocketError(int socket, int expected, const char* phase) {
    std::array<int, 2> output{-1, 12345};
    std::uint32_t size = sizeof(output);
    *sceNetErrnoLoc() = 123;
    const int result = sceNetGetsockopt(socket, 0xffff, 0x1007, output.data(), &size);
    std::fprintf(stderr, "SO_ERROR [%s]: result=0x%08x, value=%d, expected=%d, length=%u, guest errno=%d\n",
        phase, static_cast<unsigned>(result), output[0], expected, size, *sceNetErrnoLoc());
    Require(result == 0, "query SO_ERROR");
    Require(output[0] == expected, "SO_ERROR guest error value");
    Require(output[1] == 12345 && size == sizeof(int), "SO_ERROR output bounds and length");
    Require(*sceNetErrnoLoc() == 123, "SO_ERROR success preserves thread errno");
}

static void CheckInvalidArguments(int socket) {
    int value = -1;
    std::uint32_t size = sizeof(value);
    Require(Failed(sceNetGetsockopt(socket, 0xffff, 0x1007, nullptr, &size), 22), "SO_ERROR null output");
    Require(size == sizeof(value), "null output preserves length");
    Require(Failed(sceNetGetsockopt(socket, 0xffff, 0x1007, &value, nullptr), 22), "SO_ERROR null length");
    size = sizeof(value) - 1;
    Require(Failed(sceNetGetsockopt(socket, 0xffff, 0x1007, &value, &size), 22), "SO_ERROR short output");
    Require(size == sizeof(value) - 1 && value == -1, "short output is unchanged");
    size = sizeof(value);
    Require(Failed(sceNetGetsockopt(-1, 0xffff, 0x1007, &value, &size), 9), "SO_ERROR invalid descriptor");
    Require(Failed(sceNetGetsockopt(socket, 0, 0x1007, &value, &size), 45), "SO_ERROR unsupported level");
    Require(Failed(sceNetGetsockopt(socket, 0xffff, 0x7fffffff, &value, &size), 45), "unsupported socket option");
    Require(value == -1 && size == sizeof(value), "invalid query preserves output");
}

static void CheckSuccessfulConnection() {
    const auto listener = BindLoopback();
    Require(sceNetListen(listener.descriptor, 1) == 0, "listen");
    const int client = NonblockingSocket();
    const int result = sceNetConnect(client, listener.address.data(), listener.address.size());
    Require(result == 0 || Failed(result, 36), "nonblocking connect reports success or EINPROGRESS");
    Require((WaitForConnection(client) & 4) != 0, "connected socket becomes writable");
    CheckInvalidArguments(client);
    CheckSocketError(client, 0, "connected, first read");
    CheckSocketError(client, 0, "connected, second read");
    const int accepted = sceNetAccept(listener.descriptor, nullptr, nullptr);
    Require(accepted >= 0, "accept connected client");
    char value = 0;
    Require(Failed(sceNetRecv(client, &value, sizeof(value), 0), 35), "empty nonblocking receive still reports EAGAIN");
    Require(sceNetSocketClose(accepted) == 0, "close accepted socket");
    Require(sceNetSocketClose(client) == 0, "close connected client");
    Require(sceNetSocketClose(listener.descriptor) == 0, "close listener");
    std::fprintf(stderr, "PASS: nonblocking success, SO_ERROR arguments, and receive EAGAIN\n");
}

#ifdef _WIN32
static void CheckConnectionRetry(int client, const BoundSocket& refused) {
    const int retry = sceNetConnect(client, refused.address.data(), refused.address.size());
    if (Failed(retry, 36)) {
        Require((WaitForConnection(client) & (4 | 8 | 16)) != 0, "retried refusal becomes ready");
        std::array<int, 2> errors{-1, -1};
        const auto readError = [&](int index) {
            std::uint32_t size = sizeof(int);
            Require(sceNetGetsockopt(client, 0xffff, 0x1007, &errors[index], &size) == 0,
                "concurrent SO_ERROR query");
            Require(size == sizeof(int), "concurrent SO_ERROR length");
        };
        std::thread first(readError, 0);
        std::thread second(readError, 1);
        first.join();
        second.join();
        std::fprintf(stderr, "SO_ERROR [retry, concurrent reads]: values=%d,%d, expected=61,0 in either order\n",
            errors[0], errors[1]);
        Require((errors[0] == 61 && errors[1] == 0) || (errors[0] == 0 && errors[1] == 61),
            "a new connection error is consumed exactly once");
        CheckSocketError(client, 0, "retry, third read");
        std::fprintf(stderr, "PASS: same-socket refusal retry and concurrent read-and-clear\n");
    } else {
        Require(Failed(retry, 61), "retried connect reports ECONNREFUSED");
        std::fprintf(stderr, "PASS: immediate retry refusal (concurrent SO_ERROR path not exercised)\n");
    }
    const auto listener = BindLoopback();
    Require(sceNetListen(listener.descriptor, 1) == 0, "listen for retry");
    const int connected = sceNetConnect(client, listener.address.data(), listener.address.size());
    Require(connected == 0 || Failed(connected, 36), "retry to listening endpoint starts");
    Require((WaitForConnection(client) & 4) != 0, "retry connects to listening endpoint");
    CheckSocketError(client, 0, "successful retry, first read");
    CheckSocketError(client, 0, "successful retry, second read");
    const int accepted = sceNetAccept(listener.descriptor, nullptr, nullptr);
    Require(accepted >= 0, "accept retried client");
    char value = 0;
    Require(Failed(sceNetRecv(client, &value, sizeof(value), 0), 35), "retried connection receive EAGAIN");
    Require(sceNetSocketClose(accepted) == 0, "close accepted retry");
    Require(sceNetSocketClose(listener.descriptor) == 0, "close retry listener");
    std::fprintf(stderr, "PASS: same-socket retry connects successfully\n");
}
#endif

static void CheckRefusedConnection() {
    const auto reserved = BindLoopback();
    const int client = NonblockingSocket();
    Require(sceNetSocketClose(reserved.descriptor) == 0, "release refused endpoint");
    const int result = sceNetConnect(client, reserved.address.data(), reserved.address.size());
    if (Failed(result, 36)) {
        Require((WaitForConnection(client) & (4 | 8 | 16)) != 0, "refused connection becomes ready");
        CheckInvalidArguments(client);
        CheckSocketError(client, 61, "refused, first read");
        CheckSocketError(client, 0, "refused, second read");
        std::fprintf(stderr, "PASS: asynchronous refusal, error translation, and read-and-clear\n");
    } else {
        Require(Failed(result, 61), "immediate refusal reports ECONNREFUSED");
        std::fprintf(stderr, "PASS: immediate refusal (asynchronous SO_ERROR path not exercised)\n");
    }
#ifdef _WIN32
    CheckConnectionRetry(client, reserved);
#endif
    Require(sceNetSocketClose(client) == 0, "close refused client");
}

int main() {
    Require(sceNetInit_nid_postfix() == 0, "initialize networking");
    CheckSuccessfulConnection();
    CheckRefusedConnection();
}
