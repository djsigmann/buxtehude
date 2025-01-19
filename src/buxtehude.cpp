#include "buxtehude.hpp"

#include <sys/errno.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <cstdio>
#include <unistd.h>
#include <string.h>
#include <signal.h>

#include <thread>

#include <event2/thread.h>

#include "validate.hpp"

namespace buxtehude
{

// Constants

const timeval DEFAULT_TIMEOUT = { 60 , 0 }; // 60s

// Validation tests

const ValidationPair VERSION_CHECK =
    { "/version"_json_pointer, predicates::GreaterEq<BUXTEHUDE_MINIMUM_COMPATIBLE> };

const ValidationSeries VALIDATE_HANDSHAKE_SERVERSIDE = {
    { "/teamname"_json_pointer, predicates::NotEmpty },
    { "/format"_json_pointer, predicates::Matches({ JSON, MSGPACK }) },
    VERSION_CHECK
};

const ValidationSeries VALIDATE_HANDSHAKE_CLIENTSIDE = { VERSION_CHECK };

const ValidationSeries VALIDATE_AVAILABLE = {
    { "/type"_json_pointer, predicates::NotEmpty },
    { "/available"_json_pointer, predicates::IsBool }
};

const ValidationSeries VALIDATE_SERVER_MESSAGE = {
    { ""_json_pointer, predicates::NotEmpty }
};

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

void DefaultLog(LogLevel l, const std::string& message)
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
    struct sigaction sighandle = {0};
    sighandle.sa_handler = sigh ? sigh : SIG_IGN;
    sigaction(SIGPIPE, &sighandle, nullptr);
}

// Libevent Callbacks

static void ConnectionCallback(evconnlistener* listener, evutil_socket_t fd,
    sockaddr* addr, int addr_len, void* data)
{
    EventCallbackData* ecdata = (EventCallbackData*) data;

    ecdata->fd = fd;
    ecdata->address = *addr;
    ecdata->addr_len = addr_len;
    ecdata->type = NEW_CONNECTION;

    event_base_loopbreak(ecdata->ebase);
}

static void ReadCallback(evutil_socket_t fd, short what, void* data)
{
    EventCallbackData* ecdata = (EventCallbackData*) data;

    ecdata->fd = fd;
    if (what & EV_READ) ecdata->type = READ_READY;
    else if (what & EV_TIMEOUT) ecdata->type = TIMEOUT;

    event_base_loopbreak(ecdata->ebase);
}

static void LoopInterruptCallback(evutil_socket_t fd, short what, void* data)
{
    EventCallbackData* ecdata = (EventCallbackData*) data;
    ecdata->type = INTERRUPT;
    event_base_loopbreak(ecdata->ebase);
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

// ClientHandle constructors & destructor

ClientHandle::ClientHandle(Client& iclient, const std::string& teamname)
    : client_ptr(&iclient), teamname(teamname), atype(INTERNAL), connected(true)
{}

ClientHandle::ClientHandle(AddressType a, FILE* ptr) : atype(a)
{
    stream.file = ptr;
    stream.Await<uint8_t>().Await<uint32_t>().Then([this] (Stream& s, Field& f) {
        auto type = f[-1].Get<uint8_t>();
        if (type != MessageFormat::JSON && type != MessageFormat::MSGPACK) {
            s.Reset();
            Error("Invalid message type!");
            return;
        }
        auto size = f.Get<uint32_t>();
        if (size > BUXTEHUDE_MAX_MESSAGE_LENGTH) {
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
    Write(Message { .type = BUXTEHUDE_HANDSHAKE, .content = {
        { "version", (int) BUXTEHUDE_CURRENT_VERSION }
    }});
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

void ClientHandle::Error(const std::string& errstr)
{
    if (time(nullptr) - last_error < 1) return;
    last_error = time(nullptr);

    Write({ .type = BUXTEHUDE_ERROR, .content = errstr });
    if (!handshaken) Disconnect("Failed handshake");
}

void ClientHandle::Disconnect(const std::string& reason)
{
    if (!connected) return;
    Write({ .type = BUXTEHUDE_DISCONNECT, .content = {
        { "reason", reason },
        { "who", BUXTEHUDE_YOU }
    }});
    Disconnect_NoWrite();
}

void ClientHandle::Disconnect_NoWrite()
{
    if (!connected) return;
    if ((atype == UNIX || atype == INTERNET) && stream.file) {
        fclose(stream.file);

        if (read_event) {
            event_del(read_event);
            event_free(read_event);
        }
    } else if (atype == INTERNAL) {
        client_ptr->Close();
    }
    logger(DEBUG, fmt::format("Disconnecting client {}", teamname));
    connected = false;
}

bool ClientHandle::Available(const std::string& type)
{
    return std::find(unavailable.begin(), unavailable.end(), type) == unavailable.end();
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

Server::Server(const std::string& path)
{
    UnixServer(path);
}

Server::Server(short port)
{
    IPServer(port);
}

Server::~Server()
{
    Close();
}

// Listening socket setup

bool Server::UnixServer(const std::string& path)
{
    sockaddr_un addr;
    addr.sun_family = PF_LOCAL;

    size_t path_len = path.size() < sizeof(addr.sun_path) - 1 ?
        path.size() : sizeof (addr.sun_path) - 1;
    memcpy(addr.sun_path, path.data(), path_len);
    addr.sun_path[path_len] = 0; // Null terminator

    unix_path = addr.sun_path;

    if (!SetupEvents()) return false;

    unix_listener = evconnlistener_new_bind(ebase, ConnectionCallback, &callback_data,
       LEV_OPT_CLOSE_ON_FREE, -1, (sockaddr*)&addr, sizeof(addr));

    if (!unix_listener) {
        logger(WARNING,
            fmt::format("Failed to listen for UNIX domain connections at {}: {}",
                path, strerror(errno)));
        unix_server = -1;
        return false;
    }

    unix_server = evconnlistener_get_fd(unix_listener);
    logger(DEBUG, fmt::format("Listening on file {}", path));

    return true;
}

bool Server::IPServer(short port)
{
    sockaddr_in addr = {0};
    addr.sin_family = PF_INET;
    addr.sin_port = htons(port);

    if (INADDR_ANY) addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (!SetupEvents()) return false;

    // Passing -1 as the backlog allows libevent to try select an optimal backlog number.
    ip_listener = evconnlistener_new_bind(ebase, ConnectionCallback, &callback_data,
        LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE, -1, (sockaddr*)&addr, sizeof(addr));

    if (!ip_listener) {
        logger(WARNING,
            fmt::format("Failed to listen for internet domain connections on port {}: {}",
                port, strerror(errno)));
        ip_server = -1;
        return false;
    }

    ip_server = evconnlistener_get_fd(ip_listener);

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
        event_active(interrupt_event, 0, 0);
        current_thread.join();
    }

    {
    std::lock_guard<std::mutex> guard(clients_mutex);
    clients.clear();
    }

    if (unix_listener) {
        evconnlistener_free(unix_listener);
        unlink(unix_path.c_str());
    }

    if (ip_listener) evconnlistener_free(ip_listener);

    if (ebase) event_base_free(ebase);
    if (interrupt_event) event_free(interrupt_event);
}

void Server::Broadcast(const Message& m)
{
    for (ClientHandle* cl : GetClients(BUXTEHUDE_ALL)) cl->Write(m);
}

// Server connection management
// INTERNAL only functions

void Server::AddClient(Client& cl, const std::string& name)
{
    std::lock_guard<std::mutex> guard(clients_mutex);
    auto& ch = clients.emplace_back(std::make_unique<ClientHandle>(cl, name));
    ch->Handshake();
}

void Server::RemoveClient(Client& cl)
{
    std::string teamname;
    { // Lock guard
    std::lock_guard<std::mutex> guard(clients_mutex);
    for (auto it = clients.begin(); it != clients.end(); ++it) {
        ClientHandle* ch = it->get();
        if (ch->client_ptr == &cl) {
            teamname = std::move(ch->teamname);
            clients.erase(it);
            break;
        }
    }
    } // Exit lock guard

    Broadcast({ .type = BUXTEHUDE_DISCONNECT, .content = {
        { "who", teamname }
    }});
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
        Server::HandleIter iter = std::find_if(clients.begin(), clients.end(),
            [ch] (auto& unique_ptr) { return unique_ptr.get() == ch; }
        );
        clients.erase(iter);
        }
        Broadcast({ .type = BUXTEHUDE_DISCONNECT, .content = {
            { "who", teamname }
        }});
    }
}

void Server::HandleMessage(ClientHandle* ch, Message&& msg)
{
    // Types of the JSON values are validated in checks
    if (!ch->handshaken) {
        if (msg.type != BUXTEHUDE_HANDSHAKE ||
            !ValidateJSON(msg.content, VALIDATE_HANDSHAKE_SERVERSIDE)) {
            ch->Disconnect("Failed handshake");
            return;
        }

        ch->teamname = msg.content["teamname"];
        ch->preferences.format = msg.content["format"];
        ch->handshaken = true;
        return;
    }

    if (msg.type == BUXTEHUDE_AVAILABLE) {
        if (!ValidateJSON(msg.content, VALIDATE_AVAILABLE)) {
            ch->Error("Incorrect format for $$available message");
            return;
        }
        std::string type = msg.content["type"];
        bool available = msg.content["available"];
        auto it = std::find(ch->unavailable.begin(), ch->unavailable.end(), type);
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

    ebase = event_base_new();
    callback_data.ebase = ebase;
    if (!ebase) {
        logger(WARNING, "Failed to create event base");
        return false;
    }

    interrupt_event = event_new(ebase, -1, EV_PERSIST, LoopInterruptCallback,
        &callback_data);

    return true;
}

void Server::Listen()
{
    while (event_base_dispatch(ebase) == 0) {
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
    auto& cl = clients.emplace_back(std::make_unique<ClientHandle>(atype, stream));

    cl->read_event = event_new(ebase, fd, EV_PERSIST | EV_READ,
        ReadCallback, (void*)&callback_data);

    event_add(cl->read_event, &DEFAULT_TIMEOUT);

    logger(DEBUG, fmt::format("New client connected on {} domain, fd = {}",
           atype == UNIX ? "UNIX" : "internet", fd));
}

// ClientHandle iteration

std::vector<ClientHandle*> Server::GetClients(const std::string& team)
{
    std::lock_guard<std::mutex> guard(clients_mutex);
    return GetClients_NoLock(team);
}

Server::HandleIter Server::GetClientBySocket(int fd)
{
    auto iter = std::find_if(clients.begin(), clients.end(),
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
    auto iter = std::find_if(clients.begin(), clients.end(),
        [ptr] (auto& unique_pointer) {
            return unique_pointer->client_ptr == ptr;
        }
    );

    if (iter == clients.end())
        logger(WARNING, fmt::format("No client with pointer {} found", (void*)ptr));

    return iter;
}

ClientHandle* Server::GetFirstAvailable(const std::string& team,
                                        const std::string& type,
                                        const ClientHandle* exclude)
{
    ClientHandle* result = nullptr;

    for (auto it = clients.begin(); it != clients.end(); ++it) {
        ClientHandle* ch = it->get();
        if ((ch->teamname == team || team == BUXTEHUDE_ALL) && ch != exclude)
            result = ch;
        else continue;
        if (ch->Available(type)) return result;
    }

    return result;
}

std::vector<ClientHandle*> Server::GetClients_NoLock(const std::string& team)
{
    std::vector<ClientHandle*> result;
    for (auto it = clients.begin(); it != clients.end(); ++it) {
        ClientHandle* ch = it->get();
        if (ch->teamname == team || team == BUXTEHUDE_ALL) result.push_back(ch);
    }

    return result;
}

// Client
// Client constructors & destructor

Client::~Client()
{
    Close();
}

Client::Client(Server& server, const std::string& name)
{
    InternalConnect(server, name);
}

Client::Client(const std::string& path, const std::string& name)
{
    UnixConnect(path, name);
}

Client::Client(const std::string& hostname, short port, const std::string& name)
{
    IPConnect(hostname, port, name);
}

// Connection setup functions

bool Client::IPConnect(const std::string& hostname, short port,
                       const std::string& name)
{
    if (setup) return true;
    addrinfo* res;
    addrinfo hints { .ai_flags = AI_DEFAULT, .ai_family = PF_INET,
                     .ai_socktype = SOCK_STREAM };

    int gai_error;
    if ((gai_error = getaddrinfo(hostname.c_str(), nullptr, &hints, &res))) {
        logger(WARNING,
            fmt::format("Failed to connect to address {}: getaddrinfo failed: {}",
                hostname, gai_strerror(gai_error)));
        freeaddrinfo(res);
        return false;
    }

    client_socket = socket(res->ai_family, res->ai_socktype, 0);
    sockaddr_in* addr = ((sockaddr_in*)res->ai_addr);
    addr->sin_port = htons(port);

    if (connect(client_socket, (sockaddr*)addr, sizeof(sockaddr_in))) {
        logger(WARNING, fmt::format("Failed to connect to address {}: {}",
            hostname, strerror(errno)));
        return false;
    }

    freeaddrinfo(res);

    if (!SetupEvents()) return false;

    teamname = name;
    atype = INTERNET;

    Handshake();

    return true;
}

bool Client::UnixConnect(const std::string& path, const std::string& name)
{
    if (setup) return true;
    client_socket = socket(PF_LOCAL, SOCK_STREAM, 0);

    sockaddr_un addr;
    addr.sun_family = AF_LOCAL;

    size_t path_len = path.size() < sizeof(addr.sun_path) - 1 ?
        path.size() : sizeof (addr.sun_path) - 1;
    memcpy(addr.sun_path, path.data(), path_len);
    addr.sun_path[path_len] = 0; // Null terminator

    if (connect(client_socket, (sockaddr*)&addr, sizeof(sockaddr_un))) {
        logger(WARNING, fmt::format("Failed to connect to file {}: {}",
            path, strerror(errno)));
        return false;
    }

    if (!SetupEvents()) return false;

    teamname = name;
    atype = UNIX;

    Handshake();

    return true;
}

bool Client::InternalConnect(Server& server, const std::string& name)
{
    if (setup) return true;
    server_ptr = &server;
    server.AddClient(*this, name);
    setup = true;

    teamname = name;
    atype = INTERNAL;

    Handshake();

    return true;
}

// General functions applicable to all types of Client

void Client::Write(const Message& msg)
{
    if (atype == INTERNAL) {
        server_ptr->Receive(*this, msg);
        return;
    }

    if (!Message::WriteToStream(stream.file, msg, preferences.format)) {
        logger(WARNING, "Failed to write - closing connection");
        Close();
    }
}

void Client::Handshake()
{
    Write({ .type = BUXTEHUDE_HANDSHAKE, .content = {
        { "format", preferences.format },
        { "teamname", teamname },
        { "version", BUXTEHUDE_CURRENT_VERSION }
    }});

    AddHandler(BUXTEHUDE_HANDSHAKE, [] (Client& c, const Message& m) {
        if (!ValidateJSON(m.content, VALIDATE_HANDSHAKE_CLIENTSIDE)) {
            logger(WARNING, "Rejected server handshake - disconnecting");
            c.Close();
            return;
        }

        c.EraseHandler(BUXTEHUDE_HANDSHAKE);
    });

    AddHandler(BUXTEHUDE_ERROR, [] (Client& c, const Message& m) {
        if (!ValidateJSON(m.content, VALIDATE_SERVER_MESSAGE)) {
            logger(WARNING, "Erroneous server message");
            return;
        }

        logger(INFO, fmt::format("Error message from server: {}",
               m.content.get<std::string>()));
    });
}

void Client::SetAvailable(const std::string& type, bool available)
{
    Write({ .type = BUXTEHUDE_AVAILABLE, .content = {
        { "type", type },
        { "available", available }
    }});
}

void Client::HandleMessage(const Message& msg)
{
    if (msg.type.empty()) {
        logger(WARNING, "Received message with no type!");
        return;
    }

    if (handlers.contains(msg.type)) handlers[msg.type](*this, msg);
}

// Handlers

void Client::AddHandler(const std::string& type, Handler&& h)
{
    handlers.emplace(type, std::forward<Handler>(h));
}

void Client::EraseHandler(const std::string& type) { handlers.erase(type); }

void Client::ClearHandlers() { handlers.clear(); }

// Start & stop client

void Client::Run()
{
    if (!setup) {
        logger(WARNING, "Tried to run client before setup!");
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
    logger(DEBUG, "Closing client");

    if (stream.file) {
        event_active(interrupt_event, 0, 0);
        if (current_thread.joinable()
            && std::this_thread::get_id() != current_thread.get_id())
            current_thread.join();

        fclose(stream.file);
        event_free(read_event);
        event_free(interrupt_event);
        event_base_free(ebase);
    }

    if (server_ptr && atype == INTERNAL) {
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

bool Client::SetupEvents()
{
    ebase = event_base_new();
    callback_data.ebase = ebase;
    if (!ebase) {
        logger(WARNING, "Failed to create event base");
        return false;
    }

    read_event = event_new(ebase, client_socket, EV_PERSIST | EV_READ,
        ReadCallback, (void*)&callback_data);
    interrupt_event = event_new(ebase, -1, EV_PERSIST, LoopInterruptCallback,
        &callback_data);

    event_add(read_event, &DEFAULT_TIMEOUT);

    stream.file = fdopen(client_socket, "r+");
    stream.Await<uint8_t>().Await<uint32_t>().Then([this] (Stream& stream, Field& f) {
        auto type = f[-1].Get<uint8_t>();
        if (type != MessageFormat::JSON && type != MessageFormat::MSGPACK) {
            stream.Reset();
            logger(WARNING, "Invalid message type!");
            return;
        }
        auto size = f.Get<uint32_t>();
        if (size > BUXTEHUDE_MAX_MESSAGE_LENGTH) {
            stream.Reset();
            logger(WARNING, "Buffer size too big!");
            return;
        }
        stream.Await(size);
    });

    preferences.format = MSGPACK;
    setup = true;
    return true;
}

void Client::Read()
{
    if (!stream.Read()) {
        if (stream.Status() == REACHED_EOF) Close();
        return;
    }

    std::string_view data = stream[2].GetView();

    try {
        Message message = Message::Deserialise((MessageFormat)stream[0].Get<char>(),
            data);
        HandleMessage(message);
    } catch (const json::parse_error& e) {
        std::string error = fmt::format("Error parsing message: {}", e.what());
        logger(WARNING, error);
    }

    stream.Delete(stream[2]);
    stream.Reset();
}

void Client::Listen()
{
    while (event_base_dispatch(ebase) == 0) {
        if (!run) break;
        switch (callback_data.type) {
        case READ_READY:
            Read();
        default:
            break;
        case INTERRUPT: return;
        }
    }
}

}
