#include "onkyo_probe.h"

#include <cerrno>
#include <cstring>

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "phi/runtime/str.h"

#include "onkyo_trace.h"

namespace phicore::onkyo::ipc {

namespace str = phi::str;

namespace {

/// Short enough that a cancelled probe stops promptly, long enough that the
/// wait is a wait and not a spin.
constexpr int kPollSliceMs = 50;

class Descriptor
{
public:
    explicit Descriptor(int fd) : m_fd(fd) {}
    ~Descriptor()
    {
        if (m_fd >= 0)
            ::close(m_fd);
    }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    int get() const { return m_fd; }

private:
    int m_fd;
};

} // namespace

bool probeEndpoint(const std::string &host,
                   std::uint16_t port,
                   std::chrono::milliseconds timeout,
                   const std::function<bool()> &cancelled,
                   std::string *errorMessage)
{
    const auto fail = [errorMessage](std::string message) {
        if (errorMessage)
            *errorMessage = std::move(message);
        return false;
    };

    timingLog(str::arg("factory.probe.start host=%1 port=%2", host, port));

    ::addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    ::addrinfo *resolved = nullptr;
    const std::string service = str::number(static_cast<int>(port));
    const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved);
    if (rc != 0 || resolved == nullptr)
        return fail(std::string("Cannot resolve host: ") + ::gai_strerror(rc));

    Descriptor socket(::socket(resolved->ai_family, resolved->ai_socktype | SOCK_NONBLOCK,
                               resolved->ai_protocol));
    if (socket.get() < 0) {
        const std::string error = std::strerror(errno);
        ::freeaddrinfo(resolved);
        return fail(error);
    }

    const int connectResult = ::connect(socket.get(), resolved->ai_addr, resolved->ai_addrlen);
    const int connectErrno = errno;
    ::freeaddrinfo(resolved);

    if (connectResult != 0 && connectErrno != EINPROGRESS)
        return fail(std::strerror(connectErrno));

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (connectResult != 0) {
        if (cancelled && cancelled())
            return fail("Probe cancelled: adapter is stopping");
        if (std::chrono::steady_clock::now() >= deadline)
            return fail("Connection timed out");

        ::pollfd waiting{};
        waiting.fd = socket.get();
        waiting.events = POLLOUT;
        const int ready = ::poll(&waiting, 1, kPollSliceMs);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return fail(std::strerror(errno));
        }
        if (ready == 0)
            continue;

        int soError = 0;
        ::socklen_t length = sizeof(soError);
        if (::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &soError, &length) != 0)
            return fail(std::strerror(errno));
        if (soError != 0)
            return fail(std::strerror(soError));
        break;
    }

    timingLog(str::arg("factory.probe.end status=success host=%1 port=%2", host, port));
    return true;
}

} // namespace phicore::onkyo::ipc
