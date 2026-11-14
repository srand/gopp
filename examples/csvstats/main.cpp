// csvstats: sums a column of a CSV file by the values of another, like
// SQL's GROUP BY, and writes the top groups as CSV. The first record names
// the columns.
//
//     $ cat sales.csv
//     date,region,product,amount
//     2026-11-01,north,"widgets, large",120.50
//     2026-11-01,south,gizmos,80
//     2026-11-02,north,gizmos,42.25
//     2026-11-02,east,widgets,310
//     $ csvstats -by region -sum amount sales.csv
//     region,amount,count,average
//     east,310.00,1,310.00
//     north,162.75,2,81.38
//     south,80.00,1,80.00
//
// encoding/csv reads the records (quoted fields may hold commas, quotes
// and newlines) and writes the result, quoting where needed. -n keeps the
// top n groups with a container/heap min-heap of n elements: each group
// goes in, and the smallest comes out once there are more than n.
#include <gopp/container/heap.hpp>
#include <gopp/encoding/csv.hpp>
#include <gopp/errors.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/os.hpp>
#include <gopp/strings.hpp>

#include <algorithm>
#include <charconv>
#include <map>
#include <vector>

using namespace gopp;

struct Group {
    string key;
    double sum = 0;
    int64 count = 0;
};

// The groups with the largest sums: a min-heap, so that the smallest is
// the one to drop.
struct TopGroups final : heap::Interface<Group> {
    std::vector<Group> groups;

    isize Len() override { return isize(groups.size()); }
    bool Less(isize i, isize j) override { return groups[i].sum < groups[j].sum; }
    void Swap(isize i, isize j) override { std::swap(groups[i], groups[j]); }
    void Push(Group g) override { groups.push_back(std::move(g)); }
    Group Pop() override {
        Group g = std::move(groups.back());
        groups.pop_back();
        return g;
    }
};

// The index of the column named name in the header record.
std::pair<std::size_t, error> column(const std::vector<string>& header, std::string_view name) {
    auto it = std::find(header.begin(), header.end(), name);
    if (it == header.end()) return {0, fmt::Errorf("no column named {}", strconv::Quote(name))};
    return {std::size_t(it - header.begin()), nullptr};
}

int main(int argc, char** argv) {
    string by, sum;
    isize top = 0;
    flag::StringVar(&by, "by", "", "the column to group by");
    flag::StringVar(&sum, "sum", "", "the numeric column to sum");
    flag::IntVar(&top, "n", 0, "write only the n groups with the largest sums; 0 writes all, by key");
    flag::Parse(argc, argv);
    if (by.empty() || sum.empty() || flag::Args().size() > 1) {
        fmt::Println(os::Stderr, "usage: csvstats -by column -sum column [-n top] [file.csv]");
        return 2;
    }

    ptr<io::Reader> in(&os::Stdin);
    if (flag::Args().size() == 1) {
        auto [f, err] = os::Open(flag::Args()[0]);
        if (err) {
            fmt::Println(os::Stderr, "csvstats: {}", err);
            return 1;
        }
        in = f;
    }
    auto r = csv::NewReader(in);
    r->ReuseRecord = true;  // Read reuses its vector: records aren't kept

    auto [header, err] = r->Read();
    if (err) {
        fmt::Println(os::Stderr, "csvstats: {}", err);
        return 1;
    }
    auto [byCol, byErr] = column(header, by);
    auto [sumCol, sumErr] = column(header, sum);
    if (error e = errors::Join({byErr, sumErr})) {
        fmt::Println(os::Stderr, "csvstats: {}", e);
        return 2;
    }

    std::map<string, Group, std::less<>> groups;
    for (;;) {
        auto [record, rerr] = r->Read();
        if (rerr == io::EOF_) break;
        if (rerr) {
            // A *csv::ParseError: "record on line 3: wrong number of fields".
            fmt::Println(os::Stderr, "csvstats: {}", rerr);
            return 1;
        }
        std::string_view field = record[sumCol];
        double value = 0;
        auto [end, ec] = std::from_chars(field.data(), field.data() + field.size(), value);
        if (ec != std::errc() || end != field.data() + field.size()) {
            auto [line, col] = r->FieldPos(int(sumCol));
            fmt::Println(os::Stderr, "csvstats: line {}, column {}: {} isn't a number", line, col, strconv::Quote(field));
            return 1;
        }
        Group& g = groups[record[byCol]];
        g.key = record[byCol];
        g.sum += value;
        g.count++;
    }

    std::vector<Group> rows;
    if (top > 0) {
        TopGroups h;
        for (auto& [_, g] : groups) {
            heap::Push(h, g);
            if (h.Len() > top) heap::Pop(h);  // the smallest
        }
        while (h.Len() > 0) rows.push_back(heap::Pop(h));
        std::reverse(rows.begin(), rows.end());  // largest first
    } else {
        for (auto& [_, g] : groups) rows.push_back(g);
    }

    auto w = csv::NewWriter(ptr<io::Writer>(&os::Stdout));
    w->Write({by, sum, "count", "average"});
    for (const Group& g : rows) {
        w->Write({g.key, fmt::Sprint("{:.2f}", g.sum), fmt::Sprint("{}", g.count), fmt::Sprint("{:.2f}", g.sum / double(g.count))});
    }
    w->Flush();
    if (error werr = w->Error()) {
        fmt::Println(os::Stderr, "csvstats: {}", werr);
        return 1;
    }
    return 0;
}
