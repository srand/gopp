// tcp-chat: a chat room for nc. One fiber, the room, owns the list of
// clients; the connections' fibers send it join, message and leave events.
// Nothing is shared and nothing is locked: share memory by communicating.
//
//     $ tcp-chat [-addr :8083]
//     $ nc localhost 8083
//     Name: ann
//     *** ann joined the chat ***
//     hello
//     ann: hello
//     /quit
//
// Each client also has a fiber that writes to its connection, fed by a
// buffered channel, so that one slow client doesn't hold up the room.
#include <gopp/bufio.hpp>
#include <gopp/chan.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/net.hpp>
#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>

#include <map>

using namespace gopp;

enum class EventKind { join, message, leave };

struct Event {
    EventKind kind;
    int id;
    string name;
    string text;          // for message
    ptr<net::Conn> conn;  // for join
};

// Reads one client: its name, then its lines, until it leaves.
void handleConn(ptr<net::Conn> conn, int id, chan<Event> events) {
    fmt::Print(*conn, "Name: ");
    bufio::Reader reader(conn);
    auto [rawName, err] = reader.ReadString('\n');
    if (err) {
        conn->Close();
        return;
    }
    string name = strings::TrimSpace(rawName);
    if (name.empty()) name = fmt::Sprint("gopher{}", id);

    events.send({EventKind::join, id, name, {}, conn});
    for (;;) {
        auto [rawLine, readErr] = reader.ReadString('\n');
        if (readErr) break;
        string line = strings::TrimSpace(rawLine);
        if (line == "/quit") break;
        if (!line.empty()) events.send({EventKind::message, id, name, line, nullptr});
    }
    events.send({EventKind::leave, id, name, {}, nullptr});
}

// The room: the only fiber that sees the clients.
void room(chan<Event> events) {
    std::map<int, chan<string>> clients;  // each client's outgoing lines
    auto broadcast = [&](const string& line) {
        for (auto& [_, out] : clients) out.send(line);
    };

    for (const Event& e : events) {
        switch (e.kind) {
            case EventKind::join: {
                auto out = make_chan<string>(64);
                go([conn = e.conn, out] {
                    for (const string& line : out) {
                        if (fmt::Print(*conn, "{}", line).second) break;
                    }
                    conn->Close();
                });
                clients.emplace(e.id, out);
                broadcast(fmt::Sprint("*** {} joined the chat ***\n", e.name));
                break;
            }
            case EventKind::message:
                broadcast(fmt::Sprint("{}: {}\n", e.name, e.text));
                break;
            case EventKind::leave:
                if (auto it = clients.find(e.id); it != clients.end()) {
                    it->second.close();  // ends the client's writer, which closes the connection
                    clients.erase(it);
                }
                broadcast(fmt::Sprint("*** {} left the chat ***\n", e.name));
                break;
        }
    }
}

int main(int argc, char** argv) {
    string addr = ":8083";
    flag::StringVar(&addr, "addr", addr, "the address to listen on");
    flag::Parse(argc, argv);

    auto [listener, err] = net::Listen("tcp", addr);
    if (err) {
        fmt::Println(os::Stderr, "tcp-chat: {}", err);
        return 1;
    }
    fmt::Println("chat room listening on {}", listener->Addr());

    auto events = make_chan<Event>();
    go([events] { room(events); });

    for (int id = 1;; id++) {
        auto [conn, acceptErr] = listener->Accept();
        if (acceptErr) {
            fmt::Println(os::Stderr, "tcp-chat: {}", acceptErr);
            return 1;
        }
        go([conn, id, events] { handleConn(conn, id, events); });
    }
}
