// DBSP Parser Extension for CREATE/DROP MATERIALIZED VIEW
// Provides native SQL syntax for materialized views

#pragma once

#include "duckdb.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/function/table_function.hpp"

#include <cstring>
#include <iostream>

namespace dbsp_native {

using namespace duckdb;

// DuckDB 2.0 replaced the PostgreSQL-derived parser with a PEG parser, and
// with it the parse hook's contract: instead of the raw statement text the
// extension now receives the tokenized tail of the query from the PEG failure
// point (SimpleToken = raw source slice + classified type) and reports, via
// ParserExtensionParseResult::consumed_tokens, how many leading tokens it
// claimed (>0 accepted, 0 not ours, <0 raise `error`).
//
// SimpleToken::text is the verbatim source slice (quotes and all), so joining
// the tokens with single spaces rebuilds an equivalent statement for the
// text-based parsers below. Only insignificant whitespace and comments are
// normalized away. The whole tail is claimed, matching the pre-2.0 hook, which
// was handed — and consumed — the entire remaining query string.
inline string dbsp_tokens_to_query(const vector<SimpleToken> &tokens) {
    string query;
    for (auto &token : tokens) {
        if (token.type == TokenType::TERMINATOR || token.type == TokenType::END_OF_INPUT ||
            token.type == TokenType::END_OF_INPUT_AUTOCOMPLETE || token.type == TokenType::COMMENT) {
            continue;
        }
        if (!query.empty()) {
            query += " ";
        }
        query += token.text;
    }
    return query;
}

//===--------------------------------------------------------------------===//
// parser_override: the RAW-TEXT route
//===--------------------------------------------------------------------===//
//
// DuckDB 2.0 gained `ParserExtension::parser_override`, which receives the
// query TEXT and runs BEFORE the core PEG grammar (duckdb/src/parser/parser.cpp
// ParseQuery). That buys two things the token-reconstruction path above cannot:
//
//  1. **Byte-exact SQL.** The token path rebuilds the statement by joining
//     token slices with single spaces, so `dbsp_views()` reports a normalised
//     string and every comment inside the SELECT is gone. Here the SELECT body
//     is a substring of what the user typed.
//  2. **`DROP MATERIALIZED VIEW` is reachable again.** The 2.0 PEG grammar
//     CLAIMS that statement (`drop.gram`) and its transformer then throws
//     `NotImplementedException: Cannot drop MATERIALIZED VIEW yet`, so the
//     parse_function hook — which only ever sees statements the PEG parser
//     FAILED on — never saw it. Running before the grammar takes it back.
//
// It is not unconditional. `parser_override` callbacks are skipped unless
// `allow_parser_override_extension` is FALLBACK or STRICT, and DuckDB's default
// is DEFAULT (skip). The extension raises it to FALLBACK at load; a user who
// sets it back gets the token path above, which parses the same DDL and builds
// the same view — only the stored text is normalised. That is why BOTH paths
// exist and neither is deleted.

// Case-insensitive substring search that allocates nothing. `needle` must
// already be upper-case.
inline bool dbsp_contains_ci(const string &haystack, const char *needle) {
    const size_t n = strlen(needle);
    if (haystack.size() < n) {
        return false;
    }
    for (size_t i = 0; i + n <= haystack.size(); i++) {
        size_t j = 0;
        while (j < n && StringUtil::CharacterToUpper(haystack[i + j]) == needle[j]) {
            j++;
        }
        if (j == n) {
            return true;
        }
    }
    return false;
}

// True when `kw` appears at `pos` case-insensitively AND is followed by a
// non-identifier character (so "CREATED" does not match "CREATE").
inline bool dbsp_keyword_at(const string &q, size_t pos, const char *kw) {
    const size_t n = strlen(kw);
    if (pos + n > q.size()) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (StringUtil::CharacterToUpper(q[pos + i]) != kw[i]) {
            return false;
        }
    }
    if (pos + n == q.size()) {
        return true;
    }
    const char next = q[pos + n];
    return !(std::isalnum(static_cast<unsigned char>(next)) || next == '_');
}

// Advance past whitespace, `-- line` comments and `/* block */` comments.
inline size_t dbsp_skip_ws_comments(const string &q, size_t pos) {
    while (pos < q.size()) {
        if (std::isspace(static_cast<unsigned char>(q[pos]))) {
            pos++;
        } else if (q.compare(pos, 2, "--") == 0) {
            const size_t nl = q.find('\n', pos);
            pos = (nl == string::npos) ? q.size() : nl + 1;
        } else if (q.compare(pos, 2, "/*") == 0) {
            const size_t end = q.find("*/", pos + 2);
            pos = (end == string::npos) ? q.size() : end + 2;
        } else {
            break;
        }
    }
    return pos;
}

// Advance past one possibly-qualified, possibly-quoted identifier. Returns
// `pos` unchanged when there is no identifier there.
inline size_t dbsp_skip_identifier(const string &q, size_t pos) {
    const size_t start = pos;
    while (true) {
        if (pos < q.size() && q[pos] == '"') {
            pos++;
            while (pos < q.size()) {
                if (q[pos] == '"') {
                    if (pos + 1 < q.size() && q[pos + 1] == '"') {
                        pos += 2; // "" is an escaped quote inside the identifier
                        continue;
                    }
                    pos++;
                    break;
                }
                pos++;
            }
        } else {
            const size_t part = pos;
            while (pos < q.size() &&
                   (std::isalnum(static_cast<unsigned char>(q[pos])) ||
                    q[pos] == '_' || q[pos] == '$')) {
                pos++;
            }
            if (pos == part) {
                return start; // nothing consumed: not an identifier
            }
        }
        if (pos < q.size() && q[pos] == '.') {
            pos++;
            continue; // qualified name: keep going
        }
        return pos;
    }
}

// If a dollar-quote opens at `pos` ($$ or $tag$), return its full opening
// tag; otherwise "". A tag is letters, digits and underscores, not starting
// with a digit — DuckDB follows PostgreSQL here.
inline string dbsp_dollar_tag_at(const string &q, size_t pos) {
    if (pos >= q.size() || q[pos] != '$') {
        return "";
    }
    size_t i = pos + 1;
    while (i < q.size() && (std::isalnum(static_cast<unsigned char>(q[i])) ||
                            q[i] == '_')) {
        i++;
    }
    if (i >= q.size() || q[i] != '$') {
        return "";
    }
    if (i > pos + 1 && std::isdigit(static_cast<unsigned char>(q[pos + 1]))) {
        return ""; // a tag may not start with a digit
    }
    return q.substr(pos, i - pos + 1);
}

// Index of the first statement-terminating `;` at or after `pos`, skipping
// string literals, quoted identifiers, DOLLAR-QUOTED bodies and comments;
// q.size() when there is none. Needed because "the rest of the text is the
// SELECT" is only true up to a terminator a user wrote.
//
// Dollar quoting is not decoration: `SELECT $$semi;colon$$ AS s` stopped at
// the embedded `;`, the rewrite was declined, and the statement fell back to
// the token path with normalised text and nothing saying why (measured:
// stored `SELECT $$semi;colon$$ AS s , id FROM t`).
inline size_t dbsp_find_statement_end(const string &q, size_t pos) {
    while (pos < q.size()) {
        const char c = q[pos];
        if (c == ';') {
            return pos;
        }
        const string tag = dbsp_dollar_tag_at(q, pos);
        if (!tag.empty()) {
            const size_t close = q.find(tag, pos + tag.size());
            // An unterminated dollar quote is a syntax error the core parser
            // will report; treat the rest as body rather than guessing.
            pos = (close == string::npos) ? q.size() : close + tag.size();
            continue;
        }
        if (c == '\'' || c == '"') {
            const char quote = c;
            pos++;
            while (pos < q.size()) {
                if (q[pos] == quote) {
                    if (pos + 1 < q.size() && q[pos + 1] == quote) {
                        pos += 2;
                        continue;
                    }
                    pos++;
                    break;
                }
                pos++;
            }
            continue;
        }
        if (q.compare(pos, 2, "--") == 0) {
            const size_t nl = q.find('\n', pos);
            pos = (nl == string::npos) ? q.size() : nl + 1;
            continue;
        }
        if (q.compare(pos, 2, "/*") == 0) {
            const size_t end = q.find("*/", pos + 2);
            pos = (end == string::npos) ? q.size() : end + 2;
            continue;
        }
        pos++;
    }
    return q.size();
}

// Single-quoted SQL literal for `s`.
inline string dbsp_sql_literal(const string &s) {
    string out = "'";
    for (char c : s) {
        out += c;
        if (c == '\'') {
            out += c; // doubled, not prefixed: '' is the escape
        }
    }
    out += "'";
    return out;
}

inline string dbsp_trimmed(const string &s) {
    string out = s;
    StringUtil::Trim(out);
    return out;
}
// A view name as the extension stores it. `create_view` keys views by a PLAIN
// identifier, so a quoted name is unquoted here and a qualified one is
// refused: without this the raw slice was handed on and the failure read
// `Failed to create materialized view '"my view"': Invalid view name` — the
// quotes in the message being the only clue that the DDL, not the user, put
// them there.
inline bool dbsp_normalize_view_name(const string &raw, string &out,
                                     string &reason,
                                     bool lookup_only = false) {
    out.clear();
    if (raw.empty()) {
        reason = "the view name is empty";
        return false;
    }
    if (raw[0] == '"') {
        if (raw.size() < 2 || raw.back() != '"') {
            reason = "the quoted view name " + raw + " is not closed";
            return false;
        }
        for (size_t i = 1; i + 1 < raw.size(); i++) {
            if (raw[i] == '"' && i + 2 < raw.size() && raw[i + 1] == '"') {
                out += '"';
                i++;
                continue;
            }
            if (raw[i] == '"') {
                reason = "the view name " + raw + " is more than one quoted "
                                                 "identifier (qualified names "
                                                 "are not supported)";
                return false;
            }
            out += raw[i];
        }
    } else {
        out = raw;
    }
    if (lookup_only) {
        // DROP / REFRESH only LOOK the view up. A name no view can carry is
        // simply not found, and "does not exist" is a better answer than a
        // parse error about registrability.
        return true;
    }
    if (out.find('.') != string::npos) {
        reason = "the view name " + raw +
                 " is qualified; materialized views are registered by a plain "
                 "name, so write it unqualified";
        return false;
    }
    if (!(std::isalpha(static_cast<unsigned char>(out[0])) || out[0] == '_')) {
        reason = "the view name " + raw +
                 " must start with a letter or an underscore";
        return false;
    }
    for (char c : out) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            reason = "the view name " + raw +
                     " contains a character that is not a letter, a digit or "
                     "an underscore";
            return false;
        }
    }
    return true;
}

// What the parser made of a query.
enum class MvDdlMatch {
    NotOurs,   // no MATERIALIZED VIEW statement here; stay silent
    Rewritten, // it parsed; `out` holds what it means
    Declined,  // it IS one of ours and could not be parsed; `reason` says why
};

// One MATERIALIZED VIEW DDL statement, parsed. Both entry points — the
// raw-text override and the token-stream fallback — produce this, so there is
// exactly one grammar in the extension and one set of error messages.
struct MvDdl {
    enum class Kind { Create, Drop, Refresh };
    Kind kind = Kind::Create;
    string name;
    string body;            // Create only: the SELECT, byte-exact where the
                            // input was (the override's route)
    bool or_replace = false; // Create only
    bool cascade = false;    // Drop only
    bool if_exists = false;  // Drop only
};

// THE parser for this extension's DDL.
//
// The NotOurs / Declined split exists because a silent decline is how a
// defect hides: an escaping bug once made every rewrite unparseable, the
// override declined, the statement fell back to the token path, and the only
// symptom was that stored SQL stayed normalised. A Declined result is reported
// by the caller.
inline MvDdlMatch dbsp_parse_mv_ddl(const string &query, MvDdl &ddl,
                                    string &reason) {
    size_t pos = dbsp_skip_ws_comments(query, 0);

    // Everything below is a single statement. A trailing statement is DECLINED
    // rather than rewritten: the input then takes the ordinary route and fails
    // there, loudly, exactly as it did before this override existed. Silently
    // folding it into the SELECT body, or silently dropping it, are the two
    // outcomes that must never happen.
    auto single_statement = [&](size_t body_start, string &body) -> bool {
        const size_t end = dbsp_find_statement_end(query, body_start);
        if (end < query.size() &&
            dbsp_skip_ws_comments(query, end + 1) < query.size()) {
            reason = "more than one statement in the input";
            return false;
        }
        body = dbsp_trimmed(query.substr(body_start, end - body_start));
        if (body.empty()) {
            reason = "the SELECT body is empty";
            return false;
        }
        return true;
    };

    // Name scan + normalisation, shared by all three statements.
    // `lookup_only` (DROP / REFRESH) skips the REGISTRABILITY checks: those two
    // only look a view up, and a name no view can carry simply is not found —
    // which is the answer the caller wants ("does not exist"), not a parse
    // error about a name they were never going to register.
    auto take_name = [&](string &name, bool lookup_only) -> bool {
        const size_t name_start = pos;
        const size_t name_end = dbsp_skip_identifier(query, pos);
        if (name_end == name_start) {
            reason = "no view name after MATERIALIZED VIEW";
            return false;
        }
        const string raw = query.substr(name_start, name_end - name_start);
        pos = dbsp_skip_ws_comments(query, name_end);
        return dbsp_normalize_view_name(raw, name, reason, lookup_only);
    };

    if (dbsp_keyword_at(query, pos, "CREATE")) {
        pos = dbsp_skip_ws_comments(query, pos + 6);
        bool or_replace = false;
        if (dbsp_keyword_at(query, pos, "OR")) {
            const size_t after_or = dbsp_skip_ws_comments(query, pos + 2);
            if (!dbsp_keyword_at(query, after_or, "REPLACE")) {
                return MvDdlMatch::NotOurs;
            }
            or_replace = true;
            pos = dbsp_skip_ws_comments(query, after_or + 7);
        }
        if (!dbsp_keyword_at(query, pos, "MATERIALIZED")) {
            return MvDdlMatch::NotOurs;
        }
        pos = dbsp_skip_ws_comments(query, pos + 12);
        if (!dbsp_keyword_at(query, pos, "VIEW")) {
            return MvDdlMatch::NotOurs;
        }
        // Past here it IS a CREATE MATERIALIZED VIEW: every exit is a Decline.
        pos = dbsp_skip_ws_comments(query, pos + 4);
        if (dbsp_keyword_at(query, pos, "IF")) {
            // IF NOT EXISTS is accepted and ignored, as it always has been on
            // this path: create_view already treats a repeat create as a
            // redefinition rather than an error.
            size_t p = dbsp_skip_ws_comments(query, pos + 2);
            if (!dbsp_keyword_at(query, p, "NOT")) {
                reason = "expected IF NOT EXISTS";
                return MvDdlMatch::Declined;
            }
            p = dbsp_skip_ws_comments(query, p + 3);
            if (!dbsp_keyword_at(query, p, "EXISTS")) {
                reason = "expected IF NOT EXISTS";
                return MvDdlMatch::Declined;
            }
            pos = dbsp_skip_ws_comments(query, p + 6);
        }
        if (!take_name(ddl.name, /*lookup_only=*/false)) {
            return MvDdlMatch::Declined;
        }
        if (!dbsp_keyword_at(query, pos, "AS")) {
            reason = "expected AS after the view name";
            return MvDdlMatch::Declined;
        }
        if (!single_statement(pos + 2, ddl.body)) {
            return MvDdlMatch::Declined;
        }
        ddl.kind = MvDdl::Kind::Create;
        ddl.or_replace = or_replace;
        return MvDdlMatch::Rewritten;
    }

    if (dbsp_keyword_at(query, pos, "DROP")) {
        pos = dbsp_skip_ws_comments(query, pos + 4);
        if (!dbsp_keyword_at(query, pos, "MATERIALIZED")) {
            return MvDdlMatch::NotOurs;
        }
        pos = dbsp_skip_ws_comments(query, pos + 12);
        if (!dbsp_keyword_at(query, pos, "VIEW")) {
            return MvDdlMatch::NotOurs;
        }
        pos = dbsp_skip_ws_comments(query, pos + 4);
        bool if_exists = false;
        if (dbsp_keyword_at(query, pos, "IF")) {
            const size_t p = dbsp_skip_ws_comments(query, pos + 2);
            if (!dbsp_keyword_at(query, p, "EXISTS")) {
                reason = "expected IF EXISTS";
                return MvDdlMatch::Declined;
            }
            if_exists = true;
            pos = dbsp_skip_ws_comments(query, p + 6);
        }
        if (!take_name(ddl.name, /*lookup_only=*/true)) {
            return MvDdlMatch::Declined;
        }
        if (dbsp_keyword_at(query, pos, "CASCADE")) {
            ddl.cascade = true;
            pos = dbsp_skip_ws_comments(query, pos + 7);
        } else if (dbsp_keyword_at(query, pos, "RESTRICT")) {
            pos = dbsp_skip_ws_comments(query, pos + 8);
        }
        if (pos < query.size() && query[pos] == ';') {
            pos = dbsp_skip_ws_comments(query, pos + 1);
        }
        if (pos < query.size()) {
            reason = "trailing text after the DROP statement";
            return MvDdlMatch::Declined;
        }
        ddl.kind = MvDdl::Kind::Drop;
        ddl.if_exists = if_exists;
        return MvDdlMatch::Rewritten;
    }

    if (dbsp_keyword_at(query, pos, "REFRESH")) {
        pos = dbsp_skip_ws_comments(query, pos + 7);
        if (!dbsp_keyword_at(query, pos, "MATERIALIZED")) {
            return MvDdlMatch::NotOurs;
        }
        pos = dbsp_skip_ws_comments(query, pos + 12);
        if (!dbsp_keyword_at(query, pos, "VIEW")) {
            return MvDdlMatch::NotOurs;
        }
        pos = dbsp_skip_ws_comments(query, pos + 4);
        if (!take_name(ddl.name, /*lookup_only=*/true)) {
            return MvDdlMatch::Declined;
        }
        if (pos < query.size() && query[pos] == ';') {
            pos = dbsp_skip_ws_comments(query, pos + 1);
        }
        if (pos < query.size()) {
            reason = "trailing text after the REFRESH statement";
            return MvDdlMatch::Declined;
        }
        ddl.kind = MvDdl::Kind::Refresh;
        return MvDdlMatch::Rewritten;
    }

    return MvDdlMatch::NotOurs;
}

// The equivalent call on the extension's own table functions, as SQL text.
// Formatting only: `dbsp_parse_mv_ddl` above is the parser.
inline string dbsp_mv_ddl_call(const MvDdl &ddl) {
    switch (ddl.kind) {
    case MvDdl::Kind::Create:
        return "SELECT * FROM dbsp_create_materialized_view(" +
               dbsp_sql_literal(ddl.name) + ", " + dbsp_sql_literal(ddl.body) +
               ", " + (ddl.or_replace ? "true" : "false") + ")";
    case MvDdl::Kind::Drop:
        return "SELECT * FROM dbsp_drop_materialized_view(" +
               dbsp_sql_literal(ddl.name) + ", " +
               (ddl.cascade ? "true" : "false") + ", " +
               (ddl.if_exists ? "true" : "false") + ")";
    case MvDdl::Kind::Refresh:
        return "SELECT * FROM dbsp_refresh_materialized_view(" +
               dbsp_sql_literal(ddl.name) + ")";
    }
    return "";
}

// The one ParseData the token path carries: a parsed statement, not a
// statement-shaped class hierarchy. MaterializedViewPlan switches on `kind`
// where it used to dynamic_cast down three types.
struct MaterializedViewParseData : public ParserExtensionParseData {
    MvDdl ddl;

    unique_ptr<ParserExtensionParseData> Copy() const override {
        auto result = make_uniq<MaterializedViewParseData>();
        result->ddl = ddl;
        return std::move(result);
    }

    string ToString() const override { return dbsp_mv_ddl_call(ddl); }
};

inline ParserOverrideResult MaterializedViewOverride(ParserExtensionInfo *info,
                                                     const string &query,
                                                     ParserOptions &options) {
    // Cheap gate: every statement this override claims contains the word
    // MATERIALIZED, and this callback runs on EVERY query in the database.
    // Scanned in place — StringUtil::Upper() allocated a copy of every query
    // string the database ever parsed just to look for one word.
    if (!dbsp_contains_ci(query, "MATERIALIZED")) {
        return ParserOverrideResult();
    }
    MvDdl ddl;
    string reason;
    const auto match = dbsp_parse_mv_ddl(query, ddl, reason);
    if (match == MvDdlMatch::NotOurs) {
        return ParserOverrideResult();
    }
    if (match == MvDdlMatch::Declined) {
        // LOUD. Declining hands the statement to the core parser, which either
        // errors (the useful case) or lets the token path take it with
        // normalised SQL text — and the second outcome is indistinguishable
        // from success unless someone inspects dbsp_views(). Say why here.
        std::cerr << "DBSP: MATERIALIZED VIEW DDL not taken by the raw-text "
                     "parser (" << reason
                  << "); falling back to the core parser. The stored SQL, if "
                     "the statement succeeds at all, will be normalised.\n";
        return ParserOverrideResult();
    }
    try {
        // Parse the rewritten call with overrides and extensions OFF: this
        // callback would otherwise re-enter itself on its own output.
        ParserOptions inner = options;
        inner.parser_override_setting = AllowParserOverride::DEFAULT_OVERRIDE;
        inner.extensions = nullptr;
        Parser parser(inner);
        parser.ParseQuery(dbsp_mv_ddl_call(ddl));
        return ParserOverrideResult(std::move(parser.statements));
    } catch (std::exception &e) {
        std::cerr << "DBSP: MATERIALIZED VIEW DDL rewrite failed to parse ("
                  << e.what() << "); falling back to the core parser\n";
        return ParserOverrideResult(e);
    }
}

// The token-stream hook. It runs only for statements the core PEG parser
// FAILED on, and only when `allow_parser_override_extension` is DEFAULT (the
// override below is skipped then) — a database where the user set it back after
// LOAD, or an embedding whose build has no such setting at all. Both are real,
// so this entry point stays.
//
// What does NOT stay is a second parser behind it. The tokens are rejoined into
// a statement and handed to `dbsp_parse_mv_ddl` — the SAME parse the override
// runs — so the two routes accept the same DDL, refuse the same DDL and say the
// same thing when they refuse. They differ in exactly one respect, and it is
// inherent to the input: SimpleToken::text is a source slice, so rejoining them
// with single spaces NORMALISES the SQL this path stores, where the override
// sees the user's own bytes.
inline ParserExtensionParseResult MaterializedViewParse(ParserExtensionInfo *info,
                                                        const vector<SimpleToken> &tokens) {
    const auto query = dbsp_tokens_to_query(tokens);
    MvDdl ddl;
    string reason;
    switch (dbsp_parse_mv_ddl(query, ddl, reason)) {
    case MvDdlMatch::NotOurs:
        return ParserExtensionParseResult(); // consumed_tokens stays 0
    case MvDdlMatch::Declined: {
        // A negative count is what makes the peeler surface `error` — a zero
        // count would silently hand the input to the next extension.
        ParserExtensionParseResult result("MATERIALIZED VIEW: " + reason);
        result.consumed_tokens = -1;
        return result;
    }
    case MvDdlMatch::Rewritten:
        break;
    }
    auto data = make_uniq<MaterializedViewParseData>();
    data->ddl = std::move(ddl);
    ParserExtensionParseResult result(std::move(data));
    result.consumed_tokens = NumericCast<int64_t>(tokens.size());
    return result;
}

//===--------------------------------------------------------------------===//
// Plan Function (Implementation in dbsp_extension.cpp)
//===--------------------------------------------------------------------===//

// Forward declaration - implementation will be in dbsp_extension.cpp
ParserExtensionPlanResult MaterializedViewPlan(ParserExtensionInfo *info,
                                               ClientContext &context,
                                               unique_ptr<ParserExtensionParseData> parse_data);

//===--------------------------------------------------------------------===//
// Extension Registration
//===--------------------------------------------------------------------===//

inline ParserExtension CreateMaterializedViewParserExtension() {
    ParserExtension extension;
    extension.parse_function = MaterializedViewParse;
    extension.plan_function = MaterializedViewPlan;
    // Runs BEFORE the core PEG grammar when allow_parser_override_extension is
    // FALLBACK or STRICT — see the block above. parse_function stays as the
    // fallback for a database where that setting is DEFAULT.
    extension.parser_override = MaterializedViewOverride;
    extension.parser_info = nullptr; // No additional info needed
    return extension;
}

} // namespace dbsp_native
