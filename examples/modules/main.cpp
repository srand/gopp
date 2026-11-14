// go++ as C++ modules: each header <gopp/x/y.hpp> is also the module
// gopp.x.y, and `import gopp;` imports all of them.
//
//     $ modules
//     {"Name":"gopher","Count":3}
//     application/json
//     gopher has 3 items
import std;
import gopp.encoding.json;
import gopp.fmt;
import gopp.mime;
import gopp.os;
import gopp.text.tmpl;

using namespace gopp;
namespace tmpl = text::tmpl;

struct Inventory {
    string Name;
    int Count;
};

int main() {
    Inventory inv{"gopher", 3};

    auto [b, err] = json::Marshal(inv);
    if (err) {
        fmt::Println("{}", err);
        return 1;
    }
    fmt::Println("{}", asStringView(b));
    fmt::Println("{}", mime::TypeByExtension(".json"));

    auto t = tmpl::Must(tmpl::New("inventory")->Parse("{{.Name}} has {{.Count}} items\n"));
    if (auto err = t->Execute(os::Stdout, inv)) {
        fmt::Println("{}", err);
        return 1;
    }
    return 0;
}
