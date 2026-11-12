// json-file: a tiny task list stored as JSON. Edit tasks.json and run again.
// Run: json-file -file tasks.json (or pass another path to -file).
#include <gopp/encoding/json.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/os.hpp>

#include <string_view>
#include <vector>

using namespace gopp;

struct Task {
    [[=json::tag("title")]] string Title;
    [[=json::tag("done")]] bool Done = false;
};

int main(int argc, char** argv) {
    string path = "tasks.json";
    flag::StringVar(&path, "file", path, "path to the JSON task file");
    flag::Parse(argc, argv);
    if (!flag::Args().empty()) {
        fmt::Println(os::Stderr, "usage: json-file [-file path]");
        return 2;
    }

    std::vector<Task> tasks;
    auto [data, read_err] = os::ReadFile(path);
    if (read_err) {
        if (!os::IsNotExist(read_err)) {
            fmt::Println(os::Stderr, "json-file: {}", read_err);
            return 1;
        }
        tasks = {{"Read the JSON docs", true}, {"Try editing this task list", false}};
    } else if (auto err = json::Unmarshal(data, &tasks)) {
        fmt::Println(os::Stderr, "json-file: {}", err);
        return 1;
    }

    auto [encoded, marshal_err] = json::MarshalIndent(tasks, "", "  ");
    if (marshal_err) {
        fmt::Println(os::Stderr, "json-file: {}", marshal_err);
        return 1;
    }
    if (auto err = os::WriteFile(path, encoded, 0644)) {
        fmt::Println(os::Stderr, "json-file: {}", err);
        return 1;
    }

    fmt::Println("Tasks in {}:", path);
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        fmt::Println("{} [{}] {}", i + 1, tasks[i].Done ? "x" : " ", tasks[i].Title);
    }
    fmt::Println("Saved {} tasks as JSON.", tasks.size());
}
