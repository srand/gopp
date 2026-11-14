// tcp-chat: a small multi-client chat room. Connect with: nc localhost 8083
// The manager owns the client list; fibers send it join, message and leave events.
//
//     $ tcp-chat [-addr :8083]
#include <gopp/bufio.hpp>
#include <gopp/chan.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/net.hpp>
#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>

#include <atomic>
#include <map>
#include <utility>

using namespace gopp;

enum class eventKind { join, message, leave };

struct Event {
    eventKind kind;
    int id;
    string name;
    string text;
    ptr<net::Conn> conn;
};

int main(int argc, char** argv) {
    string addr = ":8083";
    flag::StringVar(&addr, "addr", addr, "the address to listen on");
    flag::Parse(argc, argv);

    auto [listener, err] = net::Listen("tcp", addr);
    if (err) {
        fmt::Println(os::Stderr, "tcp-chat: {}", err);
        return 1;
    }
    fmt::Println("Chat room listening on {}", listener->Addr());

    auto events = make_chan<Event>();
    std::atomic<int> nextId{0};

    go([listener, events, &nextId] {
        for (;;) {
            auto [conn, acceptErr] = listener->Accept();
            if (acceptErr) {
                fmt::Println(os::Stderr, "tcp-chat: {}", acceptErr);
                return;
            }
            int id = nextId.fetch_add(1);
            go([conn, events, id] {
                fmt::Print(*conn, "Name: ");
                bufio::Reader reader(conn);
                auto [rawName, nameErr] = reader.ReadString('\n');
                if (nameErr) return;
                string name = strings::TrimSpace(rawName);
                if (name.empty()) name = fmt::Sprint("gopher{}", id + 1);

                events.send(Event{eventKind::join, id, name, {}, conn});
                for (;;) {
                    auto [rawLine, readErr] = reader.ReadString('\n');
                    if (readErr) break;
                    string line = strings::TrimSpace(rawLine);
                    if (line == "/quit") break;
                    if (!line.empty()) events.send(Event{eventKind::message, id, name, line, {}});
                }
                events.send(Event{eventKind::leave, id, name, {}, {}});
            });
        }
    });

    struct ClientState {
        string name;
        chan<string> outgoing;
    };
    std::map<int, ClientState> clients;
    for (Event event : events) {
        if (event.kind == eventKind::join) {
            auto outgoing = make_chan<string>(64);
            auto conn = event.conn;
            go([conn, outgoing] {
                for (const string& line : outgoing) {
                    if (fmt::Print(*conn, "{}", line).second) break;
                }
                conn->Close();
            });
            clients.emplace(event.id, ClientState{event.name, outgoing});
            string line = fmt::Sprint("*** {} joined the chat ***\n", event.name);
            for (auto& [id, client] : clients) {
                (void)id;
                client.outgoing.send(line);
            }
        } else if (event.kind == eventKind::message) {
            string line = fmt::Sprint("{}: {}\n", event.name, event.text);
            for (auto& [id, client] : clients) {
                (void)id;
                client.outgoing.send(line);
            }
        } else {
            auto it = clients.find(event.id);
            if (it == clients.end()) continue;
            it->second.outgoing.close();
            clients.erase(it);
            string line = fmt::Sprint("*** {} left the chat ***\n", event.name);
            for (auto& [id, client] : clients) {
                (void)id;
                client.outgoing.send(line);
            }
        }
    }
}
