#include "config.h"
#include "logifix.h"
#include "tty.h"
#include "utils.h"
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fmt/color.h>
#include <fmt/core.h>
#include <iostream>
#include <mutex>
#include <nway.h>
#include <set>
#include <stack>
#include <thread>
#include <tuple>

extern std::unordered_map<
    std::string, std::tuple<std::string, std::string, std::string, bool>>
    rule_data;

namespace cli {

using namespace std::string_literals;
namespace fs = std::filesystem;

using patch_collection =
    std::vector<std::tuple<std::string, std::string, std::string, bool>>;

struct options {
    bool accept_all;
    bool in_place;
    bool patch;
    bool verbose;
    bool enable_all;
    std::set<std::string> files;
    std::set<std::string> accepted;
};

enum class key { down, left, ret, right, up, unknown };

const char* TTY_CLEAR_TO_EOL = "\033[K";
const char* TTY_CURSOR_UP = "\033[A";
const char* TTY_HIDE_CURSOR = "\033[?25l";
const char* TTY_SHOW_CURSOR = "\033[?25h";
const char* TTY_MOVE_TO_BOTTOM = "\033[9999;1H";

std::string replace_tabs_with_spaces(const std::string& str) {
    std::string result;
    for (char c : str) {
        if (c == '\t') {
            result += "    ";
        } else {
            result += c;
        }
    }
    return result;
}

std::vector<std::string> create_patch(const std::string& filename,
                                      const std::string& before,
                                      const std::string& after) {
    auto a = utils::line_split(before);
    auto b = utils::line_split(after);
    for (auto& x : a) {
        x = utils::rtrim(x);
    }
    for (auto& x : b) {
        x = utils::rtrim(x);
    }
    std::vector<std::tuple<bool, char, std::string>> changes;
    std::vector<std::optional<size_t>> lcs = nway::lcs(a, b);
    size_t a_pos = 0;
    size_t b_pos = 0;
    while (a_pos < a.size() || b_pos < b.size()) {
        /* a and b agree */
        while (a_pos < a.size() && lcs[a_pos] && *lcs[a_pos] == b_pos) {
            changes.emplace_back(false, ' ', a[a_pos]);
            a_pos++;
            b_pos++;
        }
        /* a has no matching position */
        while (a_pos < a.size() && !lcs[a_pos]) {
            changes.emplace_back(false, '-', a[a_pos]);
            a_pos++;
        }
        /* a has matching position but it is not that of b_pos */
        while (a_pos < a.size() && lcs[a_pos] && *lcs[a_pos] != b_pos) {
            changes.emplace_back(false, '+', b[b_pos]);
            b_pos++;
        }
    }
    /* 3 lines of context */
    int context = 3;
    for (int i = 0; i < changes.size(); i++) {
        auto& [keep, marker, line] = changes[i];
        for (int j = std::max(0, i - context);
             j < std::min(int(changes.size()), i + context + 1); j++) {
            if (std::get<1>(changes[j]) != ' ') {
                keep = true;
            }
        }
    }
    /* create patch output */
    std::vector<std::string> result;
    result.emplace_back(
        fmt::format("diff --git a/{} b/{}", filename, filename));
    size_t a_pos_output = 1;
    size_t b_pos_output = 1;
    for (int i = 0; i < changes.size(); i++) {
        auto& [keep, marker, line] = changes[i];
        if (!keep) {
            a_pos_output++;
            b_pos_output++;
            continue;
        }
        size_t a_start = a_pos_output;
        size_t b_start = b_pos_output;
        size_t a_lines = 0;
        size_t b_lines = 0;
        size_t hunk_end = i;
        while (hunk_end < changes.size() && std::get<0>(changes[hunk_end])) {
            switch (std::get<1>(changes[hunk_end])) {
            case ' ':
                a_pos_output++;
                b_pos_output++;
                a_lines++;
                b_lines++;
                break;
            case '-':
                a_pos_output++;
                a_lines++;
                break;
            case '+':
                b_pos_output++;
                b_lines++;
                break;
            default:
                break;
            }
            hunk_end++;
        }
        /* output header */
        result.emplace_back(fmt::format("@@ -{},{} +{},{} @@", a_start, a_lines,
                                        b_start, b_lines));
        while (i < changes.size() && std::get<0>(changes[i])) {
            result.emplace_back(fmt::format("{}{}", std::get<1>(changes[i]),
                                            std::get<2>(changes[i])));
            i++;
        }
        i--;
    }
    return result;
}

std::vector<std::string> prettify_patch(const std::vector<std::string>& lines) {
    std::vector<std::pair<std::string, std::string>> columns;
    size_t line_number = 0;
    bool seen_header_before = false;
    // Skip first line (git header)
    for (size_t i = 1; i < lines.size(); i++) {
        const auto& line = lines[i];
        if (line.empty()) {
            continue;
        }
        auto marker = line[0];
        auto content = replace_tabs_with_spaces(line.substr(1));
        switch (marker) {
        case '-':
            columns.emplace_back("-", content);
            line_number++;
            break;
        case '+':
            columns.emplace_back("+", content);
            break;
        case '@':
            if (seen_header_before) {
                columns.emplace_back("...", "");
            }
            line_number =
                std::stoi(line.substr(line.find('-') + 1, line.find(',')));
            seen_header_before = true;
            break;
        default:
            columns.emplace_back(std::to_string(line_number), content);
            line_number++;
            break;
        }
    }
    size_t left_column_size = 0;
    for (const auto& [l, r] : columns) {
        left_column_size = std::max(left_column_size, l.size());
    }
    std::vector<std::string> result;
    const auto* format = "  {0:>{2}} {1}";
    for (auto& [l, r] : columns) {
        if (l == "+") {
            result.emplace_back(fmt::format(fg(fmt::terminal_color::green),
                                            format, l, r, left_column_size));
        } else if (l == "-") {
            result.emplace_back(fmt::format(fg(fmt::terminal_color::red),
                                            format, l, r, left_column_size));
        } else {
            result.emplace_back(fmt::format(format, l, r, left_column_size));
        }
    }
    return result;
}

std::string read_file(std::string_view path) {
    constexpr auto read_size = std::size_t{4096};
    auto stream = std::ifstream{path.data()};
    stream.exceptions(std::ios_base::badbit);
    auto out = std::string{};
    auto buf = std::string(read_size, '\0');
    while (stream.read(&buf[0], read_size)) {
        out.append(buf, 0, stream.gcount());
    }
    out.append(buf, 0, stream.gcount());
    return out;
}

key get_keypress() {
    switch (getchar()) {
    case 0x0d:
        return key::ret;
    case 'k':
        return key::up;
    case 'j':
        return key::down;
    case 'h':
        return key::left;
    case 'l':
        return key::right;
    case 0x1b:
        switch (getchar()) {
        case 0x5b:
            switch (getchar()) {
            case 0x41:
                return key::up;
            case 0x42:
                return key::down;
            case 0x44:
                return key::left;
            case 0x43:
                return key::right;
            default:
                break;
            }
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return key::unknown;
}

options parse_options(int argc, char** argv) {

    options opts = {
        .accept_all = false,
        .in_place = false,
        .patch = false,
        .verbose = false,
        .enable_all = false,
        .files = {},
        .accepted = {},
    };

    std::vector<std::tuple<std::string, std::function<void(const std::string&)>,
                           std::string>>
        flags;

    auto print_version_and_exit = []() {
        std::cout << PROJECT_VERSION << std::endl;
        std::exit(0);
    };

    auto print_usage = []() {
        fmt::print(fmt::emphasis::bold, "USAGE\n");
        fmt::print("  {} [flags] path [path ...]\n\n", PROJECT_NAME);
    };

    auto print_flags = [&flags]() {
        fmt::print(fmt::emphasis::bold, "FLAGS\n");
        for (const auto& [option, _, description] : flags) {
            std::cout << " " << std::setw(19) << std::left << option
                      << description << std::endl;
        }
        std::cout << std::endl;
    };

    auto print_examples = []() {
        fmt::print(fmt::emphasis::bold, "EXAMPLES\n\n");
        fmt::print("  {} src/main src/test\n\n", PROJECT_NAME);
        fmt::print(
            "  {} src/main --in-place --accept=S1125,S1155 Test.java\n\n",
            PROJECT_NAME);
    };

    auto parse_accepted = [&](const std::string& str) {
        std::stringstream ss(str);
        while (ss.good()) {
            std::string substr;
            std::getline(ss, substr, ',');
            opts.accepted.emplace(substr);
        }
    };

    flags = {
        {"--accept-all",
         [&](const std::string& str) { opts.accept_all = true; },
         "Accept all patches without asking"},
        {"--accept=<rules>",
         [&](const std::string& str) { parse_accepted(str); },
         "Comma-separated list of rules to accept"},
        {"--enable-all",
         [&](const std::string& str) { opts.enable_all = true; },
         "Enable rules that are disabled by default"},
        {"--in-place", [&](const std::string& str) { opts.in_place = true; },
         "Disable interaction, rewrite files on disk"},
        {"--patch", [&](const std::string& str) { opts.patch = true; },
         "Disable interaction, output a patch to stdout"},
        {"--help",
         [&](const std::string& str) {
             print_usage();
             print_flags();
             print_examples();
             std::exit(0);
         },
         "Print this information and exit"},
        {"--verbose", [&](const std::string& str) { opts.verbose = true; },
         "Print debugging information"},
        {"--version", [&](const std::string& str) { print_version_and_exit(); },
         "Print version information and exit"},
    };

    std::vector<std::string> arguments;
    for (auto i = 1; i < argc; i++) {
        arguments.emplace_back(argv[i]);
    }
    std::reverse(arguments.begin(), arguments.end());

    while (!arguments.empty()) {
        auto argument = arguments.back();
        if (argument.size() < 2 || argument.substr(0, 2) != "--") {
            break;
        }
        auto found = false;
        for (const auto& [option, fn, description] : flags) {
            if (option.find('=') != std::string::npos) {
                auto opt_part = option.substr(0, option.find('=') + 1);
                if (argument.size() >= opt_part.size() &&
                    argument.substr(0, opt_part.size()) == opt_part) {
                    fn(argument.substr(opt_part.size()));
                    found = true;
                    break;
                }
            } else {
                if (argument.substr(0, option.size()) == option) {
                    fn("");
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            fmt::print("Error: Found invalid flag '{}'\n\n", argument);
            print_usage();
            print_flags();
            std::exit(1);
        }
        arguments.pop_back();
    }

    if (arguments.empty()) {
        print_usage();
        print_flags();
        print_examples();
        std::exit(0);
    }

    while (!arguments.empty()) {
        auto argument = arguments.back();
        if (!fs::exists(argument)) {
            fmt::print("Error: Path '{}' does not exist\n\n", argument);
            print_usage();
            std::exit(1);
        }
        if (fs::is_directory(argument)) {
            for (const auto& entry :
                 fs::recursive_directory_iterator(argument)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                if (entry.path().extension() != ".java") {
                    continue;
                }
                std::string s = entry.path().lexically_normal();
                opts.files.emplace(entry.path().lexically_normal());
            }
        } else {
            opts.files.emplace(
                fs::path(std::move(argument)).lexically_normal());
        }
        arguments.pop_back();
    }

    return opts;
}

int multi_choice(const std::string& question,
                 const std::vector<std::string>& alternatives,
                 bool exit_on_left = false) {
    tty::enable_cbreak_mode();
#ifndef NDEBUG
    fmt::print(fg(fmt::terminal_color::red) | fmt::emphasis::bold, "DEBUG ");
#endif
    fmt::print(fg(fmt::terminal_color::green) | fmt::emphasis::bold, "?");
    fmt::print(fmt::emphasis::bold, " {} ", question);
    if (exit_on_left) {
        fmt::print("[Use arrows to move, left to finish]");
    } else {
        fmt::print("[Use arrows to move]");
    }
    constexpr auto HEIGHT = size_t{15};
    auto cursor = 0;
    auto scroll = 0;
    auto found = false;
    while (true) {
        if (cursor != -1 && cursor < scroll) {
            scroll = cursor;
        } else if (cursor != -1 && cursor == scroll + HEIGHT) {
            scroll = cursor - HEIGHT + 1;
        }
        for (auto i = scroll;
             i < std::min(alternatives.size(), scroll + HEIGHT); i++) {
            if (cursor == i) {
                fmt::print("\n> {}", alternatives[i]);
            } else {
                fmt::print("\n  {}", alternatives[i]);
            }
            std::cout << TTY_CLEAR_TO_EOL;
        }
        if (found) {
            std::cout << std::endl;
            std::cout << std::endl;
            tty::disable_cbreak_mode();
            return cursor;
        }
        for (auto i = scroll;
             i < std::min(alternatives.size(), scroll + HEIGHT); i++) {
            std::cout << TTY_CURSOR_UP;
        }
        auto key_press = get_keypress();
        if (key_press == key::left && exit_on_left) {
            cursor = -1;
            found = true;
        } else if (key_press == key::up && cursor > 0) {
            cursor--;
        } else if (key_press == key::down && cursor + 1 < alternatives.size()) {
            cursor++;
        } else if (key_press == key::ret || key_press == key::right) {
            found = true;
        }
    }
    return 0;
}

std::string post_process_remove_introduced_empty_lines(std::string before,
                                                       std::string after) {
    auto before_lines = utils::line_split(before);
    auto after_lines = utils::line_split(after);
    auto lcs = nway::lcs(after_lines, before_lines);
    std::string result;
    for (size_t i = 0; i < after_lines.size(); i++) {
        if (!lcs[i] && utils::string_has_only_whitespace(after_lines[i])) {
            continue;
        }
        result += after_lines[i];
    }
    return result;
}

std::string post_process_harmonize_line_terminators(const std::string& before,
                                                    const std::string& after) {
    auto line_terminator = utils::detect_line_terminator(before);
    std::string result;
    for (auto line : utils::line_split(after)) {
        if (line_terminator == "\r\n") {
            if (!utils::ends_with(line, "\r\n")) {
                line.pop_back();
                line += "\r\n";
            }
        }
        result += line;
    }
    return result;
}

std::string post_process_auto_indent(std::string before,
                                     const std::string& after) {
    auto bracket_balance = [](const std::string& str) {
        int result = 0;
        for (char c : str) {
            if (c == '(' || c == '{') {
                result++;
            } else if (c == ')' || c == '}') {
                result--;
            }
        }
        return result;
    };
    auto before_lines = utils::line_split(before);
    auto after_lines = utils::line_split(after);
    auto lcs = nway::lcs(after_lines, before_lines);
    auto indentation = utils::detect_indentation(before);
    std::string result;
    for (size_t i = 0; i < after_lines.size(); i++) {
        auto& line = after_lines[i];
        if (i > 0 && !lcs[i] &&
            utils::find_first_non_space(line) == line.begin()) {
            auto prev_line = after_lines[i - 1];
            // get indent from previous line
            auto new_indent = prev_line.substr(
                0, utils::find_first_non_space(prev_line) - prev_line.begin());
            // increase indent if previous line has an open bracket
            if (bracket_balance(prev_line) > 0) {
                new_indent += indentation;
            }
            // decrease indent if we have closing bracket
            if (bracket_balance(after_lines[i]) < 0) {
                new_indent = new_indent.substr(
                    std::min(indentation.size(), new_indent.size()));
            }
            line = new_indent + line;
        }
        result += line;
    }
    return result;
}

std::string post_process_sort_imports(std::string before, std::string after) {
    auto before_lines = utils::line_split(before);
    auto after_lines = utils::line_split(after);
    auto lcs = nway::lcs(after_lines, before_lines);
    std::vector<std::string> result;
    std::vector<std::string> imports;
    bool other_imports = false;
    size_t last_import_at = 0;
    for (size_t i = 0; i < after_lines.size(); i++) {
        if (!lcs[i] && utils::starts_with(after_lines[i], "import")) {
            imports.emplace_back(after_lines[i]);
        } else {
            if (utils::starts_with(after_lines[i], "import")) {
                other_imports = true;
                last_import_at = i - imports.size();
            }
            result.emplace_back(after_lines[i]);
        }
    }
    if (!other_imports) {
        return after;
    }
    std::sort(imports.begin(), imports.end());
    for (const auto& import : imports) {
        for (size_t i = 0; i < result.size(); i++) {
            if (utils::starts_with(result[i], "import") && import < result[i]) {
                result.insert(result.begin() + i, import);
                break;
            }
            if (i > 0 && utils::starts_with(result[i - 1], "import") &&
                !utils::starts_with(result[i], "import") &&
                import > result[i]) {
                result.insert(result.begin() + i, import);
                break;
            }
        }
    }
    std::string result_str;
    for (const auto& x : result) {
        result_str += x;
    }
    return result_str;
}

std::string post_process(const std::string& before, std::string after) {
    after = post_process_remove_introduced_empty_lines(before, after);
    after = post_process_harmonize_line_terminators(before, after);
    after = post_process_auto_indent(before, after);
    after = post_process_sort_imports(before, after);
    return after;
}

std::map<std::string, std::string> get_results(
    const std::set<logifix::patch_id>& accepted_patches,
    const std::unordered_map<logifix::node_id, std::string>& filename_of_node) {
    std::map<std::string, std::string> results;
    for (auto [id, filename] : filename_of_node) {
        std::vector<logifix::patch_id> accepted_patches_for_file;
        for (auto patch : logifix::get_patches_for_file(id)) {
            if (accepted_patches.find(patch) != accepted_patches.end()) {
                accepted_patches_for_file.emplace_back(patch);
            }
        }
        if (!accepted_patches_for_file.empty()) {
            auto before = cli::read_file(filename);
            auto after = logifix::get_result(id, accepted_patches_for_file);
            results.emplace(filename, post_process(before, after));
        }
    }
    return results;
}

} // namespace cli

void at_signal(int signal) { std::exit(1); }

void at_exit() {
    std::cerr << cli::TTY_MOVE_TO_BOTTOM;
    std::cerr << cli::TTY_SHOW_CURSOR << std::endl;
    tty::disable_cbreak_mode();
}

int main(int argc, char** argv) {

    setenv("SOUFFLE_ALLOW_SIGNALS", "", 1);
    std::signal(SIGINT, at_signal);
    std::atexit(at_exit);

    std::cerr << cli::TTY_HIDE_CURSOR;

    cli::options options = cli::parse_options(argc, argv);

    std::set<logifix::patch_id> accepted_patches;

    std::unordered_map<logifix::node_id, std::string> filename_of_node;

    for (const auto& file : options.files) {
        auto node_id = logifix::add_file(cli::read_file(file));
        filename_of_node[node_id] = file;
    }

    if (!options.enable_all) {
        for (const auto& [rule, data] : rule_data) {
            if (std::get<3>(data)) {
                logifix::disable_rule(rule);
            }
        }
    }

    size_t count = 0;

    logifix::run([&count, &options](size_t node) {
        count++;
        int progress = int((double(count) / double(options.files.size())) * 40);
        int progress_full = 40;
        fmt::print(stderr, "\r[{2:=^{0}}{2: ^{1}}] {3}/{4}", progress,
                   progress_full - progress, "", count, options.files.size());
    });

    logifix::print_performance_metrics();

    auto review = [&options, &accepted_patches, &filename_of_node](
                      logifix::patch_id patch, size_t curr, size_t total) {
        // auto& [filename, rule, after, accepted] = rw;
        auto [rule, node_id, after] = logifix::get_patch_data(patch);
        auto filename = filename_of_node[node_id];
        fmt::print(fmt::emphasis::bold, "\nPatch {}/{} • {}\n\n", curr, total,
                   filename);
        std::string before = cli::read_file(filename);
        auto diff = cli::create_patch(filename, before,
                                      cli::post_process(before, after));
        if (!diff.empty()) {
            for (const auto& line : cli::prettify_patch(diff)) {
                std::cout << line << std::endl;
            }
        }
        std::cout << std::endl;
        auto choice =
            cli::multi_choice("What would you like to do?",
                              {"Accept this patch", "Reject this patch"}, true);
        if (choice == -1) {
            return false;
        }
        if (choice == 0) {
            accepted_patches.insert(patch);
        } else if (choice == 1) {
            accepted_patches.erase(patch);
        }
        return true;
    };

    if (!options.patch && !options.in_place) {

        while (true) {

            int selection;

            if (!accepted_patches.empty()) {
                fmt::print(fmt::emphasis::bold, "\nSelected {}/{} patches\n\n",
                           accepted_patches.size(),
                           logifix::get_all_patches().size());
                selection = cli::multi_choice(
                    "What would you like to do?",
                    {
                        "Review patches by rule",
                        "Show a diff of the current changes",
                        "Apply changes to files on disk and exit",
                        "Discard changes and exit",
                    });

                if (selection == 1) {
                    auto* fp = popen("less -R", "w");
                    if (fp != nullptr) {
                        for (auto [filename, after] : cli::get_results(
                                 accepted_patches, filename_of_node)) {
                            fmt::print(fp, "\n{}\n\n", filename);
                            std::string before = cli::read_file(filename);
                            auto patch =
                                cli::create_patch(filename, before, after);
                            for (const auto& line :
                                 cli::prettify_patch(patch)) {
                                fputs(line.c_str(), fp);
                                fputs("\n", fp);
                            }
                        }
                        pclose(fp);
                    }
                }
                if (selection == 2) {
                    options.in_place = true;
                    break;
                }
                if (selection == 3) {
                    break;
                }
            } else {
                fmt::print(fmt::emphasis::bold,
                           "\n\nAnalyzed {} files and found {} patches\n\n",
                           options.files.size(),
                           logifix::get_all_patches().size());

                if (logifix::get_all_patches().empty()) {
                    break;
                }

                selection = cli::multi_choice("What would you like to do?",
                                              {
                                                  "Review patches by rule",
                                                  "Exit without doing anything",
                                              });
                if (selection == 1) {
                    break;
                }
            }

            while (true) {
                if (selection == 0) {
                    std::vector<
                        std::tuple<std::string, std::string, std::string>>
                        columns;
                    for (auto [rule, data] : rule_data) {
                        auto [sqid, pmdid, description, disabled] = data;
                        std::vector<logifix::patch_id> patches =
                            logifix::get_patches_for_rule(rule);
                        if (patches.empty()) {
                            continue;
                        }
                        size_t accepted_count = std::count_if(
                            patches.begin(), patches.end(),
                            [&accepted_patches](logifix::patch_id patch) {
                                return accepted_patches.find(patch) !=
                                       accepted_patches.end();
                            });
                        if (accepted_count > 0) {
                            columns.emplace_back(
                                rule, description,
                                fmt::format(fg(fmt::terminal_color::green),
                                            "{}/{}", accepted_count,
                                            patches.size()));
                        } else {
                            columns.emplace_back(rule, description,
                                                 fmt::format("{}/{}",
                                                             accepted_count,
                                                             patches.size()));
                        }
                    }
                    size_t left_column_width = 0;
                    for (auto [rule, l, r] : columns) {
                        left_column_width =
                            std::max(left_column_width, l.size());
                    }
                    std::vector<std::string> options;
                    options.reserve(columns.size());
                    for (auto [rule, l, r] : columns) {
                        options.emplace_back(fmt::format("{0:<{2}}    {1}", l,
                                                         r, left_column_width));
                    }
                    auto rule_selection = cli::multi_choice(
                        "Which rule would you like to review?", options, true);
                    if (rule_selection == -1) {
                        break;
                    }
                    auto rule = std::get<0>(columns[rule_selection]);
                    auto patches = logifix::get_patches_for_rule(rule);
                    for (size_t i = 0; i < patches.size(); i++) {
                        if (!review(patches[i], i + 1, patches.size())) {
                            break;
                        }
                    }
                } else {
                    break;
                }
            }
        }

    } else {
        if (options.accept_all) {
            for (auto patch : logifix::get_all_patches()) {
                accepted_patches.insert(patch);
            }
        } else if (!options.accepted.empty()) {
            for (auto& rule : options.accepted) {
                for (auto patch : logifix::get_patches_for_rule(rule)) {
                    accepted_patches.insert(patch);
                }
            }
        }
    }

    for (auto [filename, after] :
         cli::get_results(accepted_patches, filename_of_node)) {
        if (options.in_place) {
            std::ofstream f(filename);
            f << after;
            f.close();
        } else if (options.patch) {
            std::string before = cli::read_file(filename);
            auto patch = cli::create_patch(filename, before, after);
            for (const auto& line : patch) {
                std::cout << line << std::endl;
            }
        }
    }

    return 0;
}
