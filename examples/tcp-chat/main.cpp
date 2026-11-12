// tcp-chat: a small multi-client chat room. Connect with: nc localhost 8083
// The manager owns the client list; fibers send it join, message and leave events.
#include <gopp/bufio.hpp>
#include <gopp/chan.hpp>
#include <gopp/fmt.hpp>
#include <gopp/net.hpp>
#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>

#include <atomic>
#include <map>
#include <utility>

using namespace gopp;

enum class event_kind { join, message, leave };

struct Event {
    event_kind kind;
    int id;
    string name;
    string text;
    ptr<net::Conn> conn;
};

int main() {
    auto [listener, err] = net::Listen("tcp", ":8083");
    if (err) {
        fmt::Println(os::Stderr, "tcp-chat: {}", err);
        return 1;
    }
    fmt::Println("Chat room listening on {}", listener->Addr());

    auto events = make_chan<Event>();
    std::atomic<int> next_id{0};

    go([listener, events, &next_id] {
        for (;;) {
            auto [conn, accept_err] = listener->Accept();
            if (accept_err) {
                fmt::Println(os::Stderr, "tcp-chat: {}", accept_err);
                return;
            }
            int id = next_id.fetch_add(1);
            go([conn, events, id] {
                fmt::Print(*conn, "Name: ");
                bufio::Reader reader(conn);
                auto [raw_name, name_err] = reader.ReadString('\n');
                if (name_err) return;
                string name = strings::TrimSpace(raw_name);
                if (name.empty()) name = fmt::Sprint("gopher{}", id + 1);

                events.send(Event{event_kind::join, id, name, {}, conn});
                for (;;) {
                    auto [raw_line, read_err] = reader.ReadString('\n');
                    if (read_err) break;
                    string line = strings::TrimSpace(raw_line);
                    if (line == "/quit") break;
                    if (!line.empty()) events.send(Event{event_kind::message, id, name, line, {}});
                }
                events.send(Event{event_kind::leave, id, name, {}, {}});
            });
        }
    });

    struct ClientState {
        string name;
        chan<string> outgoing;
    };
    std::map<int, ClientState> clients;
    for (Event event : events) {
        if (event.kind == event_kind::join) {
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
        } else if (event.kind == event_kind::message) {
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
