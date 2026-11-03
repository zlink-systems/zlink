/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace zlink::framework::tests
{
// A source gate, not a C++ lifetime proof: inspect closure reads following an
// explicit suspension. Await operands are evaluated before that suspension.
struct coroutine_scan_counts_t
{
    std::size_t files = 0;
    std::size_t parsed_files = 0;
    std::size_t lambdas = 0;
    std::size_t binding_regex_passes = 0;
    std::size_t regex_passes = 0;
    std::size_t declaration_visits = 0;
};
inline std::vector<std::size_t>
capturing_coroutine_reads (std::string source, coroutine_scan_counts_t *counts = nullptr)
{
    if (counts)
        ++counts->files;
    if (source.find ('[') == std::string::npos
        || (source.find ("co_await") == std::string::npos
            && source.find ("co_yield") == std::string::npos))
        return {};
    if (counts) {
        ++counts->parsed_files;
        counts->regex_passes += 4;
    }
    // Preserve offsets and line breaks while masking comments and literals,
    // including raw strings containing braces, captures or coroutine keywords.
    static const std::regex trivia (
      R"scan(\b[0-9][a-zA-Z_0-9'.]*|//[^\n]*|/\*[\s\S]*?\*/|(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\([\s\S]*?\)\1"|(?:u8|u|U|L)?"(?:\\[\s\S]|[^"\\])*"|(?:u8|u|U|L)?'(?:\\[\s\S]|[^'\\])*')scan");
    for (auto it = std::sregex_iterator (source.begin (), source.end (), trivia);
         it != std::sregex_iterator (); ++it) {
        for (auto p = static_cast<std::size_t> (it->position ());
             p < static_cast<std::size_t> (it->position () + it->length ()); ++p)
            if (source[p] != '\n')
                source[p] = ' ';
    }
    struct token_t
    {
        std::string text;
        std::size_t offset;
    };
    std::vector<token_t> tokens;
    static const std::regex token (R"([a-zA-Z_]\w*|::|->|[^\s])");
    for (auto it = std::sregex_iterator (source.begin (), source.end (), token);
         it != std::sregex_iterator (); ++it)
        tokens.push_back ({it->str (), static_cast<std::size_t> (it->position ())});
    const auto count = tokens.size ();
    std::vector<std::size_t> close (count, count);
    std::vector<std::size_t> stack;
    for (std::size_t i = 0; i < count; ++i) {
        const auto &t = tokens[i].text;
        if (t == "[" || t == "(" || t == "{")
            stack.push_back (i);
        else if ((t == "]" || t == ")" || t == "}") && !stack.empty ()) {
            close[stack.back ()] = i;
            stack.pop_back ();
        }
    }
    const auto identifier = [] (const std::string &s) {
        return std::isalpha (static_cast<unsigned char> (s[0])) || s[0] == '_';
    };
    const std::set<std::string> keywords{
      "co_await",   "co_return",    "co_yield", "return",      "if",
      "else",       "for",          "while",    "do",          "true",
      "false",      "nullptr",      "const",    "auto",        "void",
      "bool",       "int",          "unsigned", "long",        "double",
      "float",      "char",         "sizeof",   "static_cast", "reinterpret_cast",
      "const_cast", "dynamic_cast", "throw",    "try",         "catch",
      "switch",     "case",         "break",    "continue",    "typename",
      "struct",     "class",        "using",    "new",         "delete",
      "this"};
    // Resolve default captures against lexical declarations. Type names,
    // qualified members and coroutine-local variables are not captures.
    std::vector<token_t> declarations;
    static const std::regex declaration (
      R"((?:\b(?:const\s+)?[a-zA-Z_]\w*(?:::\w+)*(?:\s*<[^;{}=]*>)?[\s*&]+)([a-zA-Z_]\w*)\s*(?=[=;,:){\[]))");
    for (auto it = std::sregex_iterator (source.begin (), source.end (), declaration);
         it != std::sregex_iterator (); ++it) {
        const auto text = it->str ();
        const auto first = text.substr (0, text.find_first_of (" \t\n"));
        if (first == "co_return" || first == "return" || first == "throw")
            continue;
        const auto position = static_cast<std::size_t> (it->position (1));
        declarations.push_back ({(*it)[1].str (), position});
    }
    static const std::regex binding (R"(\bauto\s*[&]*\s*\[([^\]]+)\])");
    for (auto it = std::sregex_iterator (source.begin (), source.end (), binding);
         it != std::sregex_iterator (); ++it) {
        const auto position = static_cast<std::size_t> (it->position ());
        const auto names = (*it)[1].str ();
        if (counts)
            ++counts->binding_regex_passes;
        static const std::regex name (R"([a-zA-Z_]\w*)");
        for (auto n = std::sregex_iterator (names.begin (), names.end (), name);
             n != std::sregex_iterator (); ++n)
            declarations.push_back ({n->str (), position});
    }
    // Index coroutine parameters once per file. A reference argument remains
    // borrowed by the child coroutine even though the call precedes suspension.
    std::set<std::string> borrowing_coroutines;
    for (std::size_t i = 1; i + 1 < count; ++i) {
        if (tokens[i].text != "(" || !identifier (tokens[i - 1].text) || close[i] == count)
            continue;
        auto body = close[i] + 1;
        if (body >= count || tokens[body].text != "{" || close[body] == count)
            continue;
        bool reference = false;
        for (auto j = i + 1; j < close[i]; ++j)
            reference = reference || tokens[j].text == "&";
        if (!reference)
            continue;
        for (auto j = body + 1; j < close[body]; ++j)
            if (tokens[j].text == "co_await" || tokens[j].text == "co_yield") {
                borrowing_coroutines.insert (tokens[i - 1].text);
                break;
            }
    }
    std::sort (declarations.begin (), declarations.end (),
               [] (const auto &a, const auto &b) { return a.offset < b.offset; });
    std::size_t declaration_cursor = 0;
    std::set<std::string> outer_names;
    std::vector<std::size_t> found;
    for (std::size_t lambda = 0; lambda < count; ++lambda) {
        if (tokens[lambda].text != "[" || close[lambda] == count || close[lambda] == lambda + 1)
            continue;
        const auto capture_end = close[lambda];
        if (capture_end + 1 == count)
            continue;
        const auto &declarator = tokens[capture_end + 1].text;
        if (declarator != "(" && declarator != "{" && declarator != "<" && declarator != "mutable"
            && declarator != "noexcept" && declarator != "->" && declarator != "[")
            continue;
        auto body = capture_end + 1;
        // A lambda declarator ends at its body; skip parameter/default-value
        // groups, attributes and noexcept expressions without treating them as bodies.
        for (; body < count; ++body) {
            const auto &t = tokens[body].text;
            if (t == "{" || t == ";" || t == "=" || t == "]" || t == ")")
                break;
            if ((t == "(" || t == "[") && close[body] != count)
                body = close[body];
        }
        if (body == count || tokens[body].text != "{" || close[body] == count)
            continue;
        const auto end = close[body];
        if (counts)
            ++counts->lambdas;
        std::set<std::string> captures;
        std::set<std::string> references;
        bool default_capture = false;
        for (auto i = lambda + 1; i < capture_end;) {
            if (tokens[i].text == "=" || tokens[i].text == "&") {
                if (i + 1 == capture_end || tokens[i + 1].text == ",")
                    default_capture = true;
                ++i;
            }
            if (i < capture_end && tokens[i].text == "*")
                ++i;
            if (i < capture_end && identifier (tokens[i].text)) {
                captures.insert (tokens[i].text);
                if (tokens[i - 1].text == "&")
                    references.insert (tokens[i].text);
            }
            // Initializer identifiers belong to the initializer, not the capture.
            while (i < capture_end && tokens[i].text != ",") {
                if (close[i] != count)
                    i = close[i];
                ++i;
            }
            if (i < capture_end)
                ++i;
        }
        std::set<std::string> locals;
        while (declaration_cursor < declarations.size ()
               && declarations[declaration_cursor].offset < tokens[lambda].offset) {
            outer_names.insert (declarations[declaration_cursor++].text);
            if (counts)
                ++counts->declaration_visits;
        }
        for (auto d = declaration_cursor;
             d < declarations.size () && declarations[d].offset < tokens[end].offset; ++d) {
            if (counts)
                ++counts->declaration_visits;
            if (declarations[d].offset > tokens[capture_end].offset)
                locals.insert (declarations[d].text);
        }
        const auto capture_read = [&] (std::size_t i) {
            const auto &t = tokens[i].text;
            if (!identifier (t) || (keywords.count (t) && t != "this"))
                return false;
            const auto &previous = tokens[i - 1].text;
            const auto &next = tokens[i + 1].text;
            if (previous == "." || previous == "->" || previous == "::" || next == "::")
                return false;
            return captures.count (t)
                   || (captures.count ("this")
                       && (t == "this" || t[0] == '_' || (next == "(" && !locals.count (t))))
                   || (default_capture && !locals.count (t)
                       && (t == "this" || t[0] == '_' || outer_names.count (t)));
        };
        // A loop's next iteration executes after its previous suspension. Seed
        // that path only when the body can reach its back edge.
        std::set<std::size_t> back_edges;
        for (auto i = body + 1; i < end; ++i) {
            if (tokens[i].text != "for" && tokens[i].text != "while")
                continue;
            auto loop = i + 1;
            if (loop >= end || tokens[loop].text != "(" || close[loop] == count)
                continue;
            loop = close[loop] + 1;
            if (loop >= end)
                continue;
            const bool braced = tokens[loop].text == "{" && close[loop] != count;
            auto loop_end = braced ? close[loop] : loop;
            if (!braced)
                while (loop_end < end && tokens[loop_end].text != ";")
                    ++loop_end;
            bool awaits = false;
            bool terminal = false;
            unsigned depth = 0;
            for (auto j = loop + (braced ? 1 : 0); j < loop_end; ++j) {
                const auto &part = tokens[j].text;
                awaits = awaits || part == "co_await" || part == "co_yield";
                if (part == "{")
                    ++depth;
                if (part == "}" && depth)
                    --depth;
                if (!depth && (part == "co_return" || part == "break")
                    && (j == loop || tokens[j - 1].text == "{" || tokens[j - 1].text == ";"))
                    terminal = true;
            }
            if (awaits && !terminal)
                back_edges.insert (i);
        }
        struct block_t
        {
            std::size_t opening;
            bool entry_suspended;
            bool terminal;
            bool alternate_suspended;
        };
        std::vector<block_t> blocks;
        bool suspended = false;
        bool return_expression = false;
        bool alternate_suspended = false;
        for (auto i = body + 1; i < end; ++i) {
            const auto &t = tokens[i].text;
            if (back_edges.count (i))
                suspended = true;
            if (t == "{") {
                blocks.push_back ({i, suspended, false, alternate_suspended});
                alternate_suspended = false;
            } else if (t == "}" && !blocks.empty ()) {
                const auto block = blocks.back ();
                blocks.pop_back ();
                const bool exit_suspended = !block.terminal && suspended;
                if (i + 1 < end && tokens[i + 1].text == "else") {
                    alternate_suspended = exit_suspended;
                    suspended = block.entry_suspended;
                } else {
                    suspended = (block.terminal ? block.entry_suspended : suspended)
                                || block.alternate_suspended;
                }
            }
            if (t == "co_return") {
                // A return under an unbraced if does not terminate its block.
                return_expression = tokens[i - 1].text != ")" && tokens[i - 1].text != "else";
            }
            if (t == ";" && return_expression) {
                return_expression = false;
                if (blocks.empty ())
                    break;
                const auto block_end = close[blocks.back ().opening];
                auto alternative = i + 1;
                while (alternative < block_end && tokens[alternative].text != "case"
                       && tokens[alternative].text != "default")
                    ++alternative;
                if (alternative < block_end) {
                    suspended = blocks.back ().entry_suspended;
                    i = alternative - 1;
                    continue;
                }
                const bool catches = block_end + 1 < end && tokens[block_end + 1].text == "catch";
                blocks.back ().terminal = !catches;
                i = block_end - 1;
                continue;
            }
            if (t == "co_await" || t == "co_yield") {
                if (!suspended) {
                    // Skip the unary/postfix await operand, not the whole
                    // statement: `co_await f() + captured` reads after resume.
                    auto operand = i + 1;
                    while (operand < end
                           && (tokens[operand].text == "*" || tokens[operand].text == "&"
                               || tokens[operand].text == "!"))
                        ++operand;
                    if (operand < end) {
                        if (close[operand] != count)
                            operand = close[operand];
                        ++operand;
                    }
                    while (operand < end) {
                        const auto &part = tokens[operand].text;
                        if ((part == "(" || part == "[" || part == "{") && close[operand] != count)
                            operand = close[operand] + 1;
                        else if (part == "::" || part == "." || part == "->")
                            operand += 2;
                        else if (part == "<") {
                            unsigned depth = 1;
                            while (++operand < end && depth) {
                                if (tokens[operand].text == "<")
                                    ++depth;
                                if (tokens[operand].text == ">")
                                    --depth;
                            }
                        } else
                            break;
                    }
                    bool borrows = false;
                    for (auto j = i + 1; j < operand; ++j)
                        borrows = borrows || borrowing_coroutines.count (tokens[j].text);
                    if (borrows)
                        for (auto j = i + 1; j < operand; ++j)
                            if (capture_read (j) && tokens[j - 1].text != "*"
                                && tokens[j + 1].text != "->" && !references.count (tokens[j].text)
                                && (captures.count (tokens[j].text)
                                    || tokens[lambda + 1].text != "&")) {
                                found.push_back (tokens[j].offset);
                                break;
                            }
                    i = operand - 1;
                    suspended = true;
                }
                continue;
            }
            // Nested function/lambda bodies have their own coroutine context.
            // Keep scanning their capture initializers (which execute here),
            // but exclude their bodies from the enclosing coroutine's reads.
            if (t == "[" && close[i] != count) {
                auto nested = close[i] + 1;
                while (nested < end && tokens[nested].text != "{" && tokens[nested].text != ";") {
                    if (close[nested] != count)
                        nested = close[nested];
                    ++nested;
                }
                if (nested < end && tokens[nested].text == "{" && close[nested] != count) {
                    // Capture lists are reads of the enclosing closure, too.
                    if (suspended)
                        for (auto c = i + 1; c < close[i]; ++c)
                            if (captures.count (tokens[c].text)
                                || (default_capture && outer_names.count (tokens[c].text)
                                    && !locals.count (tokens[c].text))) {
                                found.push_back (tokens[c].offset);
                                break;
                            }
                    if (suspended) {
                        bool borrows = false;
                        for (auto c = i + 1; c < close[i]; ++c)
                            borrows = borrows || tokens[c].text == "&" || tokens[c].text == "=";
                        if (borrows)
                            for (auto c = nested + 1; c < close[nested]; ++c)
                                if (capture_read (c)) {
                                    found.push_back (tokens[c].offset);
                                    break;
                                }
                    }
                    i = close[nested];
                    continue;
                }
            }
            if (suspended && capture_read (i)) {
                found.push_back (tokens[i].offset);
                break;
            }
        }
    }
    return found;
}
} // namespace zlink::framework::tests
