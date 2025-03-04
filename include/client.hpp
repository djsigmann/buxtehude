#pragma once

#include "core.hpp"
#include "io.hpp"

#include <atomic>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <event2/event.h>

namespace buxtehude
{

class Server;
class ClientHandle;

class Client
{
public:
    Client() = default;
    Client(const Client& other) = delete;
    Client(Client&& other) = delete;
    Client(Server& server, std::string_view name); // Internal connection
    Client(std::string_view path, std::string_view name); // UNIX socket connection
    // IP connection
    Client(std::string_view hostname, short port, std::string_view name);
    ~Client();

    bool IPConnect(std::string_view hostname, short port, std::string_view name);
    bool UnixConnect(std::string_view path, std::string_view name);
    bool InternalConnect(Server& server, std::string_view name);

    // Applicable to all types of Client
    void Write(const Message& msg);
    void Handshake();
    void SetAvailable(std::string_view type, bool available);

    void AddHandler(std::string_view type, Handler&& h);
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
