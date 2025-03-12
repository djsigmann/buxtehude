#include "server.hpp"

#include <fmt/core.h>

#include "client.hpp"
#include "tb.hpp"

#include <ranges>

#include <unistd.h>

namespace buxtehude
{

ClientHandle::ClientHandle(Client& iclient, std::string_view teamname)
    : teamname(teamname), client_ptr(&iclient), atype(INTERNAL), connected(true)
{}

ClientHandle::ClientHandle(AddressType a, FILE* ptr, uint32_t max_msg_len) : atype(a)
{
    stream.file = ptr;
    stream.Await<uint8_t>().Await<uint32_t>()
          .Then([this, max_msg_len] (Stream& s, Field& f) {
        auto type = f[-1].Get<uint8_t>();
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

    Handshake();
}

ClientHandle::~ClientHandle() { Disconnect(); }

// Common ClientHandle functions

void ClientHandle::Handshake()
{
    Write({
        .type { MSG_HANDSHAKE },
        .content = {
            { "version", CURRENT_VERSION }
        }
    });
}

void ClientHandle::Write(const Message& msg)
{
    if (!connected) return;

    if (atype == INTERNAL) {
        client_ptr->Receive(msg);
        return;
    }

    if (!Message::WriteToStream(stream.file, msg, preferences.format))
        Disconnect_NoWrite();
}

void ClientHandle::Error(std::string_view errstr)
{
    if (time(nullptr) - last_error < 1) return;
    last_error = time(nullptr);

    Write({ .type { MSG_ERROR }, .content = errstr });
    if (!handshaken) Disconnect("Failed handshake");
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
    });
    Disconnect_NoWrite();
}

void ClientHandle::Disconnect_NoWrite()
{
    if (!connected) return;
    if (atype == UNIX || atype == INTERNET) {
        fclose(stream.file);
    } else {
        client_ptr->Close();
    }
    logger(DEBUG, fmt::format("Disconnecting client {}", teamname));
    connected = false;
}

bool ClientHandle::Available(std::string_view type)
{
    return std::ranges::find(unavailable, type) == unavailable.end();
}

// ClientHandle functions specific to stream-based connections

std::optional<Message> ClientHandle::Read()
{
    if (!stream.Read()) {
        if (stream.Status() == REACHED_EOF) Disconnect();
        return {};
    }

    std::string_view data = stream[2].GetView();

    // Neither of these change the data on their own, data is still safe to use
    stream.Delete(stream[2]);
    stream.Reset();

    try {
        return { Message::Deserialise((MessageFormat)stream[0].Get<char>(), data) };
    } catch (const json::parse_error& e) {
        std::string error = fmt::format("Error parsing message from {}: {}", teamname,
            e.what());
        logger(WARNING, error);
        Error(error);
    }

    return {};
}

// Server
// Server constructors & destructor

Server::Server(std::string_view path)
{
    UnixServer(path);
}

Server::Server(uint16_t port)
{
    IPServer(port);
}

Server::~Server()
{
    Close();
}

// Listening socket setup

bool Server::UnixServer(std::string_view path)
{
    sockaddr_un addr;
    addr.sun_family = PF_LOCAL;

    size_t path_len = path.size() < sizeof(addr.sun_path) - 1 ?
        path.size() : sizeof (addr.sun_path) - 1;
    memcpy(addr.sun_path, path.data(), path_len);
    addr.sun_path[path_len] = '\0';

    unix_path = addr.sun_path;

    if (!SetupEvents()) return false;

    unix_listener = make<UEvconnListener>(
        evconnlistener_new_bind(ebase.get(), callbacks::ConnectionCallback,
                                &callback_data, LEV_OPT_CLOSE_ON_FREE,
                                -1, (sockaddr*)&addr, sizeof(addr))
    );

    if (!unix_listener) {
        logger(WARNING,
            fmt::format("Failed to listen for UNIX domain connections at {}: {}",
                path, strerror(errno)));
        unix_server = -1;
        return false;
    }

    unix_server = evconnlistener_get_fd(unix_listener.get());
    logger(DEBUG, fmt::format("Listening on file {}", path));

    return true;
}

bool Server::IPServer(uint16_t port)
{
    sockaddr_in addr = {0};
    addr.sin_family = PF_INET;
    addr.sin_port = htons(port);

    if (INADDR_ANY) addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (!SetupEvents()) return false;

    // Passing -1 as the backlog allows libevent to try select an optimal backlog number.
    ip_listener = make<UEvconnListener>(
        evconnlistener_new_bind(ebase.get(), callbacks::ConnectionCallback,
                                &callback_data,
                                LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE, -1,
                                (sockaddr*)&addr, sizeof(addr))
    );

    if (!ip_listener) {
        logger(WARNING,
            fmt::format("Failed to listen for internet domain connections on port {}: {}",
                port, strerror(errno)));
        ip_server = -1;
        return false;
    }

    ip_server = evconnlistener_get_fd(ip_listener.get());

    logger(DEBUG, fmt::format("Listening on port {}", port));

    return true;
}

// Server initialisation & threaded logic

void Server::Run()
{
    if (unix_listener || ip_listener) {
        std::thread t(&Server::Listen, this);
        current_thread = std::move(t);
    }
    run = true;
}

void Server::Close()
{
    if (!run) return;
    run = false;
    logger(DEBUG, "Shutting down server");
    if (interrupt_event && current_thread.joinable()) {
        event_active(interrupt_event.get(), 0, 0);
        current_thread.join();
    }

    {
    std::lock_guard<std::mutex> guard(clients_mutex);
    clients.clear();
    }

    unlink(unix_path.c_str());
}

void Server::Broadcast(const Message& m)
{
    for (ClientHandle* cl : GetClients(MSG_ALL)) cl->Write(m);
}

// Server connection management
// INTERNAL only functions

void Server::AddClient(Client& cl)
{
    std::lock_guard<std::mutex> guard(clients_mutex);
    auto& ch = clients.emplace_back(
        std::make_unique<ClientHandle>(cl, cl.preferences.teamname)
    );
    ch->Handshake();
}

void Server::RemoveClient(Client& cl)
{
    { // Lock guard
    std::lock_guard<std::mutex> guard(clients_mutex);
    std::erase_if(clients, [&cl] (auto& u_ptr) {
        return u_ptr->client_ptr == &cl;
    });
    } // Exit lock guard

    Broadcast({
        .type { MSG_DISCONNECT },
        .content = {
            { "who", cl.preferences.teamname }
        }
    });
}

void Server::Receive(Client& cl, const Message& msg)
{
    ClientHandle* ch = nullptr;
    {
    std::lock_guard<std::mutex> guard(clients_mutex);
    auto iter = GetClientByPointer(&cl);
    if (iter == clients.end()) return;
    ch = iter->get();
    }
    HandleMessage(ch, Message(msg));
}

// Reading from socket-based clients

void Server::Serve(ClientHandle* ch)
{
    std::optional<Message> message = ch->Read();
    if (message) HandleMessage(ch, std::move(message.value()));

    if (!ch->connected) {
        std::string teamname = std::move(ch->teamname);
        {
        std::lock_guard<std::mutex> guard(clients_mutex);
        std::erase_if(clients, [ch] (auto& unique_ptr) {
            return unique_ptr.get() == ch;
        });
        }
        Broadcast({
            .type { MSG_DISCONNECT },
            .content = {
                { "who", teamname }
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

        ch->teamname = msg.content["teamname"];
        ch->preferences.format = msg.content["format"];
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

    msg.src = ch->teamname;
    if (msg.only_first) {
        ClientHandle* destination = GetFirstAvailable(msg.dest, msg.type, ch);
        if (destination) destination->Write(msg);
        return;
    }

    for (ClientHandle* destination : GetClients_NoLock(msg.dest))
        if (destination != ch) destination->Write(msg);
}

// Libevent setup

bool Server::SetupEvents()
{
    if (ebase) return true;

    ebase = make<UEventBase>(event_base_new());
    callback_data.ebase = ebase.get();
    if (!ebase) {
        logger(WARNING, "Failed to create event base");
        return false;
    }

    interrupt_event = make<UEvent>(
        event_new(ebase.get(), -1, EV_PERSIST, callbacks::LoopInterruptCallback,
                  &callback_data)
    );

    return true;
}

void Server::Listen()
{
    while (event_base_dispatch(ebase.get()) == 0) {
        if (!run) break;
        switch (callback_data.type) {
        case NEW_CONNECTION: {
            std::lock_guard<std::mutex> guard(clients_mutex);
            AddConnection(callback_data.fd, callback_data.address.sa_family);
            break;
        }
        case READ_READY: {
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
        case TIMEOUT: {
            Server::HandleIter iter = GetClientBySocket(callback_data.fd);
            if (iter == clients.end()) break;
            ClientHandle* ch = iter->get();

            if (!ch->handshaken) ch->Disconnect("Failed handshake");
            break;
        }
        case INTERRUPT:
            return;
        }
    }
}

void Server::AddConnection(int fd, sa_family_t addr_family)
{
    FILE* stream = fdopen(fd, "r+");

    AddressType atype = addr_family == AF_LOCAL ? UNIX : INTERNET;
    auto& cl = clients.emplace_back(
        std::make_unique<ClientHandle>(atype, stream, max_msg_length)
    );

    cl->read_event = make<UEvent>(
        event_new(ebase.get(), fd, EV_PERSIST | EV_READ,
                  callbacks::ReadCallback, (void*)&callback_data)
    );

    event_add(cl->read_event.get(), &callbacks::DEFAULT_TIMEOUT);

    logger(DEBUG, fmt::format("New client connected on {} domain, fd = {}",
           atype == UNIX ? "UNIX" : "internet", fd));
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
        logger(WARNING, fmt::format("No client with file descriptor {} found", fd));

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
        logger(WARNING, fmt::format("No client with pointer {} found", (void*)ptr));

    return iter;
}

ClientHandle* Server::GetFirstAvailable(std::string_view team,
                                        std::string_view type,
                                        const ClientHandle* exclude)
{
    ClientHandle* result = nullptr;

    for (auto it = clients.begin(); it != clients.end(); ++it) {
        ClientHandle* ch = it->get();
        if ((ch->teamname == team || team == MSG_ALL) && ch != exclude)
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
        return client->teamname == team;
    }) | tb::ptr_vec<ClientHandle>();
}

}
