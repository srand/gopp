// template: render a Markdown project report from ordinary C++ structs.
// Fields are discovered by C++26 reflection; upper is a custom template function.
#include <gopp/fmt.hpp>
#include <gopp/os.hpp>
#include <gopp/strings.hpp>
#include <gopp/text/template.hpp>

#include <vector>

using namespace gopp;
namespace tmpl = text::template_;

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
};

int main() {
    Report report{
        "Friday project update",
        {
            {"go++", "Riley", {{"Ship the template example", true}, {"Polish the docs", false}}},
            {"Gopher garden", "Sam", {{"Water the seedlings", true}, {"Plant more beans", true}}},
        },
    };

    tmpl::FuncMap funcs{
        {"upper", [](string s) { return strings::ToUpper(s); }},
    };
    auto base = tmpl::New("report");
    base->Funcs(funcs);
    auto [t, parse_err] = base->Parse(R"(# {{.Title | upper}}
{{range .Projects}}
## {{.Name}} — {{.Owner}}
{{range .Tasks}}- {{if .Done}}[x]{{else}}[ ]{{end}} {{.Title}}
{{end}}{{end}})");
    if (parse_err) {
        fmt::Println(os::Stderr, "template: {}", parse_err);
        return 1;
    }
    if (auto err = t->Execute(os::Stdout, report)) {
        fmt::Println(os::Stderr, "template: {}", err);
        return 1;
    }
}
