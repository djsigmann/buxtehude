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
    const static char* LEVEL_NAMES[] = { "DEBUG", "INFO", "WARNING", "SEVERE" };
    fmt::print("[{}] {}\n", LEVEL_NAMES[l], message);
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
        logger((LogLevel) severity, msg);
    });

    // UNIX domain connections being closed sends signal SIGPIPE to the process trying to
    // read from this socket. Not intercepting this signal would kill the process.
    struct sigaction sighandle {};
    sighandle.sa_handler = sigh ? sigh : SIG_IGN;
    sigaction(SIGPIPE, &sighandle, nullptr);
}

// Message struct functions

std::string Message::Serialise(MessageFormat f) const
{
    json json_obj(*this);

    switch (f) {
    default:
    case JSON:
        return json_obj.dump();
    case MSGPACK: {
        std::string data;
        // Despite the presence of \0 characters, to_msgpack will store all bytes of the
        // MsgPack representation in the string object, and a call to size() will
        // return the correct number of bytes.
        // Choosing std::string as the return value simplifies this function and
        // eliminates the need for unnecessary copy operations.
        json::to_msgpack(json_obj, data);
        return data;
    }
    }
}

Message Message::Deserialise(MessageFormat f, std::string_view data)
{
    switch (f) {
    case JSON:
        return json::parse(data).get<Message>();
    case MSGPACK:
        return json::from_msgpack(data).get<Message>();
    }
}

bool Message::WriteToStream(FILE* stream, const Message& message, MessageFormat f)
{
    std::string data = message.Serialise(f);
    uint32_t len = data.size();
    uint8_t format = f;
    fwrite(&format, sizeof(uint8_t), 1, stream);
    fwrite(&len, sizeof(uint32_t), 1, stream);
    fwrite(data.data(), len, 1, stream);
    return !fflush(stream);
}

namespace callbacks
{

void ConnectionCallback(evconnlistener* listener, evutil_socket_t fd,
    sockaddr* addr, int addr_len, void* data)
{
    EventCallbackData* ecdata = (EventCallbackData*) data;

    ecdata->fd = fd;
    ecdata->address = *addr;
    ecdata->addr_len = addr_len;
    ecdata->type = NEW_CONNECTION;

    event_base_loopbreak(ecdata->ebase);
}

void ReadCallback(evutil_socket_t fd, short what, void* data)
{
    EventCallbackData* ecdata = (EventCallbackData*) data;

    ecdata->fd = fd;
    if (what & EV_READ) ecdata->type = READ_READY;
    else if (what & EV_TIMEOUT) ecdata->type = TIMEOUT;

    event_base_loopbreak(ecdata->ebase);
}

void LoopInterruptCallback(evutil_socket_t fd, short what, void* data)
{
    EventCallbackData* ecdata = (EventCallbackData*) data;
    ecdata->type = INTERRUPT;
    event_base_loopbreak(ecdata->ebase);
}

}

}
