// SPDX-FileCopyrightText: 2026 Dingtaiqi
// SPDX-License-Identifier: AGPL-3.0-or-later
// ---------------------------------------------------------------------------
//  The bridge's configuration file.  Header-only and dependency-free so that the
//  NO-HARDWARE suites can gate it (iscsi_selftest.cpp, run_iscsi.ps1, CI) - the product
//  and its test must run the same parser, and this file is where that is possible.
//
//  WHY A FILE AT ALL.  Until now the whole configuration of the bridge service was the
//  command line embedded in the service's ImagePath: readable only through `sc qc`, one
//  line, no comments, and edited by reinstalling.  That is not a configuration, it is a
//  command line that happens to be stored somewhere.
//
//  WHY IT IS ARGV AND NOT A KEY/VALUE SCHEMA.  Every line is one command-line TOKEN,
//  without the leading dash:
//
//      # bridge.conf - comments start with # or ;
//      target 192.168.100.5
//      targetport 4420
//      iscsichunk 1048576
//      readwrite
//
//  The tokens are prepended to the real command line, so the existing parser does ALL the
//  validation that already exists (ranges, unknown options, contradictions), the file can
//  never drift from the flags it is supposed to describe, and a command-line argument
//  naturally overrides the file because it is parsed afterwards.  A hand-written schema
//  would have to re-implement every check, in a second place, with a second set of bugs.
//
//  WHAT IT REFUSES, and why refusing matters more than accepting: a config file that is
//  silently half-applied is worse than one that fails to load, because the bridge would
//  come up with settings nobody chose.  So: a line without a key, a key that is not a
//  bare flag name, an unterminated quote, a line or file that is too large - all rejected
//  with the line number.
// ---------------------------------------------------------------------------
#ifndef NVMEOF_CONFIG_H
#define NVMEOF_CONFIG_H

#include <stdio.h>
#include <string>
#include <vector>

namespace nvmeof_config {

struct Loaded {
    std::vector<std::string> tokens;   // "-flag", "value", "-switch", ...
    std::vector<std::pair<int, std::string>> lines;  // what was read, for -checkconfig
};

// Limits: small on purpose.  A configuration file that is megabytes long is not a
// configuration file, and a bound here means a malformed file cannot be used to make the
// process allocate without limit.
static const long kMaxFileBytes = 64 * 1024;
static const size_t kMaxLineChars = 512;
static const size_t kMaxTokens = 256;

static inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isSpace(s[a])) a++;
    while (b > a && isSpace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

// One line -> zero, one or two tokens.  Returns false and fills `err` on anything it
// cannot interpret; every error names the line, because "the config is bad" without a
// line number is a bug report about a file the reader has not seen.
static inline bool parseLine(const std::string& raw, int lineNo, Loaded& out, std::string& err) {
    std::string s = trim(raw);
    if (s.empty()) return true;                       // blank line
    if (s[0] == '#' || s[0] == ';') return true;      // comment
    if (s.size() > kMaxLineChars) {
        char b[96]; sprintf_s(b, sizeof(b), "line %d is longer than %zu characters", lineNo, kMaxLineChars);
        err = b; return false;
    }
    // The key is the first whitespace-delimited field and must look like a flag name:
    // letters, digits, '-' and '_' only.  Anything else (a stray '=', a leading '-') is
    // rejected rather than guessed at - the two ways to write "target" are how a config
    // file ends up with a setting that is silently ignored.
    size_t i = 0;
    while (i < s.size() && !isSpace(s[i])) i++;
    const std::string key = s.substr(0, i);
    if (key.empty()) { err = "line " + std::to_string(lineNo) + ": empty key"; return false; }
    if (key[0] == '-') {
        err = "line " + std::to_string(lineNo) + ": write options WITHOUT the leading '-' (" + key + ")";
        return false;
    }
    for (size_t k = 0; k < key.size(); k++) {
        const char c = key[k];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            err = "line " + std::to_string(lineNo) + ": '" + key + "' is not an option name";
            return false;
        }
    }
    std::string value = trim(s.substr(i));
    if (!value.empty() && (value[0] == '"' || value[0] == '\'')) {
        const char q = value[0];
        if (value.size() < 2 || value[value.size() - 1] != q) {
            err = "line " + std::to_string(lineNo) + ": unterminated quote";
            return false;
        }
        value = value.substr(1, value.size() - 2);
    }
    // A value containing spaces must be quoted; catching that here is what stops
    // "subnqn nqn.2024-01 com.example" from becoming two tokens and a confusing parse error.
    if (value.find(' ') != std::string::npos || value.find('\t') != std::string::npos) {
        err = "line " + std::to_string(lineNo) + ": a value with spaces must be quoted";
        return false;
    }
    out.tokens.push_back("-" + key);
    std::string shown = "-" + key;
    if (!value.empty()) { out.tokens.push_back(value); shown += " " + value; }
    out.lines.push_back({ lineNo, shown });
    if (out.tokens.size() > kMaxTokens) { err = "too many options (more than " + std::to_string(kMaxTokens) + ")"; return false; }
    return true;
}

// Load a file into tokens.  Returns false with a message that names the file and the line.
static inline bool load(const char* path, Loaded& out, std::string& err) {
    out.tokens.clear();
    out.lines.clear();
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) { err = std::string("cannot open ") + path; return false; }
    std::string text;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
        if ((long)text.size() > kMaxFileBytes) {
            fclose(f);
            err = std::string(path) + " is larger than " + std::to_string(kMaxFileBytes) + " bytes";
            return false;
        }
    }
    fclose(f);
    // A UTF-8 BOM is not a key.  Editors add one without asking, and the failure it would
    // otherwise cause ("'\xEF\xBB\xBFtarget' is not an option name") is unreadable.
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        text.erase(0, 3);
    }
    int lineNo = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        const std::string raw = (nl == std::string::npos) ? text.substr(start) : text.substr(start, nl - start);
        lineNo++;
        if (!parseLine(raw, lineNo, out, err)) { err = std::string(path) + ": " + err; return false; }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return true;
}

} // namespace nvmeof_config

#endif // NVMEOF_CONFIG_H
