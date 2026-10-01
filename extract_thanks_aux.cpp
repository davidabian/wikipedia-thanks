#include <zlib.h>
#include <arpa/inet.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// Generic utility helpers used by extract_thanks.cpp.
// This file is intentionally included by extract_thanks.cpp; compile extract_thanks.cpp only.

[[noreturn]] static void die(const std::string &msg) {
    throw std::runtime_error(msg);
}

static void add_failure_example(std::vector<std::string> &examples, const std::string &reason,
                                const std::string &logid, const std::string &timestamp,
                                const std::string &context, const std::string &detail) {
    if (examples.size() >= 30)
        return;
    examples.push_back(reason + " logid=" + (logid.empty() ? "<empty>" : logid) +
                       " timestamp=" + (timestamp.empty() ? "<empty>" : timestamp) +
                       " context=" + (context.empty() ? "<empty>" : context) + " | " + detail);
}

static bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

static std::string xml_name(const xmlNode *node) {
    if (!node || !node->name)
        return {};
    if (node->ns && node->ns->prefix) {
        return std::string(reinterpret_cast<const char *>(node->ns->prefix)) + ":" +
               reinterpret_cast<const char *>(node->name);
    }
    return reinterpret_cast<const char *>(node->name);
}

static std::string attr_value(const xmlNode *node, const char *name) {
    if (!node)
        return {};
    xmlChar *v = xmlGetProp(const_cast<xmlNode *>(node), reinterpret_cast<const xmlChar *>(name));
    if (!v)
        return {};
    std::string out(reinterpret_cast<const char *>(v));
    xmlFree(v);
    return out;
}

static bool is_deleted_node(const xmlNode *node) {
    return attr_value(node, "deleted") == "deleted";
}

static std::string node_content(const xmlNode *node) {
    xmlChar *txt = xmlNodeGetContent(const_cast<xmlNode *>(node));
    if (!txt)
        return {};
    std::string out(reinterpret_cast<const char *>(txt));
    xmlFree(txt);
    return out;
}

static const xmlNode *first_element_child_named(const xmlNode *node, const std::string &name) {
    if (!node)
        return nullptr;
    for (const xmlNode *ch = node->children; ch; ch = ch->next) {
        if (ch->type == XML_ELEMENT_NODE && xml_name(ch) == name)
            return ch;
    }
    return nullptr;
}

static int two_digits(std::string_view s, size_t pos) {
    if (pos + 1 >= s.size())
        return -1;
    unsigned char a = static_cast<unsigned char>(s[pos]);
    unsigned char b = static_cast<unsigned char>(s[pos + 1]);
    if (!std::isdigit(a) || !std::isdigit(b))
        return -1;
    return (s[pos] - '0') * 10 + (s[pos + 1] - '0');
}

static bool valid_timestamp_common(std::string_view s, bool allow_2000s) {
    if (s.size() != 20)
        return false;
    if (s[0] != '2' || s[1] != '0')
        return false;
    if (!std::isdigit(static_cast<unsigned char>(s[2])) ||
        !std::isdigit(static_cast<unsigned char>(s[3])))
        return false;
    if (!allow_2000s && (s[2] < '1' || s[2] > '9'))
        return false;
    if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' || s[19] != 'Z')
        return false;
    int month = two_digits(s, 5);
    int day = two_digits(s, 8);
    int hour = two_digits(s, 11);
    int minute = two_digits(s, 14);
    int second = two_digits(s, 17);
    if (month < 1 || month > 12)
        return false;
    const int year = 2000 + (s[2] - '0') * 10 + (s[3] - '0');
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const int days[] = {31, 28 + int(leap), 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return day >= 1 && day <= days[month - 1] && hour >= 0 && hour <= 23 && minute >= 0 &&
           minute <= 59 && second >= 0 && second <= 59;
}

static bool valid_non_deleted_timestamp(std::string_view s) {
    return valid_timestamp_common(s, false);
}

static bool valid_account_creation_timestamp(std::string_view s) {
    return valid_timestamp_common(s, true);
}

static size_t utf8_codepoint_count(std::string_view s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0u) != 0x80u)
            ++n;
    return n;
}

static std::string join_semicolon(const std::vector<std::string> &xs) {
    std::string out;
    for (size_t i = 0; i < xs.size(); ++i) {
        if (i)
            out += ';';
        out += xs[i];
    }
    return out;
}

static bool is_digits(std::string_view s) {
    if (s.empty())
        return false;
    for (unsigned char c : s)
        if (!std::isdigit(c))
            return false;
    return true;
}

// Canonical positive decimal IDs prevent alternate spellings from creating
// distinct identity keys. No machine integer conversion or overflow is involved.
static bool valid_positive_id(std::string_view s) {
    return is_digits(s) && s.front() != '0';
}

static std::string target_label_subtype(const std::string &label) {
    unsigned char buffer[16];
    return inet_pton(AF_INET, label.c_str(), buffer) == 1 ||
                   inet_pton(AF_INET6, label.c_str(), buffer) == 1
               ? "ip_address"
               : "username";
}

static std::string trim_leading_zeros(std::string_view s) {
    size_t i = 0;
    while (i + 1 < s.size() && s[i] == '0')
        ++i;
    return std::string(s.substr(i));
}

static bool logid_less(std::string_view a, std::string_view b) {
    const bool ad = is_digits(a), bd = is_digits(b);
    if (ad && bd) {
        std::string aa = trim_leading_zeros(a), bb = trim_leading_zeros(b);
        if (aa.size() != bb.size())
            return aa.size() < bb.size();
        if (aa != bb)
            return aa < bb;
        return a < b;
    }
    return a < b;
}

static std::string shell_quote(const std::string &s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

static std::string md5sum_file(const fs::path &p) {
    const std::string cmd = "md5sum -- " + shell_quote(p.string());
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        die("failed to run md5sum for: " + p.string());
    char buf[256];
    std::string output;
    while (fgets(buf, sizeof(buf), pipe))
        output += buf;
    int rc = pclose(pipe);
    if (rc != 0)
        die("md5sum failed for: " + p.string());
    std::istringstream iss(output);
    std::string md5;
    iss >> md5;
    if (md5.size() != 32)
        die("could not parse md5sum output for: " + p.string());
    return md5;
}

static std::string bool_text(bool x) {
    return x ? "true" : "false";
}

static std::string bool_yesno(bool x) {
    return x ? "yes" : "no";
}

static void append_utf8_codepoint(std::string &out, uint32_t cp) {
    if (cp <= 0x7Fu)
        out.push_back(static_cast<char>(cp));
    else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

static std::string decode_xml_entities(std::string_view s) {
    if (s.find('&') == std::string_view::npos)
        return std::string(s);
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '&') {
            out.push_back(s[i++]);
            continue;
        }
        size_t semi = s.find(';', i + 1);
        if (semi == std::string_view::npos) {
            out.push_back(s[i++]);
            continue;
        }
        std::string_view ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp")
            out.push_back('&');
        else if (ent == "lt")
            out.push_back('<');
        else if (ent == "gt")
            out.push_back('>');
        else if (ent == "quot")
            out.push_back('"');
        else if (ent == "apos")
            out.push_back('\'');
        else if (!ent.empty() && ent[0] == '#') {
            uint32_t cp = 0;
            bool ok = true;
            size_t p = 1;
            int base = 10;
            if (p < ent.size() && (ent[p] == 'x' || ent[p] == 'X')) {
                base = 16;
                ++p;
            }
            if (p >= ent.size())
                ok = false;
            for (; ok && p < ent.size(); ++p) {
                unsigned char c = static_cast<unsigned char>(ent[p]);
                int v = -1;
                if (base == 10 && std::isdigit(c))
                    v = c - '0';
                else if (base == 16 && std::isdigit(c))
                    v = c - '0';
                else if (base == 16 && c >= 'a' && c <= 'f')
                    v = 10 + c - 'a';
                else if (base == 16 && c >= 'A' && c <= 'F')
                    v = 10 + c - 'A';
                else
                    ok = false;
                if (ok) {
                    cp = cp * static_cast<uint32_t>(base) + static_cast<uint32_t>(v);
                    if (cp > 0x10FFFFu)
                        ok = false;
                }
            }
            if (ok)
                append_utf8_codepoint(out, cp);
            else {
                out.push_back('&');
                out.append(ent.data(), ent.size());
                out.push_back(';');
            }
        } else {
            out.push_back('&');
            out.append(ent.data(), ent.size());
            out.push_back(';');
        }
        i = semi + 1;
    }
    return out;
}

static bool tag_name_boundary(char c) {
    return c == '>' || c == '/' || std::isspace(static_cast<unsigned char>(c));
}

static bool start_tag_deleted(std::string_view start_tag) {
    return start_tag.find("deleted=\"deleted\"") != std::string_view::npos ||
           start_tag.find("deleted='deleted'") != std::string_view::npos;
}

static bool find_element_range(std::string_view hay, std::string_view tag, size_t from,
                               size_t &open_start, size_t &open_end, size_t &content_start,
                               size_t &content_end, bool &deleted, bool &self_closing) {
    const std::string open = "<" + std::string(tag);
    const std::string close = "</" + std::string(tag) + ">";
    size_t pos = from;
    while (true) {
        pos = hay.find(open, pos);
        if (pos == std::string_view::npos)
            return false;
        const size_t after_name = pos + open.size();
        if (after_name >= hay.size() || tag_name_boundary(hay[after_name]))
            break;
        ++pos;
    }
    open_start = pos;
    open_end = hay.find('>', open_start);
    if (open_end == std::string_view::npos)
        return false;
    std::string_view st = hay.substr(open_start, open_end - open_start + 1);
    deleted = start_tag_deleted(st);
    self_closing = open_end > open_start && hay[open_end - 1] == '/';
    if (self_closing) {
        content_start = open_end + 1;
        content_end = content_start;
        return true;
    }
    size_t close_start = hay.find(close, open_end + 1);
    if (close_start == std::string_view::npos)
        return false;
    content_start = open_end + 1;
    content_end = close_start;
    return true;
}

static bool extract_first_element_text(std::string_view hay, std::string_view tag,
                                       std::string &value) {
    size_t os = 0, oe = 0, cs = 0, ce = 0;
    bool deleted = false, self_closing = false;
    if (!find_element_range(hay, tag, 0, os, oe, cs, ce, deleted, self_closing))
        return false;
    if (deleted || self_closing)
        return false;
    value = decode_xml_entities(hay.substr(cs, ce - cs));
    return true;
}

static void write_csv_cell(std::ofstream &out, const std::string &s) {
    bool quote = false;
    for (char c : s)
        if (c == '"' || c == ',' || c == '\n' || c == '\r') {
            quote = true;
            break;
        }
    if (!quote) {
        out << s;
        return;
    }
    out.put('"');
    for (char c : s) {
        if (c == '"')
            out.put('"');
        out.put(c);
    }
    out.put('"');
}

class StreamNeedleCounter {
  public:
    explicit StreamNeedleCounter(std::string_view needle) : needle_(needle) {}
    void add(const char *data, size_t n) {
        if (needle_.empty() || n == 0)
            return;
        std::string s;
        s.reserve(tail_.size() + n);
        s.append(tail_);
        s.append(data, n);
        size_t pos = 0;
        while ((pos = s.find(needle_, pos)) != std::string::npos) {
            ++count_;
            ++pos;
        }
        const size_t keep = needle_.size() - 1;
        if (s.size() <= keep)
            tail_ = std::move(s);
        else
            tail_.assign(s.data() + s.size() - keep, keep);
    }
    uint64_t count() const {
        return count_;
    }

  private:
    std::string needle_;
    std::string tail_;
    uint64_t count_ = 0;
};
