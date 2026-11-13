// json-file: a tiny task list stored as JSON. Edit tasks.json and run again.
//
//     $ json-file [-file tasks.json] [-add "Buy milk"] [-done 2]
//
// A missing file is a new list (errors::Is(err, fs::ErrNotExist)); other
// errors are wrapped with fmt::W, Go's %w, so that the message says what
// failed and errors::Is still sees the cause. The list is saved to a
// temporary file that is then renamed over the old one, so a crash never
// leaves half a file.
#include <gopp/encoding/json.hpp>
#include <gopp/errors.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io/fs.hpp>
#include <gopp/os.hpp>
#include <gopp/path.hpp>

#include <utility>
#include <vector>

using namespace gopp;

struct Task {
    [[=json::tag("title")]] string Title;
    [[=json::tag("done")]] bool Done = false;
};

// Reads the tasks; a missing file is the starter list.
std::pair<std::vector<Task>, error> load(std::string_view name) {
    auto [data, err] = os::ReadFile(name);
    if (errors::Is(err, fs::ErrNotExist)) {
        return {{{"Read the JSON docs", true}, {"Try editing this task list", false}}, nullptr};
    }
    if (err) return {{}, err};  // a *PathError: "open tasks.json: permission denied"
    std::vector<Task> tasks;
    if (auto err = json::Unmarshal(data, &tasks)) return {{}, fmt::Errorf("parsing {}: {}", name, fmt::W(err))};
    return {std::move(tasks), nullptr};
}

// Writes the tasks to a temporary file next to name and renames it over
// name: readers see the old list or the new one, never part of one.
error save(std::string_view name, const std::vector<Task>& tasks) {
    auto [encoded, err] = json::MarshalIndent(tasks, "", "  ");
    if (err) return err;
    encoded = append(encoded, byte('\n'));

    auto [f, cerr] = os::CreateTemp(path::Dir(name), ".tasks-*.json");
    if (cerr) return fmt::Errorf("saving {}: {}", name, fmt::W(cerr));
    string tmp = f->Name();
    auto [n, werr] = f->Write(encoded);
    error serr = werr ? nullptr : f->Sync();
    error clerr = f->Close();
    if (error err = werr ? werr : serr ? serr : clerr) {
        os::Remove(tmp);
        return fmt::Errorf("saving {}: {}", name, fmt::W(err));
    }
    if (error err = os::Rename(tmp, name)) {
        os::Remove(tmp);
        return fmt::Errorf("saving {}: {}", name, fmt::W(err));
    }
    return nullptr;
}

int main(int argc, char** argv) {
    string file = "tasks.json", add;
    isize done = 0;
    flag::StringVar(&file, "file", file, "path to the JSON task file");
    flag::StringVar(&add, "add", "", "a task to add");
    flag::IntVar(&done, "done", 0, "the number of a task to mark done");
    flag::Parse(argc, argv);
    if (!flag::Args().empty()) {
        fmt::Println(os::Stderr, "usage: json-file [-file path] [-add title] [-done n]");
        return 2;
    }

    auto [tasks, err] = load(file);
    if (err) {
        fmt::Println(os::Stderr, "json-file: {}", err);
        return 1;
    }
    if (!add.empty()) tasks.push_back({add, false});
    if (done > 0) {
        if (static_cast<std::size_t>(done) > tasks.size()) {
            fmt::Println(os::Stderr, "json-file: there's no task {}", done);
            return 1;
        }
        tasks[static_cast<std::size_t>(done) - 1].Done = true;
    }
    if (auto err = save(file, tasks)) {
        fmt::Println(os::Stderr, "json-file: {}", err);
        return 1;
    }

    fmt::Println("Tasks in {}:", file);
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        fmt::Println("{} [{}] {}", i + 1, tasks[i].Done ? "x" : " ", tasks[i].Title);
    }
}
