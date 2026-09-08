// The connection, against a receiver that is not a receiver.
//
// A stub that speaks real eISCP on a loopback port: enough to pin the four
// things the instance depends on and cannot check for itself - that a poll
// batch comes back whole, that a silent receiver costs the reply timeout and
// not the connect timeout, that reachable and answering are separate answers,
// and that a stream this adapter has lost its place in ends rather than hangs.

#include <phi/adapter/testing/check.h>

#include "onkyo_frames.h"
#include "onkyo_iscp_client.h"

#include "phi/runtime/epollloop.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace phicore::onkyo::ipc;
using namespace std::chrono_literals;

namespace {

/// A loopback listener that answers eISCP queries, or does not.
class StubReceiver
{
public:
    enum class Manner {
        Answer,      ///< a proper reply per query
        StaySilent,  ///< accepts the connection and says nothing
        TalkNonsense ///< a frame whose declared length cannot be believed
    };

    explicit StubReceiver(Manner manner) : m_manner(manner)
    {
        m_listen = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        const int one = 1;
        ::setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        m_ok = ::bind(m_listen, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0
            && ::listen(m_listen, 4) == 0;
        socklen_t length = sizeof(addr);
        if (m_ok && ::getsockname(m_listen, reinterpret_cast<sockaddr *>(&addr), &length) == 0)
            m_port = ::ntohs(addr.sin_port);
        m_thread = std::thread([this]() { serve(); });
    }

    ~StubReceiver()
    {
        m_done.store(true);
        ::shutdown(m_listen, SHUT_RDWR);
        ::close(m_listen);
        if (m_thread.joinable())
            m_thread.join();
    }

    bool ok() const { return m_ok; }
    std::uint16_t port() const { return m_port; }
    int queriesSeen() const { return m_queries.load(); }

private:
    /// The answer a receiver gives to each of the four queries the poll sends.
    static std::string answerFor(const std::string &command)
    {
        if (command.rfind("PWRQSTN", 0) == 0)
            return "PWR01";
        if (command.rfind("MVLQSTN", 0) == 0)
            return "MVL20";
        if (command.rfind("AMTQSTN", 0) == 0)
            return "AMT00";
        if (command.rfind("SLIQSTN", 0) == 0)
            return "SLI2E";
        return {};
    }

    void serve()
    {
        while (!m_done.load()) {
            const int client = ::accept(m_listen, nullptr, nullptr);
            if (client < 0)
                return;
            std::string buffer;
            for (;;) {
                char chunk[512];
                const ssize_t got = ::recv(client, chunk, sizeof(chunk), 0);
                if (got <= 0)
                    break;
                buffer.append(chunk, static_cast<std::size_t>(got));

                const FrameScan scan = decodeEiscpFrames(buffer);
                buffer.erase(0, scan.consumed);
                for (const std::string &payload : scan.payloads) {
                    ++m_queries;
                    if (m_manner == Manner::StaySilent)
                        continue;
                    if (m_manner == Manner::TalkNonsense) {
                        // "ISCP", a header size of zero, a payload size of zero:
                        // the frame that used to stop the scanner dead.
                        std::string junk = "ISCP";
                        junk.append(8, '\0');
                        junk.push_back(char(1));
                        junk.append(3, '\0');
                        ::send(client, junk.data(), junk.size(), MSG_NOSIGNAL);
                        continue;
                    }
                    // payload is "!1<command>\r"
                    const std::string command = payload.size() > 2 ? payload.substr(2) : payload;
                    const std::string answer = answerFor(command);
                    if (answer.empty())
                        continue;
                    const std::string frame = encodeEiscpFrame(answer);
                    ::send(client, frame.data(), frame.size(), MSG_NOSIGNAL);
                }
            }
            ::close(client);
        }
    }

    Manner m_manner;
    int m_listen = -1;
    bool m_ok = false;
    std::uint16_t m_port = 0;
    std::atomic_bool m_done{false};
    std::atomic_int m_queries{0};
    std::thread m_thread;
};

/// Runs one request to completion on a real loop and answers with its result.
std::optional<IscpResult> run(IscpRequest request, std::chrono::milliseconds budget = 8000ms)
{
    phi::runtime::EpollLoop loop;
    IscpClient client(loop);
    std::optional<IscpResult> result;

    const bool started = client.start(std::move(request), [&](IscpResult done) {
        result = std::move(done);
        loop.stop();
    });
    if (!started)
        return std::nullopt;

    phi::runtime::Timer watchdog = loop.timerAfter(budget, [&loop]() { loop.stop(); });
    loop.run();
    return result;
}

IscpRequest pollRequest(std::uint16_t port)
{
    IscpRequest request;
    request.hosts = {"127.0.0.1"};
    request.port = port;
    request.commands = {"PWRQSTN", "MVLQSTN", "AMTQSTN", "SLIQSTN"};
    request.replyTimeout = 500ms;
    request.connectTimeout = 1500ms;
    return request;
}

void testAPollBatchComesBackWhole()
{
    StubReceiver receiver(StubReceiver::Manner::Answer);
    PHI_CHECK(receiver.ok());

    const auto result = run(pollRequest(receiver.port()));
    PHI_CHECK_MSG(result.has_value(), "the request never finished");
    if (!result)
        return;
    PHI_CHECK(result->connected);
    PHI_CHECK_MSG(result->complete, "incomplete: %s", result->error.c_str());
    PHI_CHECK(result->host == "127.0.0.1");
    PHI_CHECK_MSG(receiver.queriesSeen() == 4, "receiver saw %d queries", receiver.queriesSeen());

    PHI_CHECK_MSG(result->statuses.size() == 4, "got %d statuses", int(result->statuses.size()));
    if (result->statuses.size() == 4) {
        PHI_CHECK(result->statuses[0].kind == StatusKind::Power && result->statuses[0].flag);
        PHI_CHECK(result->statuses[1].kind == StatusKind::Volume
                  && result->statuses[1].raw == 0x20);
        PHI_CHECK(result->statuses[2].kind == StatusKind::Mute && !result->statuses[2].flag);
        PHI_CHECK(result->statuses[3].kind == StatusKind::Input
                  && result->statuses[3].code == "2E");
    }
}

void testReachableAndAnsweringAreDifferentAnswers()
{
    // The receiver takes the connection and says nothing. Connectivity is a
    // fact about the network; whether the exchange worked is a different one,
    // and the instance reports them on different channels.
    StubReceiver receiver(StubReceiver::Manner::StaySilent);
    PHI_CHECK(receiver.ok());

    IscpRequest request = pollRequest(receiver.port());
    request.replyTimeout = 200ms;
    request.connectTimeout = 3000ms;

    const auto before = std::chrono::steady_clock::now();
    const auto result = run(request);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - before);

    PHI_CHECK(result.has_value());
    if (!result)
        return;
    PHI_CHECK_MSG(result->connected, "a completed TCP connect was not reported");
    PHI_CHECK(!result->complete);
    PHI_CHECK(!result->error.empty());
    // The silence costs one reply timeout, not the connect budget.
    PHI_CHECK_MSG(elapsed < 1500ms, "a silent receiver cost %lldms",
                  static_cast<long long>(elapsed.count()));
}

void testNothingListeningIsNotReachable()
{
    // Bind and close, so the port is one nothing answers on.
    std::uint16_t deadPort = 0;
    {
        StubReceiver probe(StubReceiver::Manner::Answer);
        PHI_CHECK(probe.ok());
        deadPort = probe.port();
    }

    const auto result = run(pollRequest(deadPort));
    PHI_CHECK(result.has_value());
    if (!result)
        return;
    PHI_CHECK_MSG(!result->connected, "an unreachable port reported a connection");
    PHI_CHECK(!result->complete);
    PHI_CHECK_MSG(result->error.find(std::to_string(deadPort)) != std::string::npos,
                  "the error does not name the endpoint: %s", result->error.c_str());
}

void testTheFirstCandidateThatAnswersIsUsed()
{
    std::uint16_t deadPort = 0;
    {
        StubReceiver probe(StubReceiver::Manner::Answer);
        deadPort = probe.port();
    }
    StubReceiver live(StubReceiver::Manner::Answer);
    PHI_CHECK(live.ok());

    IscpRequest request = pollRequest(live.port());
    // Same port for both entries would prove nothing; the dead one is reached
    // through a host that resolves and refuses.
    request.hosts = {"127.0.0.2", "127.0.0.1"};
    request.port = live.port();
    (void)deadPort;

    const auto result = run(request);
    PHI_CHECK(result.has_value());
    if (!result)
        return;
    PHI_CHECK_MSG(result->complete, "fell through both candidates: %s", result->error.c_str());
    PHI_CHECK(result->host == "127.0.0.1");
}

void testAStreamWeHaveLostOurPlaceInEndsRatherThanHangs()
{
    // The frame that used to put the instance thread into a loop it never left.
    // Here it has to come back, and say why.
    StubReceiver receiver(StubReceiver::Manner::TalkNonsense);
    PHI_CHECK(receiver.ok());

    IscpRequest request = pollRequest(receiver.port());
    request.replyTimeout = 300ms;

    const auto before = std::chrono::steady_clock::now();
    const auto result = run(request, 5000ms);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - before);

    PHI_CHECK_MSG(result.has_value(), "the request never finished - the scan stood still");
    if (!result)
        return;
    PHI_CHECK(result->connected);
    PHI_CHECK(!result->complete);
    PHI_CHECK_MSG(elapsed < 3000ms, "took %lldms to give up",
                  static_cast<long long>(elapsed.count()));
}

} // namespace

int main()
{
    testAPollBatchComesBackWhole();
    testReachableAndAnsweringAreDifferentAnswers();
    testNothingListeningIsNotReachable();
    testTheFirstCandidateThatAnswersIsUsed();
    testAStreamWeHaveLostOurPlaceInEndsRatherThanHangs();
    return phi::testing::report("onkyo_iscp_client_tests");
}
