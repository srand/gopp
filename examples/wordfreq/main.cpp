// wordfreq: prints the most common words of its input. A fiber counts each
// file into a map of its own and sends it on a channel, where main merges
// the maps. Words are runs of letters and digits in any script, by Go's
// Unicode tables, and are compared in lower case.
//
//     $ wordfreq -n 10 README.md docs/*.md
//     $ curl -s https://go.dev/doc/effective_go | wordfreq -min 5
//
//       1823  the
//        912  a
//        ...
//     9132 words, 1311 distinct, in 2 files
//
// Equal counts are sorted by word, so the output doesn't change from run
// to run.
#include <gopp/chan.hpp>
#include <gopp/cmp.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/maps.hpp>
#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/slices.hpp>
#include <gopp/strings.hpp>
#include <gopp/unicode.hpp>
#include <gopp/unicode/utf8.hpp>

#include <unordered_map>
#include <utility>
#include <vector>

using namespace gopp;

using counts = std::unordered_map<string, int64>;

struct FileCounts {
    string name;
    counts words;
    error err;
};

// A word's runes: letters and digits, and apostrophes inside words
// ("don't"), which FieldsFunc can't tell apart, so they're trimmed after.
bool notWord(rune r) { return !unicode::IsLetter(r) && !unicode::IsNumber(r) && r != '\''; }

FileCounts count(string name) {
    auto [data, err] = name == "-" ? io::ReadAll(os::Stdin) : os::ReadFile(name);
    if (err) return {name, {}, err};
    counts words;
    for (const string& field : strings::FieldsFunc(String(data), notWord)) {
        string word = strings::Trim(field, "'");
        if (!word.empty()) words[strings::ToLower(word)]++;
    }
    return {name, std::move(words), nullptr};
}

int main(int argc, char** argv) {
    isize top = 20, min = 1;
    flag::IntVar(&top, "n", top, "the number of words to print; 0 prints all");
    flag::IntVar(&min, "min", min, "print only words this long or longer, in runes");
    flag::Parse(argc, argv);
    std::vector<string> files = flag::Args();
    if (files.empty()) files.push_back("-");

    auto results = makeChan<FileCounts>();
    for (const string& name : files) {
        go([name, results] { results.send(count(name)); });
    }

    counts total;
    int status = 0;
    for (std::size_t i = 0; i < files.size(); i++) {
        FileCounts fc = results.recv();
        if (fc.err) {
            fmt::Println(os::Stderr, "wordfreq: {}", fc.err);
            status = 1;
            continue;
        }
        for (const auto& [word, n] : fc.words) total[word] += n;
    }

    // Drop the short words, then sort by count, most common first.
    maps::DeleteFunc(total, [&](const string& word, int64) { return utf8::RuneCountInString(word) < min; });
    std::vector<std::pair<string, int64>> ranked(total.begin(), total.end());
    slices::SortFunc(ranked, [](const auto& a, const auto& b) {
        return cmp::Or(cmp::Compare(b.second, a.second), cmp::Compare(a.first, b.first));
    });

    int64 words = 0;
    for (const auto& [_, n] : ranked) words += n;
    std::size_t shown = top > 0 ? std::min<std::size_t>(top, ranked.size()) : ranked.size();
    for (std::size_t i = 0; i < shown; i++) fmt::Println("{:>7}  {}", ranked[i].second, ranked[i].first);
    fmt::Println("{} words, {} distinct, in {} files", words, ranked.size(), files.size());
    return status;
}
