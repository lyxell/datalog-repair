#include "repair.h"
#include <iostream>

repair::repair() : program(souffle::ProgramFactory::newInstance("program")) {
    assert(program != nullptr);
}

node_ptr repair::get_ast(const char* filename) {
    return parser.get_ast(filename);
}

void repair::add_file(const char* filename) {
    filenames.emplace_back(filename);
    parser.add_file(filename);
}

void repair::add_string(const char* filename, const char* content) {
    souffle::Relation* source_code = program->getRelation("source_code");
    assert(source_code != nullptr);
    souffle::tuple tuple(source_code);
    tuple << std::string(content);
    source_code->insert(tuple);
    filenames.emplace_back(filename);
    parser.add_string(filename, content);
}

std::tuple<std::string, int, int>
repair::get_ast_node_from_id(const char* filename, int id) {
    const auto* record = program->getRecordTable().unpack(id, 3);
    assert(record != nullptr);
    return std::tuple(program->getSymbolTable().decode(record[0]), record[1],
                      record[2]);
}

int repair::ast_node_to_record(node_ptr node) {
    if (!node) {
        return 0;
    }
    std::array<souffle::RamDomain, 3> data = {
        program->getSymbolTable().encode(node->get_name()),
        node->get_start_token(), node->get_end_token()};
    return program->getRecordTable().pack(data.data(), 3);
}

int repair::vector_of_ast_nodes_to_record(const std::vector<node_ptr>& nodes,
                                          int offset) {
    if (offset == nodes.size()) {
        return 0;
    }
    std::array<souffle::RamDomain, 2> data = {
        ast_node_to_record(nodes[offset]),
        vector_of_ast_nodes_to_record(nodes, offset + 1)};
    return program->getRecordTable().pack(data.data(), 2);
}

void repair::insert_ast_node(node_ptr node) {
    auto* relation = program->getRelation("ast_node");
    assert(relation != nullptr);
    relation->insert(souffle::tuple(relation, {ast_node_to_record(node)}));
}

int repair::string_to_id(const std::string& str) {
    return program->getSymbolTable().encode(str);
}

void repair::insert_parent_of(node_ptr parent, std::string s, node_ptr child) {
    auto* relation = program->getRelation("parent_of");
    assert(relation != nullptr);
    relation->insert(
        souffle::tuple(relation, {ast_node_to_record(parent), string_to_id(s),
                                  ast_node_to_record(child)}));
}

void repair::insert_parent_of_list(node_ptr parent, std::string s,
                                   const std::vector<node_ptr>& children) {
    auto* relation = program->getRelation("parent_of_list");
    assert(relation != nullptr);
    relation->insert(
        souffle::tuple(relation, {ast_node_to_record(parent), string_to_id(s),
                                  vector_of_ast_nodes_to_record(children, 0)}));
}

void repair::insert_node_data(node_ptr node) {
    if (node == nullptr) {
        return;
    }
    insert_ast_node(node);
    for (const auto& [symbol, child] : node->get_parent_of()) {
        insert_parent_of(node, symbol, child);
        insert_node_data(child);
    }
    for (const auto& [symbol, children] : node->get_parent_of_list()) {
        insert_parent_of_list(node, symbol, children);
        for (const auto& child : children) {
            insert_node_data(child);
        }
    }
}

void repair::run() {
    parser.run();
    for (const auto& name : filenames) {
        insert_node_data(parser.get_ast(name.c_str()));
    }
    program->run();
    program->printAll();
}

std::vector<std::tuple<int, int, std::string>>
repair::get_possible_repairs(const char* filename) {
    std::vector<std::tuple<int, int, std::string>> result;
    souffle::Relation* relation = program->getRelation("rewrite");
    assert(relation != nullptr);
    for (souffle::tuple& output : *relation) {
        int id;
        std::string out;
        output >> id >> out;
        if (id == 0) {
            continue;
        }
        auto [str, a, b] = get_ast_node_from_id(filename, id);
        result.emplace_back(a, b, out);
    }
    return result;
}

node_ptr
repair::get_hovered_node(const char* filename, size_t buffer_position) {
    node_ptr curr = get_ast(filename);
    if (!curr) return curr;
    while (true) {
        node_ptr candidate = nullptr;
        for (const auto& [symbol, child] : curr->get_parent_of()) {
            if (!child) continue;
            if (child->get_start_token() <= buffer_position &&
                child->get_end_token()   >= buffer_position) {
                candidate = child; 
            }
        }
        for (const auto& [symbol, children] : curr->get_parent_of_list()) {
            for (const auto& child : children) {
                if (!child) continue;
                if (child->get_start_token() <= buffer_position &&
                    child->get_end_token()   >= buffer_position) {
                    candidate = child; 
                }
            }
        }
        if (!candidate) break;
        curr = candidate;
    }
    return curr;
}

std::vector<std::pair<std::string, std::tuple<std::string,int,int>>>
repair::get_variables_in_scope(const char* filename, size_t buffer_position) {
    auto hovered_node = get_hovered_node(filename, buffer_position);
    std::vector<std::pair<std::string, std::tuple<std::string,int,int>>> result;
    souffle::Relation* relation = program->getRelation("is_in_scope");
    assert(relation != nullptr);
    for (auto& output : *relation) {
        std::string variable;
        int type;
        int id;
        output >> variable >> id >> type;
        if (id == 0) {
            continue;
        }
        auto [name, start, end] = get_ast_node_from_id(filename, id);
        if (name == hovered_node->get_name()
         && start == hovered_node->get_start_token()
         && end == hovered_node->get_end_token())
            result.emplace_back(variable, get_ast_node_from_id(filename, type));
    }
    return result;
}
