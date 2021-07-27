#include "logifix.h"
#include "javadoc.h"
#include "utils.h"
#include <fmt/core.h>
#include <nway.h>
#include <deque>
#include <filesystem>
#include <vector>
#include <unordered_set>
#include <thread>
#include <condition_variable>
#include <regex>
#include <mutex>
#include <iostream>

namespace logifix {

    namespace timer {

        using std::chrono::high_resolution_clock;
        using std::chrono::duration_cast;
        using std::chrono::microseconds;

        std::vector<std::chrono::time_point<std::chrono::high_resolution_clock>> start_times;
        std::vector<std::pair<std::string,size_t>> event_data;

        std::vector<std::pair<size_t, std::pair<std::string,size_t>>> events;

        std::mutex timer_mutex;

        size_t create(std::string name, size_t node_id) {
            std::unique_lock<std::mutex> lock(timer_mutex);
            auto id = start_times.size();
            start_times.emplace_back(high_resolution_clock::now());
            event_data.emplace_back(std::move(name), node_id);
            return id;
        }

        void stop(size_t id) {
            std::unique_lock<std::mutex> lock(timer_mutex);
            auto end = high_resolution_clock::now();
            auto diff = duration_cast<microseconds>(end - start_times[id]).count();
            events.emplace_back(diff, event_data[id]);
        }

    }

    std::unordered_map<std::string, node_id> file_to_node;
    std::unordered_map<node_id, std::string> node_to_file;

    std::vector<std::thread> thread_pool;

    std::condition_variable cv;
    size_t waiting_threads;
    std::deque<node_id> pending_files;
    std::deque<node_id> pending_strings;
    std::mutex work_mutex;
    std::unordered_map<node_id, std::pair<rule_id, node_id>> parent;
    std::unordered_map<node_id, std::unordered_map<rule_id, std::unordered_set<node_id>>> children;
    std::unordered_map<node_id, std::set<std::pair<rule_id, node_id>>> taken_transitions;

    size_t string_to_node_id(std::string str) {
        if (file_to_node.find(str) != file_to_node.end()) {
            return file_to_node[str];
        }
        auto node_id = file_to_node.size();
        file_to_node.emplace(str, node_id);
        node_to_file.emplace(node_id, str);
        return node_id;
    }

    size_t add_file(std::string file) {
        auto node_id = string_to_node_id(file);
        pending_files.emplace_front(node_id);
        return node_id;
    }

    std::optional<std::string> get_recursive_merge_result_for_node(node_id node) {

        /* Base case: no transitions from node */
        if (taken_transitions[node].empty()) {
            return node_to_file[node];
        }

        /* Case 1: only one transition from node */
        if (taken_transitions[node].size() == 1) {
            return get_recursive_merge_result_for_node(taken_transitions[node].begin()->second);
        }

        /* Case 2: Multiple transitions from node */

        std::vector<std::string> to_be_merged;
        for (auto [rule, next_node] : taken_transitions[node]) {
            auto result = get_recursive_merge_result_for_node(next_node);
            if (result) {
                to_be_merged.emplace_back(*result);
            }
        }

        auto diff = nway::diff(node_to_file[node], to_be_merged);

        /* Return empty optional if merging failed */
        if (nway::has_conflict(diff)) {
            std::cerr << "merging failed" << std::endl;
            return {};
        }

        return nway::merge(diff);
    }

    std::vector<std::pair<rule_id, std::string>> get_patches_for_file(std::string file) {
        std::vector<std::pair<rule_id, std::string>> rewrites;
        auto node = string_to_node_id(file);
        /* Go through the children of the node and collect all rewrites */
        for (auto [rule, rule_children] : children[node]) {
            for (auto child : rule_children) {
                auto result = get_recursive_merge_result_for_node(child);
                if (result) {
                    rewrites.emplace_back(rule, *result);
                }
            }
        }
        return rewrites;
    }

    void print_performance_metrics() { 
        std::map<std::string, size_t> time_per_event_type;
        std::map<node_id, size_t> time_per_node_id;
        for (auto [time, data] : timer::events) {
            auto [type, node_id] = data;
            time_per_event_type[type] += time;
            time_per_node_id[node_id] += time;
        }
        fmt::print("\n");
        for (auto [e, tot] : time_per_event_type) {
            fmt::print("{:20} {:20}\n", e, double(tot) / (1000.0 * 1000.0));
        }
        std::vector<std::pair<size_t, node_id>> node_data;
        for (auto [e, tot] : time_per_node_id) {
            node_data.emplace_back(tot, e);
        }
        std::sort(node_data.begin(), node_data.end());
        for (int i = 0; i < std::min(10ul, node_data.size()); i++) {
            auto [tot, e] = node_data[i];
            fmt::print("{:20} {:20}\n", e, double(tot) / (1000.0 * 1000.0));
        }
    }

    void run(std::function<void(node_id)> report_progress) {
        auto const concurrency = std::thread::hardware_concurrency();
        waiting_threads = 0;
        bool finished = false;
        for (size_t i = 0; i < concurrency; i++) {
            thread_pool.emplace_back(std::thread([&] {
                while (true) {
                    node_id current_node;
                    std::string current_node_source;
                    /* acquire work */
                    {
                        std::unique_lock<std::mutex> lock(work_mutex);
                        if (pending_strings.empty() && pending_files.empty()) {
                            waiting_threads++;
                            if (waiting_threads == concurrency) {
                                finished = true;
                                cv.notify_all();
                            } else {
                                auto wakeup_when = [&](){ return !pending_strings.empty() || finished; };
                                cv.wait(lock, wakeup_when);
                                waiting_threads--;
                            }
                        }
                        /* if we get to this point there is either work available or we are finished */
                        if (finished) return;
                        if (pending_strings.empty()) {
                            current_node = pending_files.front();
                            pending_files.pop_front();
                            report_progress(0);
                        } else {
                            current_node = pending_strings.front();
                            pending_strings.pop_front();
                        }
                        current_node_source = node_to_file[current_node];
                    }

                    std::vector<std::tuple<node_id, rule_id>> next_nodes;

                    /* perform expensive computation and store result */
                    {
                        auto rewrites = get_patches(current_node_source);
                        std::unique_lock<std::mutex> lock(work_mutex);
                        for (auto [rule, next_node_src] : rewrites) {
                            node_id next_node = string_to_node_id(next_node_src);
                            parent[next_node] = {rule, current_node};
                            children[current_node][rule].emplace(next_node);
                            next_nodes.emplace_back(next_node, rule);
                        }
                    }

                    for (const auto& [next_node, rule] : next_nodes) {
                        std::unique_lock<std::mutex> lock(work_mutex);
                        bool take_transition = true;
                        if (parent.find(current_node) != parent.end()) {
                            auto [parent_rule, parent_id] = parent[current_node];

                            auto parent_src = node_to_file[parent_id];
                            auto curr_src = node_to_file[current_node];
                            auto next_src = node_to_file[next_node];
                            lock.unlock();
                            auto diff = nway::diff(*sjp::lex(parent_src), {*sjp::lex(curr_src), *sjp::lex(next_src)});
                            std::string result;
                            for (auto& [o, cands] : diff) {
                                const auto& a = cands[0];
                                const auto& b = cands[1];
                                if (a == b) {
                                    for (auto& [type, content] : o) {
                                        result += content;
                                    }
                                } else {
                                    for (auto& [type, content] : b) {
                                        result += content;
                                    }
                                }
                            }
                            lock.lock();

                            auto id = string_to_node_id(result);

                            if (children[parent_id][rule].find(id) != children[parent_id][rule].end()) {
                                take_transition = false;
                            }

                        }
                        if (take_transition) {
                            pending_strings.emplace_back(next_node);
                            taken_transitions[current_node].emplace(rule, next_node);
                        }
                    }

                    /* notify all threads that there is more work available */
                    cv.notify_all();

                }
            }));
        }
        for (auto& t : thread_pool) {
            t.join();
        }
    }

/**
 * We are in the following scenario
 *
 *      (O)──(A)──(B)
 *       │
 *       └──(C)
 *
 * and we would like to know if there is some (C) that relates to (O) in
 * the way that (B) relates to (A), if that is the case, we do not want
 * to perform the transition, since these changes will be incorporated in
 * other branches
 */
bool edit_scripts_are_equal(std::string_view o, std::string_view a, std::string_view b, std::string_view c) {
    auto diff = nway::diff(*sjp::lex(o), {*sjp::lex(a), *sjp::lex(b), *sjp::lex(c)});
    auto ret = std::all_of(diff.begin(), diff.end(), [](const auto& hunk) {
        const auto& [oi, candidates] = hunk;
        const auto& ai = candidates[0];
        const auto& bi = candidates[1];
        const auto& ci = candidates[2];
        return ai == bi || bi == ci;
    });
    return ret;
}

/**
 * Given a source file, create a new Soufflé program, run the analysis, extract
 * and perform rewrites and finally return the set of resulting strings and the
 * rule ids for each rewrite.
 */
std::set<std::pair<rule_id,std::string>> get_patches(std::string source) {

    const char* program_name = "logifix";
    const char* filename = "file";

    //auto creation_timer = timer::create("program create", node_id);
    auto* prog = souffle::ProgramFactory::newInstance(program_name);
    //timer::stop(creation_timer);

    /* add javadoc info to prog */
    auto tokens = sjp::lex(source);
    if (tokens) {
        souffle::Relation* javadoc_references = prog->getRelation("javadoc_references");
        for (auto token : *tokens) { 
            if (std::get<0>(token) != sjp::token_type::multi_line_comment) continue;
            for (auto class_name : javadoc::get_classes(std::string(std::get<1>(token)))) {
                javadoc_references->insert(
                    souffle::tuple(javadoc_references, {prog->getSymbolTable().encode(filename),
                                              prog->getSymbolTable().encode(class_name)}));
            }
        }
    }


    /* add ast info to prog */
    sjp::parse(prog, filename, source.c_str());

    /* add source_code info to prog */
    souffle::Relation* source_code_relation = prog->getRelation("source_code");
    source_code_relation->insert(
        souffle::tuple(source_code_relation, {prog->getSymbolTable().encode(filename),
                                  prog->getSymbolTable().encode(source)}));

    /* run program */
    prog->run();

    /* extract rewrites */
    souffle::Relation* relation = prog->getRelation("rewrite");
    std::set<std::pair<std::string, std::string>> rewrites;

    for (souffle::tuple& output : *relation) {

        rule_id rule;
        std::string filename;
        int start;
        int end;
        std::string replacement;

        output >> rule >> filename >> start >> end >> replacement;

        rewrites.emplace(rule, source.substr(0, start) + replacement + source.substr(end));

    }

    delete prog;

    return rewrites;

}


} // namespace logifix
