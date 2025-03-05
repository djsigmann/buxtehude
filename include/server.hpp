#pragma once

#include "core.hpp"
#include "io.hpp"

#include <ctime>

#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <event2/event.h>
#include <event2/listener.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

namespace buxtehude
{

class Client;

class ClientHandle
{
public:
    ClientHandle(Client& iclient, std::string_view teamname);
    ClientHandle(AddressType a, FILE* ptr);
    ClientHandle(const ClientHandle& other) = delete;
    ClientHandle(ClientHandle&& other) = delete;
    ClientHandle& operator=(ClientHandle&& other) = delete;
    ~ClientHandle();

    // Applicable to all types of ClientHandle
    void Handshake();
    void Write(const Message& m);
    void Error(std::string_view errstr);
    void Disconnect(std::string_view reason="Disconnected by server");
    void Disconnect_NoWrite();

    bool Available(std::string_view type);

    // Try to read a message from the socket - only for INTERNET/UNIX
    std::optional<Message> Read();
private:
    Stream stream; // Only for UNIX/INTERNET
    std::time_t last_error = 0;
public:
    std::vector<std::string> unavailable;
    std::string teamname = "$$unauthorised";
    UEvent read_event;
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
    Server(std::string_view path); // UNIX server
    Server(short port); // IP server
    ~Server();

    // Locks access to ClientHandle list.
    std::vector<ClientHandle*> GetClients(std::string_view team=BUXTEHUDE_ALL);

    bool UnixServer(std::string_view path="buxtehude_unix");
    bool IPServer(short port=BUXTEHUDE_DEFAULT_PORT);

    void Run();
    void Close();

    void Broadcast(const Message& msg);
private: // For INTERNAL connections only.
    friend Client;
    void AddClient(Client& cl, std::string_view name);
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
    ClientHandle* GetFirstAvailable(std::string_view team, std::string_view type,
        const ClientHandle* exclude);
    std::vector<ClientHandle*> GetClients_NoLock(std::string_view team=BUXTEHUDE_ALL);

    std::vector<std::unique_ptr<ClientHandle>> clients;
    std::mutex clients_mutex;

    std::thread current_thread;
    std::atomic<bool> run = false;

    // File descriptors for listening sockets
    int unix_server = -1;
    int ip_server = -1;

    std::string unix_path;

    // Libevent internals
    UEventBase ebase;
    UEvconnListener ip_listener, unix_listener;
    UEvent interrupt_event;

    EventCallbackData callback_data;
};

}
