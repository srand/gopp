// template: renders a Markdown report from plain C++ structs with
// text/template. The template reads the structs' fields by C++26
// reflection, with no registration, and calls a C++ lambda as a function.
//
//     $ template
//     # FRIDAY PROJECT UPDATE
//
//     ## go++ (Riley)
//     - [x] Ship the template example
//     - [ ] Polish the docs
//
//     ## Gopher garden (Sam)
//     - [x] Water the seedlings
//     - [x] Plant more beans
//
//     3 of 4 tasks done.
#include <gopp/fmt.hpp>
#include <gopp/os.hpp>
#include <gopp/strings.hpp>
#include <gopp/text/template.hpp>

#include <vector>

using namespace gopp;
namespace tmpl = text::tmpl;

struct Task {
    string Title;
    bool Done;
};

struct Project {
    string Name;
    string Owner;
    std::vector<Task> Tasks;
};

struct Report {
    string Title;
    std::vector<Project> Projects;

    // A method: templates call it as {{.Done}} like a field.
    int Done() const {
        int n = 0;
        for (const auto& p : Projects) {
            for (const auto& t : p.Tasks) n += t.Done;
        }
        return n;
    }
    int Total() const {
        int n = 0;
        for (const auto& p : Projects) n += int(p.Tasks.size());
        return n;
    }
};

const char* reportTemplate = R"(# {{.Title | upper}}
{{range .Projects}}
## {{.Name}} ({{.Owner}})
{{range .Tasks}}- [{{if .Done}}x{{else}} {{end}}] {{.Title}}
{{end}}{{end}}
{{.Done}} of {{.Total}} tasks done.
)";

int main() {
    Report report{
        "Friday project update",
        {
            {"go++", "Riley", {{"Ship the template example", true}, {"Polish the docs", false}}},
            {"Gopher garden", "Sam", {{"Water the seedlings", true}, {"Plant more beans", true}}},
        },
    };

    auto t = tmpl::Must(tmpl::New("report")
                            ->Funcs({{"upper", [](string s) { return strings::ToUpper(s); }}})
                            ->Parse(reportTemplate));
    if (auto err = t->Execute(os::Stdout, report)) {
        fmt::Println(os::Stderr, "template: {}", err);
        return 1;
    }
}
