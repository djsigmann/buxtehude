#pragma once

#include <vector>
#include <string>
#include <queue>
#include <atomic>
#include <thread>
#include <type_traits>
#include <memory>

#include <fmt/core.h>

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <netdb.h>
#include <unistd.h>

#include <event2/event.h>
#include <event2/listener.h>

#include "io.hpp"

#define BUXTEHUDE_DEFAULT_PORT 1637

#define BUXTEHUDE_ALL "$$all"
#define BUXTEHUDE_AVAILABLE "$$available"
#define BUXTEHUDE_ERROR "$$error"
#define BUXTEHUDE_INFO "$$info"
#define BUXTEHUDE_SERVER "$$server"
#define BUXTEHUDE_YOU "$$you"
#define BUXTEHUDE_DISCONNECT "$$disconnect"
#define BUXTEHUDE_HANDSHAKE "$$handshake"
#define BUXTEHUDE_SUBSCRIBE "$$subscribe"

#define BUXTEHUDE_MAX_MESSAGE_LENGTH 4096

#define BUXTEHUDE_CURRENT_VERSION 0
#define BUXTEHUDE_MINIMUM_COMPATIBLE 0

namespace buxtehude
{

using nlohmann::json;

class Server;
class Client;
struct Message;

enum AddressType
{
    UNIX, INTERNET, INTERNAL
};

enum MessageFormat
{
    JSON = 0, MSGPACK = 1
};

enum EventType
{
    NEW_CONNECTION, READ_READY, TIMEOUT, INTERRUPT
};

enum LogLevel { DEBUG = 0, INFO = 1, WARNING = 2, SEVERE = 3 };

struct EventCallbackData
{
    sockaddr address;
    event_base* ebase; // Set in the listening loop and does not change
    int fd;
    EventType type;
    int addr_len;
};

struct Message
{
    std::string dest, src, type;
    json content;
    bool only_first = false;

    std::string Serialise(MessageFormat f) const;
    static Message Deserialise(MessageFormat f, const char* data, size_t size);
    static bool WriteToStream(FILE* stream, const Message& m, MessageFormat f);
};

void to_json(json& j, const Message& msg);
void from_json(const json& j, Message& msg);

struct ClientPreferences
{
    MessageFormat format = JSON;
};

using Handler = void (*)(Client&, const Message&);
using LogCallback = void (*)(LogLevel, const std::string&);
using SignalHandler = void (*)(int);

// Must be called to initialise libevent, logging and SIGPIPE handling
void Initialise(LogCallback cb = nullptr, SignalHandler sh = nullptr);

constinit inline LogCallback logger = nullptr;

class ClientHandle
{
public:
    ClientHandle(Client& iclient, const std::string& teamname);
    ClientHandle(AddressType a, FILE* ptr);
    ClientHandle(const ClientHandle& other) = delete;
    ClientHandle(ClientHandle&& other) = delete;
    ClientHandle& operator=(ClientHandle&& other) = delete;
    ~ClientHandle();

    // Applicable to all types of ClientHandle
    void Handshake();
    void Write(const Message& m);
    void Error(const std::string& errstr);
    void Disconnect(const std::string& reason="Disconnected by server");
    void Disconnect_NoWrite();

    bool Available(const std::string& type);

    // Try to read a message from the socket - only for INTERNET/UNIX
    std::pair<bool, Message> Read();
private:
    Stream stream; // Only for UNIX/INTERNET

public:
    std::vector<std::string> unavailable;
    std::string teamname = "$$unauthorised";
    event* read_event = nullptr;
    Client* client_ptr = nullptr; // Only for INTERNAL connections

    AddressType atype;
    ClientPreferences preferences;

    int socket = -1;

    bool handshaken = false;
    bool connected = false;
};

class Server
{
public:
    Server() = default;
    Server(const Server& other) = delete;
    Server(const std::string& path); // UNIX server
    Server(short port); // IP server
    ~Server();

    // Locks access to ClientHandle list.
    std::vector<ClientHandle*> GetClients(const std::string& team=BUXTEHUDE_ALL);

    bool UnixServer(const std::string& path="buxtehude_unix");
    bool IPServer(short port=BUXTEHUDE_DEFAULT_PORT);

    void Run();
    void Close();

    void Broadcast(const Message& msg);
private: // For INTERNAL connections only.
    friend Client;
    void AddClient(Client& cl, const std::string& name);
    void RemoveClient(Client& cl);
    void Receive(Client& cl, const Message& msg);
private:
    using HandleIter = std::vector<std::unique_ptr<ClientHandle>>::iterator;

    void Serve(ClientHandle* ch);
    void HandleMessage(ClientHandle* ch, Message&& msg);

    // Only if listening sockets are opened
    bool SetupEvents();
    void Listen();
    void AddConnection(int fd, sa_family_t addr_type);

    // Retrieving clients
    HandleIter GetClientBySocket(int fd);
    HandleIter GetClientByPointer(Client* ptr);
    ClientHandle* GetFirstAvailable(const std::string& team, const std::string& type,
        const ClientHandle* exclude);
    std::vector<ClientHandle*> GetClients_NoLock(const std::string& team=BUXTEHUDE_ALL);

    std::vector<std::unique_ptr<ClientHandle>> clients;
    std::mutex clients_mutex;

    std::thread current_thread;
    std::atomic<bool> run = false;

    // File descriptors for listening sockets
    int unix_server = -1;
    int ip_server = -1;

    std::string unix_path;

    // Libevent internals
    event_base* ebase = nullptr;
    evconnlistener* ip_listener = nullptr;
    evconnlistener* unix_listener = nullptr;
    event* interrupt_event = nullptr;

    EventCallbackData callback_data;
};

class Client
{
public:
    Client() = default;
    Client(const Client& other) = delete;
    Client(Client&& other) = delete;
    Client(Server& server, const std::string& name); // Internal connection
    Client(const std::string& path, const std::string& name); // UNIX socket connection
    // IP connection
    Client(const std::string& hostname, short port, const std::string& name);
    ~Client();

    bool IPConnect(const std::string& hostname, short port, const std::string& name);
    bool UnixConnect(const std::string& path, const std::string& name);
    bool InternalConnect(Server& server, const std::string& name);

    // Applicable to all types of Client
    void Write(const Message& msg);
    void Handshake();
    void SetAvailable(const std::string& type, bool available);

    void AddHandler(const std::string& type, Handler h);
    void EraseHandler(const std::string& type);
    void ClearHandlers();

    void Run();
    void Close(); // Run must be called before Close()

    std::string teamname;
    ClientPreferences preferences;
private: // Only for INTERNAL clients
    friend ClientHandle;
    // Called by the Server ClientHandle when it sends a message
    void Receive(const Message& msg);
private:
    // Only for socket-based connections
    bool SetupEvents();
    void Read();
    void Listen();

    void HandleMessage(const Message& msg);

    AddressType atype;

    int client_socket = -1;
    Stream stream;
    Server* server_ptr = nullptr;

    std::unordered_map<std::string, Handler> handlers;
    std::queue<Message> ingress, egress;

    std::thread current_thread;
    std::atomic<bool> run = false;
    bool setup = false;

    // Libevent internals
    event_base* ebase = nullptr;
    event* read_event = nullptr;
    event* interrupt_event = nullptr;

    EventCallbackData callback_data;
};

}
