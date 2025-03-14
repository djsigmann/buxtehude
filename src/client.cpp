#include "client.hpp"

#include "server.hpp"
#include "core.hpp"
#include "tb.hpp"

#include <fmt/core.h>

namespace buxtehude
{

Client::~Client()
{
    Close();
}

Client::Client(const ClientPreferences& preferences) : preferences(preferences) {}

// Connection setup functions

tb::error<ConnectError> Client::IPConnect(std::string_view hostname, uint16_t port)
{
    if (setup) return tb::ok_t {};

    addrinfo* res;
    addrinfo hints {
        .ai_flags = AI_DEFAULT, .ai_family = PF_INET,
        .ai_socktype = SOCK_STREAM
    };

    if (int gai_error = getaddrinfo(hostname.data(), nullptr, &hints, &res)) {
        logger(LogLevel::WARNING,
            fmt::format("Failed to connect to address {}: getaddrinfo failed: {}",
                hostname, gai_strerror(gai_error)));
        return ConnectError { ConnectErrorType::GETADDRINFO_ERROR, gai_error };
    }

    tb::scoped_guard addrinfo_guard = [res] () { freeaddrinfo(res); };

    client_socket = socket(res->ai_family, res->ai_socktype, 0);
    sockaddr_in* addr = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    addr->sin_port = htons(port);

    if (connect(client_socket, reinterpret_cast<sockaddr*>(addr), sizeof(sockaddr_in))) {
        logger(LogLevel::WARNING, fmt::format("Failed to connect to address {}: {}",
            hostname, strerror(errno)));
        return ConnectError { ConnectErrorType::CONNECT_ERROR, errno };
    }

    if (SetupEvents().is_error())
        return ConnectError { ConnectErrorType::LIBEVENT_ERROR, -1 };

    conn_type = ConnectionType::INTERNET;

    if (Handshake().is_error())
        return ConnectError { ConnectErrorType::WRITE_ERROR, -1 };

    return tb::ok_t {};
}

tb::error<ConnectError> Client::UnixConnect(std::string_view path)
{
    if (setup) return tb::ok_t {};

    client_socket = socket(PF_LOCAL, SOCK_STREAM, 0);
    if (client_socket == -1) {
        return ConnectError { ConnectErrorType::SOCKET_ERROR, errno };
    }

    sockaddr_un addr;
    addr.sun_family = AF_LOCAL;

    size_t path_len = path.size() < sizeof(addr.sun_path) - 1 ?
        path.size() : sizeof (addr.sun_path) - 1;
    memcpy(addr.sun_path, path.data(), path_len);
    addr.sun_path[path_len] = '\0';

    if (connect(client_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(sockaddr_un))) {
        logger(LogLevel::WARNING, fmt::format("Failed to connect to file {}: {}",
            path, strerror(errno)));
        return ConnectError { ConnectErrorType::CONNECT_ERROR, errno };
    }

    if (SetupEvents().is_error())
        return ConnectError { ConnectErrorType::LIBEVENT_ERROR, -1 };

    conn_type = ConnectionType::UNIX;

    if (Handshake().is_error())
        return ConnectError { ConnectErrorType::WRITE_ERROR, -1 };

    return tb::ok_t {};
}

void Client::InternalConnect(Server& server)
{
    if (setup) return;
    server_ptr = &server;
    server.AddClient(*this);
    setup = true;
    conn_type = ConnectionType::INTERNAL;

    [[maybe_unused]] auto _ = Handshake(); // Internal handshakes do not have a fail path
}

// General functions applicable to all types of Client

tb::error<WriteError> Client::Write(const Message& msg)
{
    if (conn_type == ConnectionType::INTERNAL) {
        server_ptr->Receive(*this, msg);
    } else if (!Message::WriteToStream(stream.file, msg, preferences.format)) {
        logger(LogLevel::WARNING, "Failed to write - closing connection");
        Close();
        return WriteError {};
    }

    return tb::ok_t {};
}

tb::error<WriteError> Client::Handshake()
{
    bool success = Write({
        .type { MSG_HANDSHAKE },
        .content = {
            { "format", preferences.format },
            { "teamname", preferences.teamname },
            { "version", CURRENT_VERSION },
            { "max-message-length", preferences.max_msg_length }
        }
    }).is_ok();

    if (!success) return WriteError {};

    AddHandler(MSG_HANDSHAKE, [] (Client& c, const Message& m) {
        if (!ValidateJSON(m.content, VALIDATE_HANDSHAKE_CLIENTSIDE)) {
            logger(LogLevel::WARNING, "Rejected server handshake - disconnecting");
            c.Close();
            return;
        }

        c.EraseHandler(std::string { MSG_HANDSHAKE });
    });

    AddHandler(MSG_ERROR, [] (Client& c, const Message& m) {
        if (!ValidateJSON(m.content, VALIDATE_SERVER_MESSAGE)) {
            logger(LogLevel::WARNING, "Erroneous server message");
            return;
        }

        logger(LogLevel::INFO, fmt::format("Error message from server: {}",
               m.content.get<std::string>()));
    });

    return tb::ok_t {};
}

tb::error<WriteError> Client::SetAvailable(std::string_view type, bool available)
{
    return Write({
        .type { MSG_AVAILABLE },
        .content = {
            { "type", type },
            { "available", available }
        }
    });
}

void Client::HandleMessage(const Message& msg)
{
    if (msg.type.empty()) {
        logger(LogLevel::WARNING, "Received message with no type!");
        return;
    }

    if (handlers.contains(msg.type)) handlers[msg.type](*this, msg);
}

// Handlers

void Client::AddHandler(std::string_view type, Handler&& h)
{
    handlers.emplace(type, std::forward<Handler>(h));
}

void Client::EraseHandler(const std::string& type) { handlers.erase(type); }

void Client::ClearHandlers() { handlers.clear(); }

// Start & stop client

void Client::Run()
{
    if (!setup) {
        logger(LogLevel::WARNING, "Tried to run client before setup!");
        return;
    }
    run = true;

    if (stream.file) {
        std::thread t(&Client::Listen, this);
        current_thread = std::move(t);
    }

    while (!ingress.empty()) {
        HandleMessage(ingress.front());
        ingress.pop();
    }
}

void Client::Close()
{
    if (!run) {
        if (current_thread.joinable()
            && std::this_thread::get_id() != current_thread.get_id())
            current_thread.join();
        return;
    }
    run = false;
    logger(LogLevel::DEBUG, "Closing client");

    if (stream.file) {
        event_active(interrupt_event.get(), 0, 0);
        if (current_thread.joinable()
            && std::this_thread::get_id() != current_thread.get_id())
            current_thread.join();

        fclose(stream.file);
    }

    if (server_ptr && conn_type == ConnectionType::INTERNAL) {
        server_ptr->RemoveClient(*this);
    }
}

// INTERNAL clients only

void Client::Receive(const Message& msg)
{
    if (!run) ingress.push(msg);
    else HandleMessage(msg);
}

// Socket-based connections only

tb::error<AllocError> Client::SetupEvents()
{
    ebase = make<UEventBase>(event_base_new());
    callback_data.ebase = ebase.get();

    read_event = make<UEvent>(
        event_new(ebase.get(), client_socket, EV_PERSIST | EV_READ,
                  callbacks::ReadCallback, reinterpret_cast<void*>(&callback_data))
    );

    interrupt_event = make<UEvent>(
        event_new(ebase.get(), -1, EV_PERSIST,
                  callbacks::LoopInterruptCallback, &callback_data)
    );

    if (!ebase || !read_event || !interrupt_event) {
        logger(LogLevel::WARNING, "Failed to create one or more libevent structures");
        return AllocError {};
    }

    event_add(read_event.get(), &callbacks::DEFAULT_TIMEOUT);

    stream.file = fdopen(client_socket, "r+");
    stream.Await<uint8_t>().Await<uint32_t>().Then([this] (Stream& stream, Field& f) {
        auto type = f[-1].Get<MessageFormat>();
        if (type != MessageFormat::JSON && type != MessageFormat::MSGPACK) {
            stream.Reset();
            logger(LogLevel::WARNING, "Invalid message type!");
            return;
        }
        auto size = f.Get<uint32_t>();
        if (size > preferences.max_msg_length) {
            stream.Reset();
            logger(LogLevel::WARNING, "Buffer size too big!");
            return;
        }
        stream.Await(size);
    });

    preferences.format = MessageFormat::MSGPACK;
    setup = true;
    return tb::ok_t {};
}

void Client::Read()
{
    if (!stream.Read()) {
        if (stream.Status() == StreamStatus::REACHED_EOF) Close();
        return;
    }

    std::string_view data = stream[2].GetView();

    try {
        HandleMessage(Message::Deserialise(stream[0].Get<MessageFormat>(), data));
    } catch (const json::parse_error& e) {
        logger(LogLevel::WARNING, fmt::format("Error parsing message: {}", e.what()));
    }

    stream.Delete(stream[2]);
    stream.Reset();
}

void Client::Listen()
{
    while (event_base_dispatch(ebase.get()) == 0) {
        if (!run) break;
        switch (callback_data.type) {
        case EventType::READ_READY:
            Read();
        case EventType::INTERRUPT: return;
        default:
            break;
        }
    }
}

}
