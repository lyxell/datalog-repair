#pragma once

#include <cassert>
#include <fstream>
#include <iostream>
#include <souffle/SouffleInterface.h>
#include <streambuf>
#include <string>
#include <tuple>
#include <optional>

namespace logifix::parser {

enum class token_type {
    text_block,
    character_literal,
    string_literal,
    restricted,
    keyword,
    sep,
    op,
    integer_literal,
    floating_point_literal,
    boolean_literal,
    null_literal,
    identifier,
    single_line_comment,
    multi_line_comment,
    whitespace,
    eof
};

struct location {
    const char* filename;
    size_t begin;
    size_t end;
};

inline std::ostream& operator<<(std::ostream& os, const location& loc)
{
    os << loc.filename << ":" << loc.begin << "-" << loc.end;
    return os;
}

using token = std::pair<token_type, std::string>;
using token_collection = std::vector<token>;

std::optional<token_collection> lex(const std::string& content);

int parse(souffle::SouffleProgram* program, const char* filename, const char* content);

inline auto token_to_string(const token& token) -> std::string {
    const auto& [type, content] = token;
    return content;
}

inline std::string token_collection_to_string(const token_collection& tokens) {
    std::string result;
    for (const auto& [type, content] : tokens) {
        result += content;
    }
    return result;
}

} // namespace logifix::parser
