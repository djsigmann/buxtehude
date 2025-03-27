#include "server.hpp"

#include <fmt/core.h>

#include "client.hpp"
#include "tb.hpp"

#include <ranges>

#include <unistd.h>

namespace buxtehude
{

ClientHandle::ClientHandle(Client& iclient, std::string_view teamname)
    : client_ptr(&iclient), conn_type(ConnectionType::INTERNAL), connected(true)
{
    preferences.teamname = teamname;
}

ClientHandle::ClientHandle(ConnectionType conn_type, FILE* ptr, uint32_t max_msg_len)
    : conn_type(conn_type)
{
    stream.file = ptr;
    stream.Await<MessageFormat>().Await<uint32_t>()
          .Then([this, max_msg_len] (Stream& s, Field& f) {
        auto type = f[-1].Get<MessageFormat>();
        if (type != MessageFormat::JSON && type != MessageFormat::MSGPACK) {
            s.Reset();
            Error("Invalid message type!");
            return;
        }
        auto size = f.Get<uint32_t>();
        if (size > max_msg_len) {
            s.Reset();
            Error("Buffer size too big!");
            return;
        }
        s.Await(size);
    });

    socket = fileno(ptr);
    connected = true;
    if (Handshake().is_error()) Disconnect_NoWrite();
}

ClientHandle::~ClientHandle() { Disconnect(); }

// Common ClientHandle functions

tb::error<WriteError> ClientHandle::Handshake()
{
    return Write({
        .type { MSG_HANDSHAKE },
        .content = {
            { "version", CURRENT_VERSION }
        }
    });
}

tb::error<WriteError> ClientHandle::Write(const Message& msg)
{
    if (!connected) return WriteError {};

    if (conn_type == ConnectionType::INTERNAL) {
        client_ptr->Internal_Receive(msg);
    } else if (!Message::WriteToStream(stream.file, msg, preferences.format)) {
        return WriteError {};
    }

    return tb::ok;
}

void ClientHandle::Error(std::string_view errstr)
{
    if (time(nullptr) - last_error < 1) return;
    last_error = time(nullptr);

    bool success = Write({ .type { MSG_ERROR }, .content = errstr }).is_ok();
    if (!handshaken || !success) Disconnect("Failed handshake");
}

void ClientHandle::Disconnect(std::string_view reason)
{
    if (!connected) return;
    Write({
        .type { MSG_DISCONNECT },
        .content = {
            { "reason", reason },
            { "who", MSG_YOU }
        }
    }).ignore_error();
    Disconnect_NoWrite();
}

void ClientHandle::Disconnect_NoWrite()
{
    if (!connected) return;
    if (conn_type == ConnectionType::UNIX || conn_type == ConnectionType::INTERNET) {
        fclose(stream.file);
    } else if (conn_type == ConnectionType::INTERNAL) {
        client_ptr->Internal_Disconnect();
    }
    logger(LogLevel::DEBUG, fmt::format("Disconnecting client {}",
        preferences.teamname));
    connected = false;
}

bool ClientHandle::Available(std::string_view type)
{
    return std::ranges::find(unavailable, type) == unavailable.end();
}

// ClientHandle functions specific to stream-based connections

tb::result<Message, ReadError> ClientHandle::Read()
{
    if (!stream.Read()) {
        if (stream.Status() == StreamStatus::REACHED_EOF) {
            Disconnect();
            return { ReadError::CONNECTION_ERROR };
        } else {
            return { ReadError::INCOMPLETE_MESSAGE };
        }
    }

    std::string_view data = stream[2].GetView();
    tb::scoped_guard reset_stream = [this] {
        stream.Delete(stream[2]);
        stream.Reset();
    };

    try {
        return { Message::Deserialise(stream[0].Get<MessageFormat>(), data) };
    } catch (const json::parse_error& e) {
        std::string error = fmt::format("Error parsing message from {}: {}",
            preferences.teamname, e.what());
        logger(LogLevel::WARNING, error);
        Error(error);
    }

    return { ReadError::PARSE_ERROR };
}

// Server
// Server constructors & destructor

Server::~Server()
{
    Close();
}

// Listening socket setup

tb::error<ListenError> Server::UnixServer(std::string_view path)
{
    if (SetupEvents().is_error())
        return ListenError { ListenErrorType::LIBEVENT_ERROR };

    sockaddr_un addr;
    addr.sun_family = PF_LOCAL;

    size_t path_len = path.size() < sizeof(addr.sun_path) - 1 ?
        path.size() : sizeof(addr.sun_path) - 1;
    memcpy(addr.sun_path, path.data(), path_len);
    addr.sun_path[path_len] = '\0';

    unix_path = addr.sun_path;

    unix_listener = make<UEvconnListener>(
        evconnlistener_new_bind(ebase.get(), callbacks::ConnectionCallback,
                                &callback_data, LEV_OPT_CLOSE_ON_FREE,
                                -1, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))
    );

    if (!unix_listener) {
        logger(LogLevel::WARNING,
            fmt::format("Failed to listen for UNIX domain connections at {}: {}",
                path, strerror(errno)));
        unix_server = -1;
        return ListenError { ListenErrorType::BIND_ERROR, errno };
    }

    unix_server = evconnlistener_get_fd(unix_listener.get());

    Run();
    logger(LogLevel::DEBUG, fmt::format("Listening on file {}", path));

    return tb::ok;
}

tb::error<ListenError> Server::IPServer(uint16_t port)
{
    if (SetupEvents().is_error())
        return ListenError { ListenErrorType::LIBEVENT_ERROR };

    sockaddr_in addr = {0};
    addr.sin_family = PF_INET;
    addr.sin_port = htons(port);

    if (INADDR_ANY) addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Passing -1 as the backlog allows libevent to try select an optimal backlog number.
    ip_listener = make<UEvconnListener>(
        evconnlistener_new_bind(ebase.get(), callbacks::ConnectionCallback,
                                &callback_data,
                                LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE, -1,
                                reinterpret_cast<sockaddr*>(&addr), sizeof(addr))
    );

    if (!ip_listener) {
        logger(LogLevel::WARNING,
            fmt::format("Failed to listen for internet domain connections on port {}: {}",
                port, strerror(errno)));
        ip_server = -1;
        return ListenError { ListenErrorType::BIND_ERROR, errno };
    }

    ip_server = evconnlistener_get_fd(ip_listener.get());

    Run();
    logger(LogLevel::DEBUG, fmt::format("Listening on port {}", port));

    return tb::ok;
}

tb::error<AllocError> Server::InternalServer()
{
    if (SetupEvents().is_error()) return AllocError {};

    Run();
    return tb::ok;
}

// Server initialisation & threaded logic

void Server::Run()
{
    if (run) return;
    run = true;

    if (current_thread.joinable()) {
        event_active(interrupt_event.get(), 0, 0);
        current_thread.join();
    }

    std::thread t(&Server::Listen, this);
    current_thread = std::move(t);
}

void Server::Close()
{
    logger(LogLevel::DEBUG, "Shutting down server");
    if (current_thread.joinable()) {
        event_active(interrupt_event.get(), 0, 0);
        current_thread.join();
    }

    if (unix_listener)
        unlink(unix_path.c_str());
}

void Server::Broadcast(const Message& m)
{
    std::vector<ClientHandle*> all = GetClients(MSG_ALL);
    for (ClientHandle* cl : all) {
        if (cl->Write(m).is_error()) cl->Disconnect_NoWrite();
    }
}

// Server connection management
// INTERNAL only functions

void Server::Internal_AddClient(Client& cl)
{
    std::lock_guard<std::mutex> guard(clients_mutex);
    auto& ch = clients.emplace_back(
        std::make_unique<ClientHandle>(cl, cl.preferences.teamname)
    );
    if (ch->Handshake().is_error()) ch->Disconnect_NoWrite();
}

void Server::Internal_RemoveClient(Client& cl)
{
    { // Lock guard
    std::lock_guard<std::mutex> guard(clients_mutex);
    std::erase_if(clients, [&cl] (auto& unique_ptr) {
        return unique_ptr->client_ptr == &cl;
    });
    } // Exit lock guard

    Broadcast({
        .type { MSG_DISCONNECT },
        .content = {
            { "who", cl.preferences.teamname }
        }
    });
}

void Server::Internal_ReceiveFrom(Client& cl, const Message& msg)
{
    std::lock_guard<std::mutex> guard(internal_mutex);
    internal_messages.emplace_back(&cl, msg);
    event_active(read_internal_event.get(), 0, 0);
}

// Reading from socket-based clients

void Server::Serve(ClientHandle* ch)
{
    tb::result<Message, ReadError> message = ch->Read();
    if (message.is_ok()) HandleMessage(ch, std::move(message.get_mut_unchecked()));

    if (!ch->connected) {
        std::string teamname = std::move(ch->preferences.teamname);
        {
        std::lock_guard<std::mutex> guard(clients_mutex);
        std::erase_if(clients, [ch] (auto& unique_ptr) {
            return unique_ptr.get() == ch;
        });
        }
        Broadcast({
            .type { MSG_DISCONNECT },
            .content = {
                { "who", ch->preferences.teamname }
            }
        });
    }
}

void Server::HandleMessage(ClientHandle* ch, Message&& msg)
{
    // Types of the JSON values are validated in checks
    if (!ch->handshaken) {
        if (msg.type != MSG_HANDSHAKE ||
            !ValidateJSON(msg.content, VALIDATE_HANDSHAKE_SERVERSIDE)) {
            ch->Disconnect("Failed handshake");
            return;
        }

        ch->preferences.teamname = msg.content["teamname"];
        ch->preferences.format = msg.content["format"];
        ch->preferences.max_msg_length = msg.content["max-message-length"];
        ch->handshaken = true;
        return;
    }

    if (msg.type == MSG_AVAILABLE) {
        if (!ValidateJSON(msg.content, VALIDATE_AVAILABLE)) {
            ch->Error("Incorrect format for $$available message");
            return;
        }
        std::string type = msg.content["type"];
        bool available = msg.content["available"];
        auto it = std::ranges::find(ch->unavailable, type);
        if (available) {
            if (it != ch->unavailable.end()) ch->unavailable.erase(it);
        } else {
            if (it == ch->unavailable.end()) ch->unavailable.emplace_back(type);
        }
    }

    if (msg.dest.empty()) return;

    msg.src = ch->preferences.teamname;
    if (msg.only_first) {
        ClientHandle* destination = GetFirstAvailable(msg.dest, msg.type, ch);
        if (destination) {
            if (destination->Write(msg).is_error()) destination->Disconnect_NoWrite();
        }
        return;
    }

    std::vector<ClientHandle*> recipients = GetClients_NoLock(msg.dest);
    for (ClientHandle* destination : recipients) {
        if (destination != ch) {
            if (destination->Write(msg).is_error()) destination->Disconnect_NoWrite();
        }
    }
}

// Libevent setup

tb::error<AllocError> Server::SetupEvents()
{
    if (ebase) return tb::ok;

    ebase = make<UEventBase>(event_base_new());
    callback_data.ebase = ebase.get();

    interrupt_event = make<UEvent>(
        event_new(ebase.get(), -1, EV_PERSIST, callbacks::LoopInterruptCallback,
                  &callback_data)
    );

    read_internal_event = make<UEvent>(
        event_new(ebase.get(), -1, 0, callbacks::InternalReadCallback, &callback_data)
    );

    if (!ebase || !interrupt_event || !read_internal_event) {
        logger(LogLevel::WARNING, "Failed to allocate one or more libevent structures");
        return AllocError {};
    }

    return tb::ok;
}

void Server::Listen()
{
    while (event_base_loop(ebase.get(), EVLOOP_NO_EXIT_ON_EMPTY) == 0) {
        switch (callback_data.type) {
        case EventType::NEW_CONNECTION: {
            std::lock_guard<std::mutex> guard(clients_mutex);
            AddConnection(callback_data.fd, callback_data.address.sa_family);
            break;
        }
        case EventType::READ_READY: {
            ClientHandle* ch = nullptr;
            {
            std::lock_guard<std::mutex> guard(clients_mutex);
            Server::HandleIter iter = GetClientBySocket(callback_data.fd);
            if (iter == clients.end()) break;
            ch = iter->get();
            }

            Serve(ch);

            break;
        }
        case EventType::TIMEOUT: {
            Server::HandleIter iter = GetClientBySocket(callback_data.fd);
            if (iter == clients.end()) break;
            ClientHandle* ch = iter->get();

            if (!ch->handshaken) ch->Disconnect("Failed handshake");
            break;
        }
        case EventType::INTERNAL_READ_READY: {
            event_del(read_internal_event.get());
            std::vector<std::pair<Client*, Message>> messages;
            {
                std::lock_guard<std::mutex> guard(internal_mutex);
                messages = std::move(internal_messages);
            }
            std::lock_guard<std::mutex> guard(clients_mutex);
            for (auto& [client_ptr, message] : messages) {
                Server::HandleIter iter = GetClientByPointer(client_ptr);
                if (iter == clients.end()) continue;
                HandleMessage((*iter).get(), std::move(message));
            }
            break;
        }
        case EventType::INTERRUPT:
            return;
        }
    }
}

void Server::AddConnection(int fd, sa_family_t addr_family)
{
    FILE* stream = fdopen(fd, "r+");

    ConnectionType conn_type;
    std::string_view debug_string;

    switch (addr_family) {
    case AF_LOCAL:
        conn_type = ConnectionType::UNIX;
        evconnlistener_enable(unix_listener.get());
        debug_string = "UNIX";
        break;
    case AF_INET:
    default:
        conn_type = ConnectionType::INTERNET;
        evconnlistener_enable(ip_listener.get());
        debug_string = "internet";
        break;
    }

    auto& cl = clients.emplace_back(
        std::make_unique<ClientHandle>(conn_type, stream, max_msg_length)
    );

    cl->read_event = make<UEvent>(
        event_new(ebase.get(), fd, EV_PERSIST | EV_READ,
                  callbacks::ReadCallback, &callback_data)
    );

    event_add(cl->read_event.get(), &callbacks::DEFAULT_TIMEOUT);

    logger(LogLevel::DEBUG,
        fmt::format("New client connected on {} domain, fd = {}", debug_string, fd));
}

// ClientHandle iteration

std::vector<ClientHandle*> Server::GetClients(std::string_view team)
{
    std::lock_guard<std::mutex> guard(clients_mutex);
    return GetClients_NoLock(team);
}

Server::HandleIter Server::GetClientBySocket(int fd)
{
    auto iter = std::ranges::find_if(clients,
        [fd] (auto& unique_pointer) {
            return unique_pointer->socket == fd;
        }
    );

    if (iter == clients.end())
        logger(LogLevel::WARNING,
            fmt::format("No client with file descriptor {} found", fd));

    return iter;
}

Server::HandleIter Server::GetClientByPointer(Client* ptr)
{
    auto iter = std::ranges::find_if(clients,
        [ptr] (auto& unique_pointer) {
            return unique_pointer->client_ptr == ptr;
        }
    );

    if (iter == clients.end())
        logger(LogLevel::WARNING,
            fmt::format("No client with pointer {} found",
                        static_cast<void*>(ptr)));

    return iter;
}

ClientHandle* Server::GetFirstAvailable(std::string_view team,
                                        std::string_view type,
                                        const ClientHandle* exclude)
{
    ClientHandle* result = nullptr;

    for (auto it = clients.begin(); it != clients.end(); ++it) {
        ClientHandle* ch = it->get();
        if ((ch->preferences.teamname == team || team == MSG_ALL) && ch != exclude)
            result = ch;
        else continue;
        if (ch->Available(type)) return result;
    }

    return result;
}

std::vector<ClientHandle*> Server::GetClients_NoLock(std::string_view team)
{
    if (team == MSG_ALL) return clients | tb::ptr_vec<ClientHandle>();

    return clients | std::views::filter([team] (const auto& client) {
        return client->preferences.teamname == team;
    }) | tb::ptr_vec<ClientHandle>();
}

}
