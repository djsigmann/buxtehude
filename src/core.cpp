#include "core.hpp"

#include <fmt/core.h>
#include <event2/thread.h>

#include <signal.h>

namespace buxtehude
{

// JSON conversion functions

void to_json(json& j, const Message& msg)
{
    j = { { "type", msg.type }, { "only_first", msg.only_first } };
    if (!msg.dest.empty()) j["dest"] = msg.dest;
    if (!msg.src.empty()) j["src"] = msg.src;
    if (!msg.content.empty()) j["content"] = msg.content;
}

void from_json(const json& j, Message& msg)
{
    if (j.contains("dest")) j["dest"].get_to(msg.dest);
    if (j.contains("src")) j["src"].get_to(msg.src);
    if (j.contains("type")) j["type"].get_to(msg.type);
    if (j.contains("only_first")) j["only_first"].get_to(msg.only_first);
    if (j.contains("content")) j["content"].get_to(msg.content);
}

// Logging & Library initialisation

void DefaultLog(LogLevel l, std::string_view message)
{
    constexpr static std::string_view LEVEL_NAMES[] = {
        "DEBUG", "INFO", "WARNING", "SEVERE"
    };
    fmt::print("[{}] {}\n", LEVEL_NAMES[static_cast<size_t>(l)], message);
}

void Initialise(LogCallback logcb, SignalHandler sigh)
{
#ifdef EVTHREAD_USE_PTHREADS_IMPLEMENTED
    evthread_use_pthreads();
#elif EVTHREAD_USE_WINDOWS_THREADS_IMPLEMENTED
    evthread_use_windows_threads();
#else
    static_assert(0,
        "Buxtehude requires Libevent to have been built with thread support");
#endif

    logger = logcb ? logcb : DefaultLog;
    event_set_log_callback([] (int severity, const char* msg) {
        logger(static_cast<LogLevel>(severity), msg);
    });

    // UNIX domain connections being closed sends signal SIGPIPE to the process trying to
    // read from this socket. Not intercepting this signal would kill the process.
    struct sigaction sighandle {};
    sighandle.sa_handler = sigh ? sigh : SIG_IGN;
    sigaction(SIGPIPE, &sighandle, nullptr);
}

// Message struct functions

Message Message::Deserialise(MessageFormat f, std::string_view data)
{
    switch (f) {
    case MessageFormat::JSON:
        return json::parse(data).get<Message>();
    case MessageFormat::MSGPACK:
        return json::from_msgpack(data).get<Message>();
    }
}

auto Message::WriteToStream(Stream& stream, const Message& message, MessageFormat f)
-> tb::error<int>
{
    constexpr size_t HEADER_SIZE = sizeof(uint32_t) + sizeof(MessageFormat);

    json object = message;
    switch (f) {
    case MessageFormat::JSON: {
        // nlohmann JSON does not offer a function for parsing JSON & writing through
        // an output adapter.
        std::string data = object.dump();
        uint32_t msg_len = data.size();

        data.insert(data.begin(), HEADER_SIZE, '\0');
        memcpy(data.data(), &f, sizeof(MessageFormat));
        memcpy(data.data() + sizeof(MessageFormat), &msg_len, sizeof(uint32_t));

        return stream.TryWrite(data);
    }
    case MessageFormat::MSGPACK: {
        std::vector<uint8_t> data;
        data.reserve(1024);
        data.insert(data.begin(), HEADER_SIZE, '\0');
        json::to_msgpack(object, data);

        uint32_t msg_len = data.size() - HEADER_SIZE;
        memcpy(data.data(), &f, sizeof(MessageFormat));
        memcpy(data.data() + sizeof(MessageFormat), &msg_len, sizeof(uint32_t));

        return stream.TryWrite(data);
    }
    }
}

namespace callbacks
{

void ConnectionCallback(evconnlistener* listener, evutil_socket_t fd,
    sockaddr* addr, int addr_len, void* data)
{
    auto* ecdata = static_cast<EventCallbackData*>(data);

    ecdata->fd = fd;
    ecdata->address = *addr;
    ecdata->addr_len = addr_len;
    ecdata->type = EventType::NEW_CONNECTION;

    event_base_loopbreak(ecdata->ebase);

    // If there is a queue of connections, these callbacks will run in succession
    // even with a call to event_base_loopbreak(). Disabling the listener will
    // break out of the listener accept(2) loop. The listener is re-enabled after
    // accepting the connection in Server::AddConnection().
    evconnlistener_disable(listener);
}

void ReadWriteCallback(evutil_socket_t fd, short what, void* data)
{
    auto* ecdata = static_cast<EventCallbackData*>(data);

    ecdata->fd = fd;
    if (what & EV_READ) ecdata->type = EventType::READ_READY;
    else if (what & EV_WRITE) ecdata->type = EventType::WRITE_READY;
    else if (what & EV_TIMEOUT) ecdata->type = EventType::TIMEOUT;

    event_base_loopbreak(ecdata->ebase);
}

void LoopInterruptCallback(evutil_socket_t fd, short what, void* data)
{
    auto* ecdata = static_cast<EventCallbackData*>(data);
    ecdata->type = EventType::INTERRUPT;
    event_base_loopbreak(ecdata->ebase);
}

void InternalReadCallback(evutil_socket_t fd, short what, void* data)
{
    auto* ecdata = static_cast<EventCallbackData*>(data);
    ecdata->type = EventType::INTERNAL_READ_READY;
    event_base_loopbreak(ecdata->ebase);
}

}

}
