// After spf13/cobra v1.10.2's flag_groups.go.
#include <gopp/cmd.hpp>
#include <gopp/fmt.hpp>
#include <gopp/strings.hpp>

#include "cmd/internal.hpp"

#include <algorithm>
#include <map>

namespace gopp::cmd {

namespace {

constexpr std::string_view requiredAsGroupAnnotation = "cobra_annotation_required_if_others_set";
constexpr std::string_view oneRequiredAnnotation = "cobra_annotation_one_required";
constexpr std::string_view mutuallyExclusiveAnnotation = "cobra_annotation_mutually_exclusive";

constexpr std::string_view annotations[] = {requiredAsGroupAnnotation, oneRequiredAnnotation, mutuallyExclusiveAnnotation};

// The groups (as their flag names joined by spaces) and the flags in each,
// whether they were set. A std::map: Go sorts the keys before reporting.
using groupStatusT = std::map<string, std::map<string, bool>>;

bool hasAllFlags(const pflag::FlagSet* fs, std::span<const string> flagnames) {
    for (auto& fname : flagnames) {
        if (!fs->Lookup(fname.view())) return false;
    }
    return true;
}

void processFlagForGroupAnnotation(const pflag::FlagSet* flags, const pflag::Flag* pf, std::string_view annotation,
                                   groupStatusT& groupStatus) {
    auto it = pf->Annotations.find(string(annotation));
    if (it == pf->Annotations.end()) return;
    for (auto& group : it->second) {
        if (!groupStatus.contains(group)) {
            auto names = strings::Split(group, " ");
            std::vector<string> flagnames(names.begin(), names.end());
            // Only consider this flag group at all if all the flags are
            // defined.
            if (!hasAllFlags(flags, flagnames)) continue;
            auto& st = groupStatus[group];
            for (auto& name : flagnames) st[name] = false;
        }
        groupStatus[group][pf->Name] = pf->Changed;
    }
}

// Go's %v of a []string: [a b].
std::string goList(const std::vector<string>& v) {
    std::string out = "[";
    for (std::size_t i = 0; i < v.size(); i++) {
        if (i > 0) out += ' ';
        out += v[i].view();
    }
    return out + "]";
}

error validateRequiredFlagGroups(const groupStatusT& data) {
    for (auto& [flagList, flagnameAndStatus] : data) {
        std::vector<string> unset;
        for (auto& [flagname, isSet] : flagnameAndStatus) {
            if (!isSet) unset.push_back(flagname);
        }
        if (unset.size() == flagnameAndStatus.size() || unset.empty()) continue;
        // Sort values, so they can be tested/scripted against
        // consistently.
        std::sort(unset.begin(), unset.end());
        return fmt::Errorf("if any flags in the group [{}] are set they must all be set; missing {}", flagList, goList(unset));
    }
    return nullptr;
}

error validateOneRequiredFlagGroups(const groupStatusT& data) {
    for (auto& [flagList, flagnameAndStatus] : data) {
        bool anySet = false;
        for (auto& [flagname, isSet] : flagnameAndStatus) anySet = anySet || isSet;
        if (anySet) continue;
        return fmt::Errorf("at least one of the flags in the group [{}] is required", flagList);
    }
    return nullptr;
}

error validateExclusiveFlagGroups(const groupStatusT& data) {
    for (auto& [flagList, flagnameAndStatus] : data) {
        std::vector<string> set;
        for (auto& [flagname, isSet] : flagnameAndStatus) {
            if (isSet) set.push_back(flagname);
        }
        if (set.size() <= 1) continue;
        std::sort(set.begin(), set.end());
        return fmt::Errorf("if any flags in the group [{}] are set none of the others can be; {} were all set", flagList, goList(set));
    }
    return nullptr;
}

}  // namespace

void Command::markFlagGroup(int kind, std::vector<string> flagNames) {
    detail::mergePersistentFlags(*this);
    string joined = strings::Join(flagNames, " ");
    for (auto& v : flagNames) {
        pflag::Flag* f = Flags()->Lookup(v.view());
        if (!f) {
            static constexpr std::string_view what[] = {"as being required in a flag group", "as being in a one-required flag group",
                                                        "as being in a mutually exclusive flag group"};
            panic(fmt::Sprint("Failed to find flag {} and mark it {}", strconv::Quote(v), what[kind]).view());
        }
        // Each time this is called is a single new entry; this allows it
        // to be a member of multiple groups if needed.
        auto values = f->Annotations[string(annotations[kind])];
        values.push_back(joined);
        if (auto err = Flags()->SetAnnotation(v.view(), annotations[kind], std::move(values))) panic(err.Error().view());
    }
}

error Command::ValidateFlagGroups() {
    if (DisableFlagParsing) return nullptr;
    pflag::FlagSet* flags = Flags();
    // groupStatus format is the list of flags as a unique ID, then a map of
    // each flag name and whether it is set or not.
    groupStatusT groupStatus, oneRequiredGroupStatus, mutuallyExclusiveGroupStatus;
    flags->VisitAll([&](pflag::Flag* pf) {
        processFlagForGroupAnnotation(flags, pf, requiredAsGroupAnnotation, groupStatus);
        processFlagForGroupAnnotation(flags, pf, oneRequiredAnnotation, oneRequiredGroupStatus);
        processFlagForGroupAnnotation(flags, pf, mutuallyExclusiveAnnotation, mutuallyExclusiveGroupStatus);
    });
    if (auto err = validateRequiredFlagGroups(groupStatus)) return err;
    if (auto err = validateOneRequiredFlagGroups(oneRequiredGroupStatus)) return err;
    if (auto err = validateExclusiveFlagGroups(mutuallyExclusiveGroupStatus)) return err;
    return nullptr;
}

namespace detail {

// Changes the flags for completion: the rest of a required-together group
// becomes required once one of them is set, a one-required group's flags
// are required until one is set, and the others of a mutually exclusive
// group are hidden once one is set.
void enforceFlagGroupsForCompletion(Command& c) {
    if (c.DisableFlagParsing) return;
    pflag::FlagSet* flags = c.Flags();
    groupStatusT groupStatus, oneRequiredGroupStatus, mutuallyExclusiveGroupStatus;
    flags->VisitAll([&](pflag::Flag* pf) {
        processFlagForGroupAnnotation(flags, pf, requiredAsGroupAnnotation, groupStatus);
        processFlagForGroupAnnotation(flags, pf, oneRequiredAnnotation, oneRequiredGroupStatus);
        processFlagForGroupAnnotation(flags, pf, mutuallyExclusiveAnnotation, mutuallyExclusiveGroupStatus);
    });
    // If a flag that is part of a group is present, we make all the other
    // flags of that group required so that the shell completion suggests
    // them automatically.
    for (auto& [flagList, flagnameAndStatus] : groupStatus) {
        for (auto& [_, isSet] : flagnameAndStatus) {
            if (isSet) {
                for (auto& fName : strings::Split(flagList, " ")) c.MarkFlagRequired(fName.view());
            }
        }
    }
    // If none of the flags of a one-required group are present, we make
    // all the flags of that group required so that the shell completion
    // suggests them automatically.
    for (auto& [flagList, flagnameAndStatus] : oneRequiredGroupStatus) {
        bool isSet = false;
        for (auto& [_, set] : flagnameAndStatus) {
            isSet = set;
            if (isSet) break;
        }
        // None of the flags of the group are set, mark all flags in the
        // group as required.
        if (!isSet) {
            for (auto& fName : strings::Split(flagList, " ")) c.MarkFlagRequired(fName.view());
        }
    }
    // If a flag that is mutually exclusive to others is present, we hide
    // the other flags of that group so the shell completion does not
    // suggest them.
    for (auto& [flagList, flagnameAndStatus] : mutuallyExclusiveGroupStatus) {
        for (auto& [flagName, isSet] : flagnameAndStatus) {
            if (isSet) {
                // One of the flags of the mutually exclusive group is set,
                // mark the other ones as hidden. Don't mark the flag that
                // is already set as hidden because it may be an array or
                // slice flag and therefore must continue being suggested.
                for (auto& fName : strings::Split(flagList, " ")) {
                    if (fName != flagName) flags->Lookup(fName.view())->Hidden = true;
                }
            }
        }
    }
}

}  // namespace detail

}  // namespace gopp::cmd
