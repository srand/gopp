// kvctl: a kubectl-style command line for a key/value store, and the
// store's server, which it talks to over HTTP.
//
//     $ kvctl serve &                         # listens on :7070
//     $ kvctl put greeting "hello, world"
//     $ kvctl put config --from-file config.json
//     $ kvctl get greeting
//     hello, world
//     $ kvctl get greeting config -o json
//     $ kvctl list --prefix g
//     $ kvctl delete greeting                 # or rm; --all for everything
//     $ kvctl --help                          # grouped commands, global flags
//     $ kvctl gte                             # "Did you mean this? get"
//
// Shell completion completes commands, flags, -o's values and, by asking
// the server, the keys:
//
//     $ source <(kvctl completion bash)       # or zsh, fish, powershell
//     $ kvctl get gr<TAB>                     # kvctl get greeting
//
// The commands are cmd::Commands (cobra's), with persistent flags that all
// of them inherit (--server, --output, --timeout), argument validators,
// mutually exclusive flags, aliases and command groups. Ctrl-C cancels the
// context that ExecuteContext gives the commands: requests in flight stop,
// and `serve` shuts down gracefully.
#include <gopp/chan.hpp>
#include <gopp/cmd.hpp>
#include <gopp/context.hpp>
#include <gopp/encoding/json.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/net/http.hpp>
#include <gopp/net/url.hpp>
#include <gopp/os.hpp>
#include <gopp/os/signal.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>
#include <gopp/syscall.hpp>

#include <algorithm>
#include <map>
#include <vector>

using namespace gopp;
namespace signal = os::signal;

struct Entry {
    [[=json::tag("key")]] string Key;
    [[=json::tag("value")]] string Value;
};

// ---------------------------------------------------------------------------
// The server

// The store: keys to values, in key order.
class Store {
public:
    std::vector<Entry> List(std::string_view prefix) {
        std::lock_guard lock(mu_);
        std::vector<Entry> out;
        for (auto& [k, v] : data_) {
            if (k.starts_with(prefix)) out.push_back({k, v});
        }
        return out;
    }
    std::pair<string, bool> Get(const string& key) {
        std::lock_guard lock(mu_);
        auto it = data_.find(key);
        if (it == data_.end()) return {{}, false};
        return {it->second, true};
    }
    void Put(string key, string value) {
        std::lock_guard lock(mu_);
        data_[std::move(key)] = std::move(value);
    }
    bool Delete(const string& key) {
        std::lock_guard lock(mu_);
        return data_.erase(key) > 0;
    }
    void Clear() {
        std::lock_guard lock(mu_);
        data_.clear();
    }

private:
    sync::Mutex mu_;
    std::map<string, string> data_;
};

error serve(cmd::Command& c, string addr) {
    auto store = std::make_shared<Store>();
    auto mux = http::NewServeMux();
    mux->HandleFunc("GET /keys", [store](http::ResponseWriter& w, http::Request& r) {
        w.Header().Set("Content-Type", "application/json"_s);
        json::Encode(w, store->List(r.FormValue("prefix")));
    });
    mux->HandleFunc("GET /keys/{key}", [store](http::ResponseWriter& w, http::Request& r) {
        string key = r.PathValue("key");
        auto [value, ok] = store->Get(key);
        if (!ok) return http::Error(w, fmt::Sprint("key {} not found", strconv::Quote(key)), http::StatusNotFound);
        w.Header().Set("Content-Type", "application/json"_s);
        json::Encode(w, Entry{key, value});
    });
    mux->HandleFunc("PUT /keys/{key}", [store](http::ResponseWriter& w, http::Request& r) {
        auto [body, err] = io::ReadAll(*r.Body);
        if (err) return http::Error(w, err.Error(), http::StatusBadRequest);
        store->Put(r.PathValue("key"), String(body));
        w.WriteHeader(http::StatusNoContent);
    });
    mux->HandleFunc("DELETE /keys/{key}", [store](http::ResponseWriter& w, http::Request& r) {
        string key = r.PathValue("key");
        if (!store->Delete(key)) return http::Error(w, fmt::Sprint("key {} not found", strconv::Quote(key)), http::StatusNotFound);
        w.WriteHeader(http::StatusNoContent);
    });
    mux->HandleFunc("DELETE /keys", [store](http::ResponseWriter& w, http::Request&) {
        store->Clear();
        w.WriteHeader(http::StatusNoContent);
    });

    http::Server srv{.Addr = addr, .Handler = mux};
    auto served = make_chan<error>(1);
    go([&] { served.send(srv.ListenAndServe()); });
    fmt::Println(c.OutOrStdout(), "serving on {}", addr);

    // Until Ctrl-C cancels the command's context.
    error err;
    select(recv(served, [&](error e) { err = e; }), recv(c.Context()->Done(), [] {}));
    if (err) return err;
    fmt::Println(c.OutOrStdout(), "shutting down");
    auto [ctx, cancel] = context::WithTimeout(context::Background(), 5 * time::Second);
    srv.Shutdown(ctx);
    cancel();
    served.recv();
    return nullptr;
}

// ---------------------------------------------------------------------------
// The client

struct Options {
    string server;
    string output;
    time::Duration timeout;
};

class Client {
public:
    Client(const Options& o, ptr<context::Context> ctx) : o_(o), ctx_(std::move(ctx)) {}
    ~Client() {
        for (auto& c : cancels_) c();
    }

    std::pair<std::vector<Entry>, error> List(std::string_view prefix) {
        auto [resp, err] = send("GET", "/keys?prefix="_s + url::QueryEscape(prefix), nullptr);
        if (err) return {{}, err};
        std::vector<Entry> entries;
        if (auto err = json::NewDecoder(*resp->Body).Decode(&entries)) return {{}, err};
        return {entries, nullptr};
    }

    std::pair<Entry, error> Get(std::string_view key) {
        auto [resp, err] = send("GET", "/keys/"_s + url::PathEscape(key), nullptr);
        if (err) return {{}, err};
        Entry e;
        if (auto err = json::NewDecoder(*resp->Body).Decode(&e)) return {{}, err};
        return {e, nullptr};
    }

    error Put(std::string_view key, string value) {
        return send("PUT", "/keys/"_s + url::PathEscape(key), strings::NewReader(std::move(value))).second;
    }

    error Delete(std::string_view key) { return send("DELETE", "/keys/"_s + url::PathEscape(key), nullptr).second; }
    error DeleteAll() { return send("DELETE", "/keys", nullptr).second; }

private:
    // Sends a request; a status other than 2xx is an error, with the
    // server's message.
    std::pair<ptr<http::Response>, error> send(std::string_view method, string path, ptr<io::Reader> body) {
        auto [ctx, cancel] = context::WithTimeout(ctx_, o_.timeout);
        cancels_.push_back(cancel);
        auto [req, err] = http::NewRequestWithContext(ctx, method, o_.server + path, body);
        if (err) return {nullptr, err};
        auto [resp, rerr] = http::DefaultClient().Do(req);
        if (rerr) return {nullptr, rerr};
        if (resp->StatusCode / 100 != 2) {
            auto [msg, _] = io::ReadAll(*resp->Body);
            return {nullptr, errors::New(strings::TrimSpace(String(msg)))};
        }
        return {resp, nullptr};
    }

    Options o_;
    ptr<context::Context> ctx_;
    std::vector<context::CancelFunc> cancels_;  // of the requests' contexts
};

// Prints entries as the --output flag asks.
error printEntries(cmd::Command& c, const Options& o, const std::vector<Entry>& entries, bool valuesOnly) {
    io::Writer& out = c.OutOrStdout();
    if (o.output == "json") return json::Encode(out, entries);
    if (valuesOnly) {
        for (auto& e : entries) fmt::Println(out, "{}", e.Value);
        return nullptr;
    }
    std::size_t width = 3;
    for (auto& e : entries) width = std::max(width, e.Key.size());
    fmt::Println(out, "{:<{}}   VALUE", "KEY", width);
    for (auto& e : entries) {
        // A value with line breaks would break the table: quote it.
        bool quote = strings::ContainsFunc(e.Value, [](rune r) { return r < ' '; });
        fmt::Println(out, "{:<{}}   {}", e.Key, width, quote ? strconv::Quote(e.Value) : e.Value);
    }
    return nullptr;
}

// Completes keys by asking the server, kubectl-style: the shell runs
// `kvctl __complete get gr`, which calls this.
cmd::CompletionFunc completeKeys(Options& o) {
    return [&o](cmd::Command& c, std::span<const string> args, std::string_view toComplete) {
        Client client(o, c.Context());
        auto [entries, err] = client.List(toComplete);
        if (err) {
            cmd::CompDebugln(err.Error().view(), false);
            return std::pair{std::vector<cmd::Completion>{}, cmd::ShellCompDirectiveNoFileComp};
        }
        std::vector<cmd::Completion> keys;
        for (auto& e : entries) {
            // Not the keys given already.
            if (std::find(args.begin(), args.end(), e.Key) != args.end()) continue;
            keys.push_back(cmd::CompletionWithDesc(e.Key, e.Value));
        }
        return std::pair{keys, cmd::ShellCompDirectiveNoFileComp};
    };
}

// ---------------------------------------------------------------------------
// The commands

ptr<cmd::Command> newGetCmd(Options& o) {
    return cmd::New({
        .Use = "get KEY...",
        .Aliases = {"g"},
        .Short = "Show the values of keys",
        .GroupID = "basic",
        .Long = "Show the values of one or more keys. With -o json, the keys too.",
        .Example = "  kvctl get greeting\n  kvctl get greeting config -o json",
        .ValidArgsFunction = completeKeys(o),
        .Args = cmd::MinimumNArgs(1),
        .RunE = [&o](cmd::Command& c, std::span<const string> args) -> error {
            Client client(o, c.Context());
            std::vector<Entry> entries;
            for (auto& key : args) {
                auto [e, err] = client.Get(key);
                if (err) return err;
                entries.push_back(e);
            }
            return printEntries(c, o, entries, true);
        },
    });
}

ptr<cmd::Command> newPutCmd(Options& o) {
    auto fromFile = std::make_shared<string>();
    auto c = cmd::New({
        .Use = "put KEY [VALUE]",
        .Aliases = {"set"},
        .Short = "Set the value of a key",
        .GroupID = "basic",
        .Example = "  kvctl put greeting \"hello, world\"\n  kvctl put config --from-file config.json",
        .ValidArgsFunction = completeKeys(o),
        .Args = cmd::RangeArgs(1, 2),
        .RunE = [&o, fromFile](cmd::Command& c, std::span<const string> args) -> error {
            string value;
            if (!fromFile->empty()) {
                if (args.size() == 2) return errors::New("give a VALUE or --from-file, not both");
                auto [data, err] = os::ReadFile(*fromFile);
                if (err) return err;
                value = String(data);
            } else if (args.size() == 2) {
                value = args[1];
            } else {
                return errors::New("a VALUE or --from-file is required");
            }
            Client client(o, c.Context());
            return client.Put(args[0], value);
        },
    });
    c->Flags()->StringVarP(fromFile.get(), "from-file", "f", "", "read the value from `file`");
    c->MarkFlagFilename("from-file");
    return c;
}

ptr<cmd::Command> newDeleteCmd(Options& o) {
    auto all = std::make_shared<bool>(false);
    auto c = cmd::New({
        .Use = "delete [KEY... | --all]",
        .Aliases = {"rm"},
        .Short = "Delete keys",
        .GroupID = "basic",
        .ValidArgsFunction = completeKeys(o),
        .Args = [all](cmd::Command&, std::span<const string> args) -> error {
            if (*all && !args.empty()) return errors::New("keys and --all can't go together");
            if (!*all && args.empty()) return errors::New("give keys to delete, or --all");
            return nullptr;
        },
        .RunE = [&o, all](cmd::Command& c, std::span<const string> args) -> error {
            Client client(o, c.Context());
            if (*all) return client.DeleteAll();
            for (auto& key : args) {
                if (auto err = client.Delete(key)) return err;
                fmt::Println(c.OutOrStdout(), "deleted {}", key);
            }
            return nullptr;
        },
    });
    c->Flags()->BoolVar(all.get(), "all", false, "delete all keys");
    return c;
}

ptr<cmd::Command> newListCmd(Options& o) {
    struct listFlags {
        string prefix;
        isize limit = 0;
    };
    auto f = std::make_shared<listFlags>();
    auto c = cmd::New({
        .Use = "list",
        .Aliases = {"ls"},
        .Short = "List the keys and their values",
        .GroupID = "basic",
        .Args = cmd::NoArgs,
        .RunE = [&o, f](cmd::Command& c, std::span<const string>) -> error {
            Client client(o, c.Context());
            auto [entries, err] = client.List(f->prefix);
            if (err) return err;
            if (f->limit > 0 && static_cast<isize>(entries.size()) > f->limit) entries.resize(static_cast<std::size_t>(f->limit));
            return printEntries(c, o, entries, false);
        },
    });
    c->Flags()->StringVarP(&f->prefix, "prefix", "p", "", "only keys that start with `prefix`");
    c->Flags()->IntVarP(&f->limit, "limit", "l", 0, "at most this many keys (0: all)");
    return c;
}

ptr<cmd::Command> newServeCmd() {
    auto addr = std::make_shared<string>();
    auto c = cmd::New({
        .Use = "serve",
        .Short = "Run the key/value server",
        .GroupID = "server",
        .Long = "Run the key/value server, which keeps the keys in memory, until Ctrl-C.",
        .Args = cmd::NoArgs,
        .RunE = [addr](cmd::Command& c, std::span<const string>) { return serve(c, *addr); },
    });
    c->Flags()->StringVar(addr.get(), "addr", ":7070", "address to listen on");
    return c;
}

ptr<cmd::Command> newRootCmd(Options& o) {
    auto root = cmd::New({
        .Use = "kvctl",
        .Short = "kvctl controls a key/value store",
        .Long = "kvctl controls a key/value store, and runs one: kvctl serve.\n\n"
                "The server's address comes from --server, or $KVCTL_SERVER.",
        .Version = "1.0.0",
        .PersistentPreRunE = [&o](cmd::Command& c, std::span<const string>) -> error {
            if (!c.Flags()->Changed("server")) {
                if (auto [env, ok] = os::LookupEnv("KVCTL_SERVER"); ok) o.server = env;
            }
            if (o.output != "text" && o.output != "json") {
                return fmt::Errorf("unknown output format {}: text or json", strconv::Quote(o.output));
            }
            // From here on, errors are about the work, not the command
            // line: don't show the usage with them.
            c.SilenceUsage = true;
            return nullptr;
        },
    });
    auto* pf = root->PersistentFlags();
    pf->StringVarP(&o.server, "server", "s", "http://127.0.0.1:7070", "the server's `URL`");
    pf->StringVarP(&o.output, "output", "o", "text", "output format: text or json");
    pf->DurationVar(&o.timeout, "timeout", 5 * time::Second, "time limit for each request");
    root->RegisterFlagCompletionFunc("output", cmd::FixedCompletions({"text\tvalues, and tables of keys", "json\tJSON"},
                                                                      cmd::ShellCompDirectiveNoFileComp));

    root->AddGroup(cmd::Group{.ID = "basic", .Title = "Basic Commands:"}, cmd::Group{.ID = "server", .Title = "Server Commands:"});
    root->AddCommand(newGetCmd(o), newPutCmd(o), newDeleteCmd(o), newListCmd(o), newServeCmd());
    return root;
}

int main() {
    Options o;
    auto root = newRootCmd(o);
    auto [ctx, stop] = signal::NotifyContext(context::Background(), os::Interrupt, syscall::SIGTERM_);
    auto err = root->ExecuteContext(ctx);
    stop();
    return err ? 1 : 0;
}
