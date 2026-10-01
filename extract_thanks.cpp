/*
  Extract timestamped Thanks networks from MediaWiki logging dumps.

  Publication mode writes three files per site:
    *.edges.csv: retained events with source and target IDs;
    *.nodes.csv: account or synthetic-label nodes and analytical attributes;
    *.target_resolution_audit.csv: node summaries and event-resolution evidence.

  Sources are contributor account IDs. Targets use creation, rename-interval,
  and contributor-name evidence; unresolved username/IP labels receive negative
  IDs. Interval-only matches are withheld because log titles may have changed
  after renames. Events at or after the dump-date cutoff and resolved self-loops
  are excluded.

  Build with compile.sh; extract_thanks_aux.cpp is included by this file.
  A build with EXTRACT_THANKS_VERBOSE_CAPABLE=1 also supports --verbose/--debug
  for additional identity diagnostics. The CSV schemas are described in main.tex.
*/

#include "extract_thanks_aux.cpp"

static constexpr std::string_view OPEN_TAG = "<logitem";
static constexpr std::string_view CLOSE_TAG = "</logitem>";
static constexpr std::string_view SITEINFO_OPEN_TAG = "<siteinfo";
static constexpr std::string_view SITEINFO_CLOSE_TAG = "</siteinfo>";
static constexpr std::string_view THANKS_NEEDLE = "<type>thanks</type>";
static constexpr std::string_view NEWUSERS_NEEDLE = "<type>newusers</type>";
static constexpr std::string_view RENAMEUSER_NEEDLE = "<type>renameuser</type>";
static constexpr std::string_view BLOCK_NEEDLE = "<type>block</type>";
#ifndef EXTRACT_THANKS_VERBOSE_CAPABLE
#define EXTRACT_THANKS_VERBOSE_CAPABLE 1
#endif

static_assert(EXTRACT_THANKS_VERBOSE_CAPABLE == 0 || EXTRACT_THANKS_VERBOSE_CAPABLE == 1,
              "EXTRACT_THANKS_VERBOSE_CAPABLE must be 0 or 1");

static constexpr bool VERBOSE_CAPABLE = EXTRACT_THANKS_VERBOSE_CAPABLE != 0;
static constexpr std::string_view EXTRACT_THANKS_VERSION =
    VERBOSE_CAPABLE ? "interval-identity-v24-unified-audit-debug-capable"
                    : "interval-identity-v24-unified-audit-publication-only";

struct Options {
    fs::path out_dir;
    bool use_out_dir = false;
    bool force = false;
    bool verbose = false;
    size_t buffer_bytes = 16u * 1024u * 1024u;
    std::vector<fs::path> inputs;
};

struct Stats {
    uint64_t logitem_blocks = 0;
    uint64_t raw_thanks_type_occurrences = 0;
    uint64_t raw_block_type_occurrences = 0;
    uint64_t logitem_blocks_at_or_after_input_date = 0;
    uint64_t thanks_candidate_blocks_at_or_after_input_date = 0;
    uint64_t thanks_candidate_blocks = 0;
    uint64_t block_candidate_blocks_at_or_after_input_date = 0;
    uint64_t block_candidate_blocks = 0;
    uint64_t block_action_block = 0;
    uint64_t block_action_not_block = 0;
    uint64_t block_parse_failures = 0;
    uint64_t block_timestamp_missing = 0;
    uint64_t block_timestamp_invalid = 0;
    uint64_t block_contributor_deleted_or_missing = 0;
    uint64_t block_source_id_missing = 0;
    uint64_t blocker_first_timestamps_recorded = 0;
    uint64_t publication_account_nodes = 0;
    uint64_t publication_account_nodes_with_first_block_by_user_timestamp = 0;
    uint64_t publication_first_block_by_user_timestamp_none_fail = 0;
    uint64_t publication_first_block_by_user_timestamp_high_coverage_fail = 0;
    uint64_t publication_first_block_before_creation = 0;
    uint64_t publication_first_block_before_creation_fail = 0;
    uint64_t self_loop_edges_dropped = 0;
    uint64_t retained_observed_edges = 0;
    uint64_t observed_edges_rows = 0;
    uint64_t resolved_edges_rows = 0;
    uint64_t accounts_rows = 0;
    uint64_t excluded_deleted = 0;
    uint64_t excluded_deleted_rows_written = 0;
    uint64_t excluded_timestamp_deleted = 0;
    uint64_t excluded_contributor_deleted = 0;
    uint64_t excluded_logtitle_deleted = 0;

    uint64_t parse_failures = 0;
    uint64_t non_thanks_after_parse = 0;
    uint64_t thanks_action_not_thank = 0;
    uint64_t thanks_params_invalid = 0;
    uint64_t timestamp_missing = 0;
    uint64_t timestamp_invalid = 0;
    uint64_t contributor_missing = 0;
    uint64_t target_logtitle_missing = 0;
    uint64_t target_logtitle_bad_prefix = 0;
    uint64_t target_namespace_not_in_siteinfo = 0;
    uint64_t target_namespace_inconsistent = 0;
    uint64_t source_username_missing = 0;
    uint64_t source_id_missing = 0;
    uint64_t source_id_invalid = 0;
    uint64_t identity_id_invalid = 0;
    uint64_t newusers_zero_param_ignored = 0;
    uint64_t logaction_id_invalid = 0;
    uint64_t source_username_id_conflict = 0;
    uint64_t source_id_username_conflict = 0;
    uint64_t logaction_id_missing = 0;
    uint64_t logaction_id_duplicate = 0;

    uint64_t siteinfo_parse_failures = 0;
    uint64_t namespace_count = 0;
    bool siteinfo_missing = false;
    bool unmatched_logitem_at_eof = false;

    uint64_t global_contributor_identities = 0;
    uint64_t global_contributor_username_id_conflict = 0;
    uint64_t global_contributor_id_username_conflict = 0;

    uint64_t newusers_candidate_blocks = 0;
    uint64_t newusers_parse_failures = 0;
    uint64_t newusers_total = 0;
    uint64_t newusers_action_create = 0;
    uint64_t newusers_action_autocreate = 0;
    uint64_t newusers_action_newusers = 0;
    uint64_t newusers_action_create2 = 0;
    uint64_t newusers_action_byemail = 0;
    uint64_t newusers_action_forcecreatelocal = 0;
    uint64_t newusers_action_other = 0;
    uint64_t newusers_params_userid_matches_contributor_id = 0;
    uint64_t newusers_params_userid_differs_from_contributor_id = 0;

    uint64_t account_creation_by_contributor = 0;
    uint64_t account_creation_by_params_userid = 0;
    uint64_t account_creation_by_logtitle_only = 0;
    uint64_t account_creation_by_logtitle_only_current_username = 0;
    uint64_t account_creation_logtitle_only_ignored_not_current = 0;
    uint64_t account_creation_logtitle_only_ignored_after_observed_activity = 0;
    uint64_t account_creation_duplicate_same = 0;
    uint64_t account_creation_duplicate_earlier = 0;
    uint64_t account_creation_conflict_username = 0;
    uint64_t account_creation_conflict_userid = 0;
    uint64_t account_creation_username_differs_from_current = 0;
    uint64_t account_creation_ignored_after_observed_activity = 0;
    uint64_t account_creation_aliases_recorded = 0;
    uint64_t account_creation_alias_duplicate_same = 0;
    uint64_t account_creation_alias_intervals_built = 0;
    uint64_t account_creation_alias_intervals_closed_by_rename = 0;
    uint64_t account_creation_alias_interval_overlaps = 0;
    uint64_t account_creation_alias_interval_rename_old_not_active = 0;
    uint64_t account_creation_alias_interval_rename_old_ambiguous = 0;
    uint64_t account_creation_alias_interval_ignored_no_account_creation = 0;

    uint64_t newusers_timestamp_missing = 0;
    uint64_t newusers_timestamp_invalid = 0;
    uint64_t newusers_username_missing = 0;
    uint64_t newusers_userid_missing = 0;
    uint64_t newusers_missing_userid_pending_username = 0;
    uint64_t newusers_missing_userid_recovered_from_username = 0;
    uint64_t newusers_missing_userid_ignored_username_not_current = 0;
    uint64_t newusers_missing_userid_ignored_after_observed_activity = 0;
    uint64_t newusers_logtitle_missing = 0;
    uint64_t newusers_logtitle_bad_prefix = 0;
    uint64_t newusers_params_userid_missing = 0;
    uint64_t newusers_missing_username_pending_logtitle = 0;
    uint64_t newusers_missing_username_recovered_from_logtitle = 0;
    uint64_t newusers_missing_username_ignored_id_username_mismatch = 0;
    uint64_t newusers_missing_username_ignored_id_not_current = 0;
    uint64_t newusers_missing_username_ignored_after_observed_activity = 0;

    uint64_t renameuser_candidate_blocks = 0;
    uint64_t renameuser_total = 0;
    uint64_t renameuser_parsed = 0;
    uint64_t renameuser_unparsed = 0;
    uint64_t renameuser_action_not_renameuser = 0;
    uint64_t renameuser_timestamp_invalid = 0;
    uint64_t rename_events_rows = 0;

    uint64_t source_thanks_before_account_creation = 0;
    uint64_t target_resolved_thanks_before_account_creation = 0;
    uint64_t account_creation_timestamp_id_order_invalidated = 0;
    uint64_t thanks_logid_timestamp_order_violations = 0;

    uint64_t target_resolution_direct_current = 0;
    uint64_t target_resolution_direct_current_no_creation = 0;
    uint64_t target_resolution_rename_chain = 0;
    uint64_t target_resolution_direct_and_chain_same = 0;
    uint64_t target_resolution_creation_alias = 0;
    uint64_t target_resolution_direct_and_creation_alias_same = 0;
    uint64_t target_resolution_ambiguous = 0;
    uint64_t target_resolution_ambiguous_creation_alias = 0;
    uint64_t target_resolution_missing_created_after_thank = 0;
    uint64_t target_resolution_missing_no_current = 0;
    uint64_t target_resolution_missing_unresolved_rename_chain = 0;
};

static uint64_t target_resolution_ambiguous_total(const Stats &s) {
    return s.target_resolution_ambiguous + s.target_resolution_ambiguous_creation_alias;
}

static uint64_t target_resolution_status_total(const Stats &s) {
    return s.target_resolution_direct_current + s.target_resolution_direct_current_no_creation +
           s.target_resolution_rename_chain + s.target_resolution_direct_and_chain_same +
           s.target_resolution_creation_alias + s.target_resolution_direct_and_creation_alias_same +
           s.target_resolution_ambiguous + s.target_resolution_ambiguous_creation_alias +
           s.target_resolution_missing_created_after_thank +
           s.target_resolution_missing_no_current +
           s.target_resolution_missing_unresolved_rename_chain;
}

static uint64_t target_resolution_unresolved_or_ambiguous_total(const Stats &s) {
    return target_resolution_ambiguous_total(s) + s.target_resolution_missing_created_after_thank +
           s.target_resolution_missing_no_current +
           s.target_resolution_missing_unresolved_rename_chain;
}

struct ObservedEdge {
    std::string logid;
    std::string timestamp;
    std::string source_id;
    std::string source_username;
    std::string target_username;
    std::string target_account;
    std::string target_resolution;
};

struct DeletedExclusion {
    std::string logid;
    std::string timestamp;
    std::string source_username;
    std::string source_id;
    bool contributor_deleted = false;
    std::string logtitle;
    std::string deleted_fields;
};

struct ParsedThanks {
    ObservedEdge edge;
    DeletedExclusion exclusion;
    bool valid = false;
    bool excluded_deleted = false;
};

struct RenameEvent {
    std::string logid;
    std::string timestamp;
    std::string old_username;
    std::string new_username;
    std::string parse_status;
};

struct AccountInfo {
    std::string account_id;
    std::string username;
    std::string creation_timestamp;
    std::string creation_evidence;
};

struct UsernameOnlyCreation {
    std::string username;
    std::string timestamp;
    std::string evidence;
    std::string logid;
};

struct AccountCreationAlias {
    std::string username;
    std::string account_id;
    std::string timestamp;
    std::string evidence;
    std::string logid;
};

struct UsernameInterval {
    std::string username;
    std::string account_id;
    std::string start_timestamp;
    std::string end_timestamp;
    std::string start_evidence;
    std::string start_logid;
    std::string end_logid;
};

struct PendingIdLogtitleCreation {
    std::string account_id;
    std::string username;
    std::string timestamp;
    std::string evidence;
    std::string logid;
};

struct PendingUsernameCreation {
    std::string username;
    std::string timestamp;
    std::string evidence;
    std::string logid;
};

struct OutputPaths {
    // Default publication/network-science outputs.
    fs::path network_edges;
    fs::path network_nodes;
    fs::path node_metadata;

    // Verbose/debug audit outputs.
    fs::path observed_edges;
    fs::path resolved_edges;
    fs::path accounts;
    fs::path rename_events;
    fs::path excluded_deleted;
    fs::path username_intervals;
    fs::path account_creation_candidates;
    fs::path rename_resolution;
    fs::path target_resolution_audit;
    fs::path validation_summary;
};

struct NamespaceInfo {
    std::unordered_set<std::string> all;
    std::unordered_set<std::string> user;
};

struct StringInterner {
    std::unordered_set<std::string> pool;

    std::string_view intern(const std::string &s) {
        auto it = pool.find(s);
        if (it != pool.end())
            return std::string_view(*it);
        auto inserted = pool.emplace(s);
        return std::string_view(*inserted.first);
    }

    void reserve(size_t n) {
        pool.reserve(n);
    }
};

using InternedStringMap = std::unordered_map<std::string_view, std::string_view>;
using InternedStringSet = std::unordered_set<std::string_view>;
using InternedTimestampMap = std::unordered_map<std::string_view, std::string>;

template <typename Container> static void release_container_memory(Container &c) {
    Container empty;
    c.swap(empty);
}

static bool extract_username_from_logtitle(const std::string &logtitle,
                                           const std::unordered_set<std::string> *namespaces,
                                           std::string &file_target_namespace,
                                           std::string &username, Stats *stats) {
    const size_t colon = logtitle.find(':');
    if (colon == std::string::npos) {
        if (stats)
            ++stats->target_logtitle_bad_prefix;
        return false;
    }
    const std::string ns = logtitle.substr(0, colon);
    const size_t n_chars = utf8_codepoint_count(ns);
    if (n_chars < 1 || n_chars > 60) {
        if (stats)
            ++stats->target_logtitle_bad_prefix;
        return false;
    }
    if (namespaces && namespaces->find(ns) == namespaces->end()) {
        if (stats)
            ++stats->target_namespace_not_in_siteinfo;
        return false;
    }
    if (stats && namespaces) {
        if (file_target_namespace.empty())
            file_target_namespace = ns;
        else if (file_target_namespace != ns) {
            ++stats->target_namespace_inconsistent;
            return false;
        }
    }
    username = logtitle.substr(colon + 1);
    if (username.empty()) {
        if (stats)
            ++stats->target_logtitle_missing;
        return false;
    }
    return true;
}

static NamespaceInfo parse_siteinfo_block(std::string_view block, Stats &stats) {
    NamespaceInfo info;
    std::string frag(block);
    xmlDocPtr doc =
        xmlReadMemory(frag.data(), static_cast<int>(frag.size()), "siteinfo.xml", nullptr,
                      XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) {
        ++stats.siteinfo_parse_failures;
        return info;
    }

    xmlNode *root = xmlDocGetRootElement(doc);
    if (!root || root->type != XML_ELEMENT_NODE || xml_name(root) != "siteinfo") {
        ++stats.siteinfo_parse_failures;
        xmlFreeDoc(doc);
        return info;
    }

    bool saw_namespaces = false;
    for (const xmlNode *ch = root->children; ch; ch = ch->next) {
        if (ch->type != XML_ELEMENT_NODE || xml_name(ch) != "namespaces")
            continue;
        saw_namespaces = true;

        for (const xmlNode *ns = ch->children; ns; ns = ns->next) {
            if (ns->type != XML_ELEMENT_NODE || xml_name(ns) != "namespace")
                continue;
            std::string value = node_content(ns);
            if (!value.empty()) {
                info.all.insert(value);

                std::string key = attr_value(ns, "key");
                if (key == "2") {
                    info.user.insert(value);
                    info.user.insert("User");
                }
            }
        }
    }

    if (!saw_namespaces || info.all.empty() || info.user.empty())
        ++stats.siteinfo_parse_failures;
    xmlFreeDoc(doc);
    return info;
}

static void record_first_observed_contributor_timestamp(
    std::string_view account_id, const std::string &timestamp,
    InternedTimestampMap &first_observed_contributor_timestamp_by_id) {
    if (account_id.empty() || timestamp.empty())
        return;
    if (!valid_account_creation_timestamp(timestamp))
        return;
    auto it = first_observed_contributor_timestamp_by_id.find(std::string_view(account_id));
    if (it == first_observed_contributor_timestamp_by_id.end() || timestamp < it->second) {
        first_observed_contributor_timestamp_by_id[account_id] = timestamp;
    }
}

static bool record_global_contributor_id_to_username(std::string_view username, std::string_view id,
                                                     const std::string &logid,
                                                     const std::string &timestamp,
                                                     InternedStringMap &contributor_id_to_username,
                                                     Stats &stats,
                                                     std::vector<std::string> &failure_examples) {
    auto iit = contributor_id_to_username.find(id);
    if (iit != contributor_id_to_username.end() && iit->second != username) {
        ++stats.global_contributor_id_username_conflict;
        add_failure_example(failure_examples, "global_contributor_id_username_conflict", logid,
                            timestamp, std::string(username),
                            "id=" + std::string(id) +
                                " previous_username=" + std::string(iit->second) +
                                " new_username=" + std::string(username));
        return false;
    }
    contributor_id_to_username.emplace(id, username);
    return true;
}

static void record_global_contributor_identity(std::string_view username, std::string_view id,
                                               const std::string &logid,
                                               const std::string &timestamp,
                                               InternedStringMap &contributor_username_to_id,
                                               InternedStringMap &contributor_id_to_username,
                                               InternedStringSet &ambiguous_contributor_usernames,
                                               Stats &stats,
                                               std::vector<std::string> &failure_examples) {
    if (username.empty() || id.empty())
        return;
    ++stats.global_contributor_identities;

    if (ambiguous_contributor_usernames.find(username) != ambiguous_contributor_usernames.end()) {
        (void)record_global_contributor_id_to_username(
            username, id, logid, timestamp, contributor_id_to_username, stats, failure_examples);
        return;
    }

    auto uit = contributor_username_to_id.find(username);
    if (uit != contributor_username_to_id.end() && uit->second != id) {
        ++stats.global_contributor_username_id_conflict;
        add_failure_example(failure_examples, "global_contributor_username_id_conflict", logid,
                            timestamp, std::string(username),
                            "previous_id=" + std::string(uit->second) +
                                " new_id=" + std::string(id));

        // A contributor username can legitimately be reused after rename/usurpation.
        // Once a username maps to multiple account IDs, the timeless username->ID
        // fallback is ambiguous and must not be used for target resolution or
        // username-only creation recovery.  Keep ID->username label evidence for
        // each account, but permanently remove this username from direct lookup.
        contributor_username_to_id.erase(uit);
        ambiguous_contributor_usernames.insert(username);
        (void)record_global_contributor_id_to_username(
            username, id, logid, timestamp, contributor_id_to_username, stats, failure_examples);
        return;
    }

    if (!record_global_contributor_id_to_username(
            username, id, logid, timestamp, contributor_id_to_username, stats, failure_examples)) {
        return;
    }
    contributor_username_to_id.emplace(username, id);
}

static void parse_global_contributor_block_fast(
    std::string_view block, InternedStringMap &contributor_username_to_id,
    InternedStringMap &contributor_id_to_username,
    InternedStringSet &ambiguous_contributor_usernames,
    InternedTimestampMap &first_observed_contributor_timestamp_by_id,
    StringInterner &contributor_string_interner, Stats &stats,
    std::vector<std::string> &failure_examples) {
    if (block.find("<contributor") == std::string_view::npos)
        return;
    std::string logid;
    std::string timestamp;
    extract_first_element_text(block, "id", logid);
    extract_first_element_text(block, "timestamp", timestamp);
    size_t pos = 0;
    while (true) {
        size_t os = 0, oe = 0, cs = 0, ce = 0;
        bool deleted = false, self_closing = false;
        if (!find_element_range(block, "contributor", pos, os, oe, cs, ce, deleted, self_closing))
            return;
        pos = oe + 1;
        if (deleted || self_closing)
            continue;
        std::string_view contributor_block = block.substr(cs, ce - cs);
        std::string username, id;
        extract_first_element_text(contributor_block, "username", username);
        extract_first_element_text(contributor_block, "id", id);
        if (username.empty() || id.empty())
            continue;
        if (!valid_positive_id(id) || !valid_positive_id(logid)) {
            ++stats.identity_id_invalid;
            add_failure_example(failure_examples, "identity_id_invalid", logid, timestamp, username,
                                "invalid contributor or log ID");
            continue;
        }
        const std::string_view username_view = contributor_string_interner.intern(username);
        const std::string_view id_view = contributor_string_interner.intern(id);
        record_first_observed_contributor_timestamp(id_view, timestamp,
                                                    first_observed_contributor_timestamp_by_id);
        record_global_contributor_identity(
            username_view, id_view, logid, timestamp, contributor_username_to_id,
            contributor_id_to_username, ambiguous_contributor_usernames, stats, failure_examples);
    }
}

static bool validate_and_record_logaction_id(const std::string &logid,
                                             std::unordered_set<std::string> &seen_logaction_ids,
                                             Stats &stats,
                                             std::vector<std::string> &failure_examples,
                                             const std::string &timestamp,
                                             const std::string &context) {
    if (logid.empty()) {
        ++stats.logaction_id_missing;
        add_failure_example(failure_examples, "logaction_id_missing", logid, timestamp, context,
                            "logid=<missing_or_empty>");
        return false;
    }
    if (!valid_positive_id(logid)) {
        ++stats.logaction_id_invalid;
        add_failure_example(failure_examples, "logaction_id_invalid", logid, timestamp, context,
                            "expected canonical positive decimal ID");
        return false;
    }
    if (!seen_logaction_ids.insert(logid).second) {
        ++stats.logaction_id_duplicate;
        add_failure_example(failure_examples, "logaction_id_duplicate", logid, timestamp, context,
                            "duplicate logid=" + logid);
        return false;
    }
    return true;
}

static std::string trim_ascii_ws_copy(std::string_view s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return std::string(s.substr(b, e - b));
}

static std::string parse_serialized_userid_param(const std::string &params) {
    const std::string t = trim_ascii_ws_copy(params);
    if (t.empty() || t == "a:0:{}")
        return {};

    bool all_digits = true;
    for (unsigned char c : t) {
        if (!std::isdigit(c)) {
            all_digits = false;
            break;
        }
    }
    if (all_digits)
        return valid_positive_id(t) ? t : std::string{};

    // Common modern PHP-serialized form, e.g. a:1:{s:9:"4::userid";i:123;}
    // Use a suffix match so this also works if the numeric prefix before ::userid changes.
    static constexpr std::string_view key = "::userid\";i:";
    size_t pos = t.find(key);
    if (pos == std::string::npos)
        return {};
    pos += key.size();

    size_t end = pos;
    while (end < t.size() && std::isdigit(static_cast<unsigned char>(t[end])))
        ++end;
    if (end == pos)
        return {};
    if (end >= t.size() || t[end] != ';')
        return {};

    const std::string id = t.substr(pos, end - pos);
    return valid_positive_id(id) ? id : std::string{};
}

static std::string parse_rename_newuser_param(const std::string &params) {
    if (params.empty())
        return {};

    // Modern PHP-serialized rename params contain a key like 5::newuser.
    // Do not hard-code the numeric prefix because it has varied across MediaWiki versions.
    static constexpr std::string_view key = "::newuser\";s:";
    size_t pos = params.find(key);
    if (pos != std::string::npos) {
        pos += key.size();

        size_t len_end = pos;
        while (len_end < params.size() && std::isdigit(static_cast<unsigned char>(params[len_end])))
            ++len_end;
        if (len_end == pos)
            return {};
        if (len_end + 1 >= params.size() || params[len_end] != ':' || params[len_end + 1] != '"')
            return {};

        uint64_t n = 0;
        for (size_t i = pos; i < len_end; ++i) {
            n = n * 10 + static_cast<uint64_t>(params[i] - '0');
            if (n > params.size())
                return {};
        }

        const size_t value_start = len_end + 2;
        if (value_start + n > params.size())
            return {};
        if (value_start + n >= params.size() || params[value_start + n] != '"')
            return {};
        return params.substr(value_start, static_cast<size_t>(n));
    }

    // Very old dumps may contain the new username as a plain params string.
    if (params.find("a:") == 0)
        return {};
    return params;
}

static void record_account_creation(
    const std::string &account_id, const std::string &username, const std::string &timestamp,
    const std::string &evidence, std::unordered_map<std::string, AccountInfo> &accounts_by_id,
    std::unordered_map<std::string, UsernameOnlyCreation> &username_only_creation, Stats &stats,
    std::vector<std::string> &failure_examples, const std::string &logid) {
    (void)failure_examples;
    if (!valid_account_creation_timestamp(timestamp))
        return;
    if (account_id.empty()) {
        if (username.empty())
            return;
        auto uit = username_only_creation.find(username);
        if (uit == username_only_creation.end() || timestamp < uit->second.timestamp) {
            username_only_creation[username] =
                UsernameOnlyCreation{username, timestamp, evidence, logid};
        }
        ++stats.account_creation_by_logtitle_only;
        return;
    }
    auto it = accounts_by_id.find(account_id);
    if (it == accounts_by_id.end()) {
        accounts_by_id.emplace(account_id, AccountInfo{account_id, username, timestamp, evidence});
        if (evidence.find("params_userid") != std::string::npos)
            ++stats.account_creation_by_params_userid;
        else
            ++stats.account_creation_by_contributor;
        return;
    }
    AccountInfo &acc = it->second;
    bool username_differs_from_existing =
        !username.empty() && !acc.username.empty() && acc.username != username;
    if (username_differs_from_existing) {
        ++stats.account_creation_username_differs_from_current;
    }

    if (acc.creation_timestamp.empty() || timestamp < acc.creation_timestamp) {
        ++stats.account_creation_duplicate_earlier;
        acc.creation_timestamp = timestamp;
        acc.creation_evidence =
            evidence + (username_differs_from_existing ? "_username_differs_from_existing" : "");
        if (acc.username.empty())
            acc.username = username;
    } else {
        ++stats.account_creation_duplicate_same;
    }
}

static void
record_account_creation_alias(const std::string &username, const std::string &account_id,
                              const std::string &timestamp, const std::string &evidence,
                              const std::string &logid,
                              std::unordered_map<std::string, std::vector<AccountCreationAlias>>
                                  &account_creation_aliases_by_username,
                              Stats &stats) {
    if (username.empty() || account_id.empty() || timestamp.empty())
        return;
    if (!valid_account_creation_timestamp(timestamp))
        return;
    auto &aliases = account_creation_aliases_by_username[username];
    for (const auto &a : aliases) {
        if (a.account_id == account_id && a.timestamp == timestamp) {
            ++stats.account_creation_alias_duplicate_same;
            return;
        }
    }
    aliases.push_back(AccountCreationAlias{username, account_id, timestamp, evidence, logid});
    ++stats.account_creation_aliases_recorded;
}

static void apply_pending_id_logtitle_creations(
    const std::vector<PendingIdLogtitleCreation> &pending,
    const InternedStringMap &contributor_username_to_id,
    const InternedStringMap &contributor_id_to_username,
    const InternedTimestampMap &first_observed_contributor_timestamp_by_id,
    std::unordered_map<std::string, AccountInfo> &accounts_by_id,
    std::unordered_map<std::string, UsernameOnlyCreation> &username_only_creation,
    std::unordered_map<std::string, std::vector<AccountCreationAlias>>
        &account_creation_aliases_by_username,
    Stats &stats, std::vector<std::string> &failure_examples) {
    (void)contributor_username_to_id;
    for (const PendingIdLogtitleCreation &p : pending) {
        auto iit = contributor_id_to_username.find(std::string_view(p.account_id));
        if (iit == contributor_id_to_username.end()) {
            ++stats.newusers_missing_username_ignored_id_not_current;
            continue;
        }

        auto fit = first_observed_contributor_timestamp_by_id.find(std::string_view(p.account_id));
        if (fit != first_observed_contributor_timestamp_by_id.end() && !fit->second.empty() &&
            fit->second < p.timestamp) {
            ++stats.newusers_missing_username_ignored_after_observed_activity;
            continue;
        }

        // The account ID is stronger evidence than the historical username.  The username may
        // legitimately differ from the current username because of a later rename.
        record_account_creation(p.account_id, p.username, p.timestamp, p.evidence, accounts_by_id,
                                username_only_creation, stats, failure_examples, p.logid);
        record_account_creation_alias(p.username, p.account_id, p.timestamp, p.evidence, p.logid,
                                      account_creation_aliases_by_username, stats);
        ++stats.newusers_missing_username_recovered_from_logtitle;
    }
}

static void apply_pending_username_creations_to_current_ids(
    const std::vector<PendingUsernameCreation> &pending,
    const InternedStringMap &contributor_username_to_id,
    const InternedTimestampMap &first_observed_contributor_timestamp_by_id,
    std::unordered_map<std::string, AccountInfo> &accounts_by_id,
    std::unordered_map<std::string, UsernameOnlyCreation> &username_only_creation, Stats &stats,
    std::vector<std::string> &failure_examples) {
    for (const PendingUsernameCreation &p : pending) {
        auto uit = contributor_username_to_id.find(std::string_view(p.username));
        if (uit == contributor_username_to_id.end()) {
            ++stats.newusers_missing_userid_ignored_username_not_current;
            continue;
        }
        const std::string account_id(uit->second);
        auto fit = first_observed_contributor_timestamp_by_id.find(std::string_view(account_id));
        if (fit != first_observed_contributor_timestamp_by_id.end() && !fit->second.empty() &&
            fit->second < p.timestamp) {
            ++stats.newusers_missing_userid_ignored_after_observed_activity;
            continue;
        }
        record_account_creation(account_id, p.username, p.timestamp, p.evidence, accounts_by_id,
                                username_only_creation, stats, failure_examples, p.logid);
        ++stats.newusers_missing_userid_recovered_from_username;
    }
}

static void apply_username_only_creations_to_current_accounts(
    const std::unordered_map<std::string, UsernameOnlyCreation> &username_only_creation,
    const InternedStringMap &contributor_username_to_id,
    const InternedTimestampMap &first_observed_contributor_timestamp_by_id,
    std::unordered_map<std::string, AccountInfo> &accounts_by_id,
    std::unordered_map<std::string, std::vector<AccountCreationAlias>>
        &account_creation_aliases_by_username,
    Stats &stats) {
    for (const auto &kv : username_only_creation) {
        const UsernameOnlyCreation &u = kv.second;
        auto uit = contributor_username_to_id.find(std::string_view(u.username));
        if (uit == contributor_username_to_id.end()) {
            ++stats.account_creation_logtitle_only_ignored_not_current;
            continue;
        }
        const std::string account_id(uit->second);
        auto existing = accounts_by_id.find(account_id);
        if (existing != accounts_by_id.end() && !existing->second.creation_timestamp.empty())
            continue;
        auto fit = first_observed_contributor_timestamp_by_id.find(std::string_view(account_id));
        if (fit != first_observed_contributor_timestamp_by_id.end() && !fit->second.empty() &&
            fit->second < u.timestamp) {
            ++stats.account_creation_logtitle_only_ignored_after_observed_activity;
            continue;
        }
        AccountInfo &acc = accounts_by_id[account_id];
        acc.account_id = account_id;
        acc.username = u.username;
        acc.creation_timestamp = u.timestamp;
        acc.creation_evidence = u.evidence + "_current_username";
        record_account_creation_alias(u.username, account_id, u.timestamp, u.evidence, u.logid,
                                      account_creation_aliases_by_username, stats);
        ++stats.account_creation_by_logtitle_only_current_username;
    }
}

static void discard_account_creations_after_observed_activity(
    std::unordered_map<std::string, AccountInfo> &accounts_by_id,
    const InternedTimestampMap &first_observed_contributor_timestamp_by_id, Stats &stats) {
    for (auto &kv : accounts_by_id) {
        const std::string &account_id = kv.first;
        AccountInfo &account = kv.second;
        if (account.creation_timestamp.empty())
            continue;
        auto first_seen_it =
            first_observed_contributor_timestamp_by_id.find(std::string_view(account_id));
        if (first_seen_it == first_observed_contributor_timestamp_by_id.end())
            continue;
        const std::string &first_seen = first_seen_it->second;
        if (!first_seen.empty() && first_seen < account.creation_timestamp) {
            ++stats.account_creation_ignored_after_observed_activity;
            account.creation_evidence = "ignored_" + account.creation_evidence +
                                        "_after_observed_activity_at_" + first_seen;
            account.creation_timestamp.clear();
        }
    }
}

static void
parse_newusers_block(std::string_view block, const std::unordered_set<std::string> &namespaces,
                     std::unordered_map<std::string, AccountInfo> &accounts_by_id,
                     std::unordered_map<std::string, UsernameOnlyCreation> &username_only_creation,
                     std::unordered_map<std::string, std::vector<AccountCreationAlias>>
                         &account_creation_aliases_by_username,
                     std::vector<PendingIdLogtitleCreation> &pending_id_logtitle_creations,
                     std::vector<PendingUsernameCreation> &pending_username_creations, Stats &stats,
                     std::vector<std::string> &failure_examples) {
    (void)pending_id_logtitle_creations;
    ++stats.newusers_candidate_blocks;

    std::string frag(block);
    xmlDocPtr doc =
        xmlReadMemory(frag.data(), static_cast<int>(frag.size()), "logitem.xml", nullptr,
                      XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) {
        ++stats.newusers_parse_failures;
        return;
    }

    xmlNode *root = xmlDocGetRootElement(doc);
    if (!root || root->type != XML_ELEMENT_NODE || xml_name(root) != "logitem") {
        ++stats.newusers_parse_failures;
        xmlFreeDoc(doc);
        return;
    }

    std::string logid, timestamp, type, action, contributor_username, contributor_id, logtitle,
        params;
    bool timestamp_deleted = false;
    bool logtitle_deleted = false;
    bool params_deleted = false;

    for (const xmlNode *ch = root->children; ch; ch = ch->next) {
        if (ch->type != XML_ELEMENT_NODE)
            continue;
        const std::string name = xml_name(ch);
        const bool deleted = is_deleted_node(ch);

        if (name == "id") {
            if (!deleted)
                logid = node_content(ch);
        } else if (name == "timestamp") {
            timestamp_deleted = deleted;
            if (!deleted)
                timestamp = node_content(ch);
        } else if (name == "type") {
            type = node_content(ch);
        } else if (name == "action") {
            action = node_content(ch);
        } else if (name == "contributor" && !deleted) {
            const xmlNode *username_node = first_element_child_named(ch, "username");
            const xmlNode *id_node = first_element_child_named(ch, "id");
            if (username_node && !is_deleted_node(username_node))
                contributor_username = node_content(username_node);
            if (id_node && !is_deleted_node(id_node))
                contributor_id = node_content(id_node);
        } else if (name == "logtitle") {
            logtitle_deleted = deleted;
            if (!deleted)
                logtitle = node_content(ch);
        } else if (name == "params") {
            params_deleted = deleted;
            if (!deleted)
                params = node_content(ch);
        }
    }

    if (type != "newusers") {
        xmlFreeDoc(doc);
        return;
    }

    if (!valid_positive_id(logid) ||
        (!contributor_id.empty() && !valid_positive_id(contributor_id))) {
        ++stats.identity_id_invalid;
        xmlFreeDoc(doc);
        return;
    }
    ++stats.newusers_total;

    if (action == "create")
        ++stats.newusers_action_create;
    else if (action == "autocreate")
        ++stats.newusers_action_autocreate;
    else if (action == "newusers")
        ++stats.newusers_action_newusers;
    else if (action == "create2")
        ++stats.newusers_action_create2;
    else if (action == "byemail")
        ++stats.newusers_action_byemail;
    else if (action == "forcecreatelocal")
        ++stats.newusers_action_forcecreatelocal;
    else {
        ++stats.newusers_action_other;
        xmlFreeDoc(doc);
        return;
    }

    if (timestamp_deleted || timestamp.empty()) {
        ++stats.newusers_timestamp_missing;
        xmlFreeDoc(doc);
        return;
    }
    if (!valid_account_creation_timestamp(timestamp)) {
        ++stats.newusers_timestamp_invalid;
        xmlFreeDoc(doc);
        return;
    }

    std::string logtitle_username;
    bool logtitle_username_ok = false;
    if (!logtitle_deleted && !logtitle.empty()) {
        std::string dummy_ns;
        logtitle_username_ok = extract_username_from_logtitle(logtitle, &namespaces, dummy_ns,
                                                              logtitle_username, nullptr) &&
                               !logtitle_username.empty();
    }

    std::string params_userid;
    if (!params_deleted && !params.empty()) {
        params_userid = parse_serialized_userid_param(params);
        // A zero optional userid is not account evidence. For self-creation
        // actions only, a valid contributor supplies the identity independently.
        static const std::regex zero_userid(R"(^a:1:\{s:9:"4::userid";i:0;\}$)");
        const bool zero_with_contributor =
            std::regex_match(params, zero_userid) &&
            (action == "create" || action == "autocreate" || action == "newusers") &&
            valid_positive_id(contributor_id) && !contributor_username.empty();
        if (zero_with_contributor)
            ++stats.newusers_zero_param_ignored;
        if (!zero_with_contributor && params_userid.empty() && !params.empty() &&
            params != "a:0:{}" &&
            (params.find("::userid") != std::string::npos || params.front() != 'a')) {
            ++stats.identity_id_invalid;
            add_failure_example(failure_examples, "identity_id_invalid", logid, timestamp, logtitle,
                                "invalid newusers parameter ID");
        }
    }

    if (action == "create" || action == "autocreate" || action == "newusers") {
        // For these actions the contributor block is the created account when present.
        // logtitle is historical and often mismatches the current actor name after renames/usurpations;
        // never prefer logtitle over contributor.username for the account label.
        // A params userid is useful corroborating evidence, or fallback ID evidence only when contributor.id is missing.
        if (!contributor_id.empty()) {
            if (!params_userid.empty()) {
                if (params_userid == contributor_id) {
                    ++stats.newusers_params_userid_matches_contributor_id;
                } else {
                    ++stats.newusers_params_userid_differs_from_contributor_id;
                    add_failure_example(
                        failure_examples, "newusers_params_userid_differs_from_contributor_id",
                        logid, timestamp,
                        contributor_username.empty() ? logtitle_username : contributor_username,
                        "action=" + action + " contributor_id=" + contributor_id +
                            " params_userid=" + params_userid + " logtitle=" + logtitle);
                }
            }

            if (!contributor_username.empty()) {
                const std::string evidence =
                    !params_userid.empty() && params_userid == contributor_id
                        ? "newusers_contributor_" + action + "_params_confirmed"
                        : "newusers_contributor_" + action;
                record_account_creation(contributor_id, contributor_username, timestamp, evidence,
                                        accounts_by_id, username_only_creation, stats,
                                        failure_examples, logid);
                if (logtitle_username_ok) {
                    // A valid User-namespace logtitle is the best historical username
                    // evidence for the creation-time label.
                    record_account_creation_alias(logtitle_username, contributor_id, timestamp,
                                                  evidence + "_logtitle_alias", logid,
                                                  account_creation_aliases_by_username, stats);
                } else {
                    // Old create/newusers rows often have Special:Userlogin in logtitle.
                    // In that case the contributor username is the only available creation
                    // label, so it should seed an interval.
                    record_account_creation_alias(contributor_username, contributor_id, timestamp,
                                                  evidence + "_contributor_username_alias", logid,
                                                  account_creation_aliases_by_username, stats);
                }
                xmlFreeDoc(doc);
                return;
            }

            ++stats.newusers_username_missing;
            if (logtitle_username_ok) {
                // ID is strong enough; the logtitle username is only auxiliary historical label evidence.
                record_account_creation(contributor_id, logtitle_username, timestamp,
                                        "newusers_contributor_id_logtitle_username_" + action,
                                        accounts_by_id, username_only_creation, stats,
                                        failure_examples, logid);
                record_account_creation_alias(logtitle_username, contributor_id, timestamp,
                                              "newusers_contributor_id_logtitle_username_" +
                                                  action + "_logtitle_alias",
                                              logid, account_creation_aliases_by_username, stats);
                ++stats.newusers_missing_username_recovered_from_logtitle;
            } else {
                record_account_creation(contributor_id, "", timestamp,
                                        "newusers_contributor_id_only_" + action, accounts_by_id,
                                        username_only_creation, stats, failure_examples, logid);
            }
            xmlFreeDoc(doc);
            return;
        }

        if (!params_userid.empty()) {
            std::string created_username;
            if (!contributor_username.empty())
                created_username = contributor_username;
            else if (logtitle_username_ok)
                created_username = logtitle_username;

            if (created_username.empty()) {
                ++stats.newusers_username_missing;
                xmlFreeDoc(doc);
                return;
            }

            const std::string evidence =
                "newusers_params_userid_" + action +
                (contributor_username.empty() ? "_logtitle_username" : "_contributor_username");
            record_account_creation(params_userid, created_username, timestamp, evidence,
                                    accounts_by_id, username_only_creation, stats, failure_examples,
                                    logid);
            if (logtitle_username_ok) {
                record_account_creation_alias(logtitle_username, params_userid, timestamp,
                                              evidence + "_logtitle_alias", logid,
                                              account_creation_aliases_by_username, stats);
            } else if (!contributor_username.empty()) {
                record_account_creation_alias(contributor_username, params_userid, timestamp,
                                              evidence + "_contributor_username_alias", logid,
                                              account_creation_aliases_by_username, stats);
            }
            if (contributor_username.empty())
                ++stats.newusers_missing_username_recovered_from_logtitle;
            xmlFreeDoc(doc);
            return;
        }

        ++stats.newusers_userid_missing;
        if (!contributor_username.empty()) {
            pending_username_creations.push_back(PendingUsernameCreation{
                contributor_username, timestamp,
                "newusers_missing_contributor_id_username_" + action, logid});
            ++stats.newusers_missing_userid_pending_username;
        } else {
            ++stats.newusers_username_missing;
        }

        xmlFreeDoc(doc);
        return;
    }

    if (action == "byemail" || action == "forcecreatelocal") {
        if (!logtitle_username_ok) {
            ++stats.newusers_logtitle_bad_prefix;
            xmlFreeDoc(doc);
            return;
        }
        if (params_deleted || params.empty() || params_userid.empty()) {
            ++stats.newusers_params_userid_missing;
            xmlFreeDoc(doc);
            return;
        }
        record_account_creation(params_userid, logtitle_username, timestamp,
                                "newusers_logtitle_params_userid_" + action, accounts_by_id,
                                username_only_creation, stats, failure_examples, logid);
        record_account_creation_alias(logtitle_username, params_userid, timestamp,
                                      "newusers_logtitle_params_userid_" + action +
                                          "_logtitle_alias",
                                      logid, account_creation_aliases_by_username, stats);
        xmlFreeDoc(doc);
        return;
    }

    if (action == "create2") {
        if (!logtitle_username_ok) {
            ++stats.newusers_logtitle_bad_prefix;
            xmlFreeDoc(doc);
            return;
        }
        if (!params_userid.empty()) {
            record_account_creation(params_userid, logtitle_username, timestamp,
                                    "newusers_logtitle_params_userid_create2", accounts_by_id,
                                    username_only_creation, stats, failure_examples, logid);
            record_account_creation_alias(logtitle_username, params_userid, timestamp,
                                          "newusers_logtitle_params_userid_create2_logtitle_alias",
                                          logid, account_creation_aliases_by_username, stats);
        } else {
            record_account_creation("", logtitle_username, timestamp,
                                    "newusers_logtitle_only_create2", accounts_by_id,
                                    username_only_creation, stats, failure_examples, logid);
        }
        xmlFreeDoc(doc);
        return;
    }

    xmlFreeDoc(doc);
}

static void parse_renameuser_block(std::string_view block,
                                   const std::unordered_set<std::string> &namespaces,
                                   std::vector<RenameEvent> &rename_events, Stats &stats) {
    ++stats.renameuser_candidate_blocks;
    std::string frag(block);
    xmlDocPtr doc =
        xmlReadMemory(frag.data(), static_cast<int>(frag.size()), "logitem.xml", nullptr,
                      XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) {
        ++stats.renameuser_unparsed;
        return;
    }
    xmlNode *root = xmlDocGetRootElement(doc);
    if (!root || root->type != XML_ELEMENT_NODE || xml_name(root) != "logitem") {
        ++stats.renameuser_unparsed;
        xmlFreeDoc(doc);
        return;
    }
    std::string logid, timestamp, type, action, logtitle, params;
    bool timestamp_deleted = false, logtitle_deleted = false, params_deleted = false;
    for (const xmlNode *ch = root->children; ch; ch = ch->next) {
        if (ch->type != XML_ELEMENT_NODE)
            continue;
        const std::string name = xml_name(ch);
        const bool deleted = is_deleted_node(ch);
        if (name == "id") {
            if (!deleted)
                logid = node_content(ch);
        } else if (name == "timestamp") {
            timestamp_deleted = deleted;
            if (!deleted)
                timestamp = node_content(ch);
        } else if (name == "type")
            type = node_content(ch);
        else if (name == "action")
            action = node_content(ch);
        else if (name == "logtitle") {
            logtitle_deleted = deleted;
            if (!deleted)
                logtitle = node_content(ch);
        } else if (name == "params") {
            params_deleted = deleted;
            if (!deleted)
                params = node_content(ch);
        }
    }
    if (type != "renameuser") {
        xmlFreeDoc(doc);
        return;
    }
    if (!valid_positive_id(logid)) {
        ++stats.identity_id_invalid;
        xmlFreeDoc(doc);
        return;
    }
    ++stats.renameuser_total;
    RenameEvent ev;
    ev.logid = logid;
    ev.timestamp = timestamp;
    if (action != "renameuser") {
        ++stats.renameuser_action_not_renameuser;
        ev.parse_status = "action_not_renameuser";
        rename_events.push_back(std::move(ev));
        xmlFreeDoc(doc);
        return;
    }
    if (timestamp_deleted || !valid_account_creation_timestamp(timestamp)) {
        ++stats.renameuser_timestamp_invalid;
        ev.parse_status = "timestamp_invalid";
        rename_events.push_back(std::move(ev));
        xmlFreeDoc(doc);
        return;
    }
    if (logtitle_deleted || logtitle.empty() || params_deleted || params.empty()) {
        ++stats.renameuser_unparsed;
        ev.parse_status = "missing_logtitle_or_params";
        rename_events.push_back(std::move(ev));
        xmlFreeDoc(doc);
        return;
    }
    std::string dummy_ns, old_username;
    if (!extract_username_from_logtitle(logtitle, &namespaces, dummy_ns, old_username, nullptr) ||
        old_username.empty()) {
        ++stats.renameuser_unparsed;
        ev.parse_status = "bad_old_username_logtitle";
        rename_events.push_back(std::move(ev));
        xmlFreeDoc(doc);
        return;
    }
    std::string new_username = parse_rename_newuser_param(params);
    if (new_username.empty()) {
        ++stats.renameuser_unparsed;
        ev.parse_status = "bad_new_username_params";
        rename_events.push_back(std::move(ev));
        xmlFreeDoc(doc);
        return;
    }
    ev.old_username = old_username;
    ev.new_username = new_username;
    ev.parse_status = "parsed";
    ++stats.renameuser_parsed;
    rename_events.push_back(std::move(ev));
    xmlFreeDoc(doc);
}

static void parse_block_action_block(
    std::string_view block,
    std::unordered_map<std::string, std::string> &blocker_first_block_timestamp_by_id, Stats &stats,
    std::vector<std::string> &failure_examples) {
    ++stats.block_candidate_blocks;

    std::string frag(block);
    xmlDocPtr doc =
        xmlReadMemory(frag.data(), static_cast<int>(frag.size()), "logitem.xml", nullptr,
                      XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) {
        ++stats.block_parse_failures;
        add_failure_example(failure_examples, "block_parse_failure", "", "", "",
                            "xmlReadMemory failed");
        return;
    }

    xmlNode *root = xmlDocGetRootElement(doc);
    if (!root || root->type != XML_ELEMENT_NODE || xml_name(root) != "logitem") {
        ++stats.block_parse_failures;
        add_failure_example(failure_examples, "block_parse_failure", "", "", "",
                            "root is not logitem");
        xmlFreeDoc(doc);
        return;
    }

    std::string type, action, logid, timestamp, source_id;
    bool saw_type = false, saw_action = false;
    bool saw_timestamp = false, timestamp_deleted = false;
    bool saw_contributor = false, contributor_deleted = false;

    for (const xmlNode *ch = root->children; ch; ch = ch->next) {
        if (ch->type != XML_ELEMENT_NODE)
            continue;
        const std::string name = xml_name(ch);
        const bool deleted = is_deleted_node(ch);
        if (name == "id") {
            if (!deleted)
                logid = node_content(ch);
        } else if (name == "timestamp") {
            saw_timestamp = true;
            timestamp_deleted = deleted;
            if (!deleted)
                timestamp = node_content(ch);
        } else if (name == "contributor") {
            saw_contributor = true;
            contributor_deleted = deleted;
            if (!deleted) {
                const xmlNode *id_node = first_element_child_named(ch, "id");
                if (id_node && !is_deleted_node(id_node))
                    source_id = node_content(id_node);
            }
        } else if (name == "type") {
            saw_type = true;
            if (!deleted)
                type = node_content(ch);
        } else if (name == "action") {
            saw_action = true;
            if (!deleted)
                action = node_content(ch);
        }
    }

    if (!saw_type || type != "block") {
        ++stats.block_parse_failures;
        add_failure_example(failure_examples, "block_type_parse_mismatch", logid, timestamp, "",
                            "type=" + (saw_type ? type : "<missing>"));
        xmlFreeDoc(doc);
        return;
    }
    if (!saw_action || action != "block") {
        ++stats.block_action_not_block;
        xmlFreeDoc(doc);
        return;
    }
    if (!valid_positive_id(logid)) {
        ++stats.identity_id_invalid;
        xmlFreeDoc(doc);
        return;
    }
    ++stats.block_action_block;

    if (!saw_timestamp || timestamp_deleted || timestamp.empty()) {
        ++stats.block_timestamp_missing;
        xmlFreeDoc(doc);
        return;
    }
    if (!valid_account_creation_timestamp(timestamp)) {
        ++stats.block_timestamp_invalid;
        add_failure_example(failure_examples, "block_timestamp_invalid", logid, timestamp, "",
                            "invalid block timestamp");
        xmlFreeDoc(doc);
        return;
    }
    if (!saw_contributor || contributor_deleted) {
        ++stats.block_contributor_deleted_or_missing;
        xmlFreeDoc(doc);
        return;
    }
    if (!valid_positive_id(source_id)) {
        ++stats.block_source_id_missing;
        xmlFreeDoc(doc);
        return;
    }

    auto it = blocker_first_block_timestamp_by_id.find(source_id);
    if (it == blocker_first_block_timestamp_by_id.end()) {
        blocker_first_block_timestamp_by_id.emplace(source_id, timestamp);
        ++stats.blocker_first_timestamps_recorded;
    } else if (timestamp < it->second) {
        it->second = timestamp;
    }
    xmlFreeDoc(doc);
}

static ParsedThanks parse_thanks_block(std::string_view block,
                                       const std::unordered_set<std::string> &namespaces,
                                       std::string &file_target_namespace,
                                       std::unordered_set<std::string> &seen_logaction_ids,
                                       Stats &stats, std::vector<std::string> &failure_examples) {
    ParsedThanks parsed;
    std::string frag(block);
    xmlDocPtr doc =
        xmlReadMemory(frag.data(), static_cast<int>(frag.size()), "logitem.xml", nullptr,
                      XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) {
        ++stats.parse_failures;
        return parsed;
    }
    xmlNode *root = xmlDocGetRootElement(doc);
    if (!root || root->type != XML_ELEMENT_NODE || xml_name(root) != "logitem") {
        ++stats.parse_failures;
        xmlFreeDoc(doc);
        return parsed;
    }
    std::string type, action, params, logtitle, logid, timestamp, source_username, source_id;
    bool saw_type = false, saw_action = false, saw_params = false, params_deleted = false;
    bool saw_timestamp = false, timestamp_deleted = false;
    bool saw_contributor = false, contributor_deleted = false;
    bool saw_logtitle = false, logtitle_deleted = false, text_deleted = false,
         action_deleted = false;
    for (const xmlNode *ch = root->children; ch; ch = ch->next) {
        if (ch->type != XML_ELEMENT_NODE)
            continue;
        const std::string name = xml_name(ch);
        const bool deleted = is_deleted_node(ch);
        if (name == "id") {
            if (!deleted)
                logid = node_content(ch);
        } else if (name == "timestamp") {
            saw_timestamp = true;
            timestamp_deleted = deleted;
            if (!deleted)
                timestamp = node_content(ch);
        } else if (name == "contributor") {
            saw_contributor = true;
            contributor_deleted = deleted;
            if (!deleted) {
                const xmlNode *username_node = first_element_child_named(ch, "username");
                const xmlNode *id_node = first_element_child_named(ch, "id");
                if (username_node && !is_deleted_node(username_node))
                    source_username = node_content(username_node);
                if (id_node && !is_deleted_node(id_node))
                    source_id = node_content(id_node);
            }
        } else if (name == "type") {
            saw_type = true;
            type = node_content(ch);
        } else if (name == "action") {
            saw_action = true;
            action_deleted = deleted;
            action = node_content(ch);
        } else if (name == "params") {
            saw_params = true;
            params_deleted = deleted;
            if (!deleted)
                params = node_content(ch);
        } else if (name == "logtitle") {
            saw_logtitle = true;
            logtitle_deleted = deleted;
            if (!deleted)
                logtitle = node_content(ch);
        } else if (name == "text" && deleted) {
            text_deleted = true;
        }
    }
    if (!saw_type || type != "thanks") {
        ++stats.non_thanks_after_parse;
        xmlFreeDoc(doc);
        return parsed;
    }
    if (!action_deleted && (!saw_action || action != "thank")) {
        ++stats.thanks_action_not_thank;
        add_failure_example(failure_examples, "thanks_action_not_thank", logid, timestamp, logtitle,
                            "action=" + (saw_action ? action : "<missing>"));
        xmlFreeDoc(doc);
        return parsed;
    }
    if (!validate_and_record_logaction_id(logid, seen_logaction_ids, stats, failure_examples,
                                          timestamp, logtitle)) {
        xmlFreeDoc(doc);
        return parsed;
    }
    if (saw_params && !params_deleted && params != "a:0:{}") {
        ++stats.thanks_params_invalid;
        add_failure_example(failure_examples, "thanks_params_invalid", logid, timestamp, logtitle,
                            "params=" + params);
        xmlFreeDoc(doc);
        return parsed;
    }
    if (!saw_timestamp) {
        ++stats.timestamp_missing;
        add_failure_example(failure_examples, "timestamp_missing", logid, timestamp, logtitle,
                            "timestamp=<missing>");
        xmlFreeDoc(doc);
        return parsed;
    }
    if (!timestamp_deleted) {
        if (timestamp.empty()) {
            ++stats.timestamp_missing;
            add_failure_example(failure_examples, "timestamp_missing", logid, timestamp, logtitle,
                                "timestamp=<empty>");
            xmlFreeDoc(doc);
            return parsed;
        }
        if (!valid_non_deleted_timestamp(timestamp)) {
            ++stats.timestamp_invalid;
            add_failure_example(failure_examples, "timestamp_invalid", logid, timestamp, logtitle,
                                "timestamp does not match required pattern");
            xmlFreeDoc(doc);
            return parsed;
        }
    }
    if (!saw_contributor) {
        ++stats.contributor_missing;
        add_failure_example(failure_examples, "contributor_missing", logid, timestamp, logtitle,
                            "contributor=<missing>");
        xmlFreeDoc(doc);
        return parsed;
    }
    if (!contributor_deleted) {
        if (source_username.empty()) {
            ++stats.source_username_missing;
            add_failure_example(failure_examples, "source_username_missing", logid, timestamp,
                                logtitle, "source.username=<missing_or_empty>");
            xmlFreeDoc(doc);
            return parsed;
        }
        if (source_id.empty()) {
            ++stats.source_id_missing;
            add_failure_example(failure_examples, "source_id_missing", logid, timestamp, logtitle,
                                "source.id=<missing_or_empty>");
            xmlFreeDoc(doc);
            return parsed;
        }
    }
    if (!contributor_deleted && !source_id.empty() && !valid_positive_id(source_id)) {
        ++stats.source_id_invalid;
        add_failure_example(failure_examples, "source_id_invalid", logid, timestamp, logtitle,
                            "expected canonical positive decimal ID");
        xmlFreeDoc(doc);
        return parsed;
    }
    const bool deleted_target_details = logtitle_deleted || text_deleted || action_deleted;
    if (!saw_logtitle && !deleted_target_details) {
        ++stats.target_logtitle_missing;
        add_failure_example(failure_examples, "target_logtitle_missing", logid, timestamp, logtitle,
                            "logtitle=<missing>");
        xmlFreeDoc(doc);
        return parsed;
    }
    if (timestamp_deleted || contributor_deleted || deleted_target_details) {
        std::vector<std::string> deleted_fields;
        if (timestamp_deleted) {
            ++stats.excluded_timestamp_deleted;
            deleted_fields.push_back("timestamp");
        }
        if (contributor_deleted) {
            ++stats.excluded_contributor_deleted;
            deleted_fields.push_back("contributor");
        }
        if (deleted_target_details) {
            ++stats.excluded_logtitle_deleted;
            deleted_fields.push_back("logtitle");
        }
        parsed.exclusion.logid = logid;
        parsed.exclusion.timestamp = timestamp;
        parsed.exclusion.source_username = source_username;
        parsed.exclusion.source_id = source_id;
        parsed.exclusion.contributor_deleted = contributor_deleted;
        parsed.exclusion.logtitle = logtitle;
        parsed.exclusion.deleted_fields = join_semicolon(deleted_fields);
        parsed.excluded_deleted = true;
        ++stats.excluded_deleted;
        xmlFreeDoc(doc);
        return parsed;
    }
    std::string target_username;
    if (!extract_username_from_logtitle(logtitle, &namespaces, file_target_namespace,
                                        target_username, &stats) ||
        target_username.empty()) {
        add_failure_example(failure_examples, "target_logtitle_namespace_invalid", logid, timestamp,
                            logtitle, "target namespace/prefix failed validation");
        xmlFreeDoc(doc);
        return parsed;
    }
    parsed.edge.logid = logid;
    parsed.edge.timestamp = timestamp;
    parsed.edge.source_id = source_id;
    parsed.edge.source_username = source_username;
    parsed.edge.target_username = target_username;
    parsed.valid = true;
    xmlFreeDoc(doc);
    return parsed;
}

static std::string site_from_input_filename(const fs::path &in) {
    const std::string filename = in.filename().string();
    static const std::regex expected(
        R"(^([a-z0-9_]{2,20})wiki-(20[0-9][0-9][01][0-9][0-3][0-9])-pages-logging\.xml(\.gz)?$)");
    std::smatch m;
    if (!std::regex_match(filename, m, expected)) {
        die("input filename does not match expected pattern: " + filename +
            " ; expected ^([a-z0-9_]{2,20})wiki-(20[0-9][0-9][01][0-9][0-3][0-9])-pages-logging\\.xml(\\.gz)?$");
    }
    return m[1].str();
}

static std::string input_date_from_input_filename(const fs::path &in) {
    const std::string filename = in.filename().string();
    static const std::regex expected(
        R"(^([a-z0-9_]{2,20})wiki-(20[0-9][0-9][01][0-9][0-3][0-9])-pages-logging\.xml(\.gz)?$)");
    std::smatch m;
    if (!std::regex_match(filename, m, expected)) {
        die("input filename does not match expected pattern: " + filename +
            " ; expected ^([a-z0-9_]{2,20})wiki-(20[0-9][0-9][01][0-9][0-3][0-9])-pages-logging\\.xml(\\.gz)?$");
    }
    return m[2].str();
}

static std::string timestamp_cutoff_from_input_date(std::string_view input_date) {
    if (input_date.size() != 8)
        die("input date must be YYYYMMDD");
    for (unsigned char c : input_date) {
        if (!std::isdigit(c))
            die("input date must contain only digits: " + std::string(input_date));
    }
    std::string out;
    out.reserve(20);
    out.append(input_date.substr(0, 4));
    out.push_back('-');
    out.append(input_date.substr(4, 2));
    out.push_back('-');
    out.append(input_date.substr(6, 2));
    out.append("T00:00:00Z");
    if (!valid_account_creation_timestamp(out))
        die("invalid input-date cutoff timestamp: " + out);
    return out;
}

static std::string timestamp_cutoff_from_input_filename(const fs::path &in) {
    return timestamp_cutoff_from_input_date(input_date_from_input_filename(in));
}

static OutputPaths output_paths_for(const fs::path &in, const Options &opt) {
    const fs::path root_dir = opt.use_out_dir ? opt.out_dir : in.parent_path();
    const std::string site = site_from_input_filename(in);
    const std::string input_date = input_date_from_input_filename(in);
    const fs::path dir = root_dir / input_date;
    const std::string base = site + "wiki.thanks";
    return OutputPaths{dir / (base + ".edges.csv"),
                       dir / (base + ".nodes.csv"),
                       dir / (base + ".node_metadata.csv"),
                       dir / (base + ".observed_edges.csv"),
                       dir / (base + ".resolved_edges.csv"),
                       dir / (base + ".accounts.csv"),
                       dir / (base + ".rename_events.csv"),
                       dir / (base + ".excluded_deleted.csv"),
                       dir / (base + ".username_intervals.csv"),
                       dir / (base + ".account_creation_candidates.csv"),
                       dir / (base + ".rename_resolution.csv"),
                       dir / (base + ".target_resolution_audit.csv"),
                       dir / (base + ".validation_summary.csv")};
}

static void ensure_outputs_do_not_exist(const OutputPaths &p, const Options &opt) {
    if (opt.force)
        return;

    if (!opt.verbose) {
        for (const fs::path &path :
             {p.network_edges, p.network_nodes, p.node_metadata, p.target_resolution_audit}) {
            if (fs::exists(path))
                die("output exists; use --force to overwrite: " + path.string());
        }
        return;
    }

    for (const fs::path &path :
         {p.observed_edges, p.resolved_edges, p.accounts, p.rename_events, p.excluded_deleted,
          p.username_intervals, p.account_creation_candidates, p.rename_resolution,
          p.target_resolution_audit, p.validation_summary}) {
        if (fs::exists(path))
            die("output exists; use --force to overwrite: " + path.string());
    }
}

static void sort_edges(std::vector<ObservedEdge> &rows) {
    std::sort(rows.begin(), rows.end(), [](const ObservedEdge &a, const ObservedEdge &b) {
        if (a.timestamp != b.timestamp)
            return a.timestamp < b.timestamp;
        return logid_less(a.logid, b.logid);
    });
}

static void sort_rename_events(std::vector<RenameEvent> &events) {
    std::sort(events.begin(), events.end(), [](const RenameEvent &a, const RenameEvent &b) {
        if (a.timestamp != b.timestamp)
            return a.timestamp < b.timestamp;
        return logid_less(a.logid, b.logid);
    });
}

static std::unordered_map<std::string, std::vector<UsernameInterval>>
build_account_creation_alias_intervals(
    std::unordered_map<std::string, std::vector<AccountCreationAlias>>
        &account_creation_aliases_by_username,
    const std::unordered_map<std::string, AccountInfo> &accounts_by_id,
    const std::vector<RenameEvent> &rename_events, Stats &stats,
    bool release_alias_evidence_after_event_copy, bool keep_interval_audit_fields) {
    struct Event {
        std::string timestamp;
        std::string logid;
        int kind = 0; // 0=creation alias, 1=renameuser
        std::string username;
        std::string account_id;
        std::string new_username;
        std::string evidence;
    };

    std::vector<Event> events;
    events.reserve(rename_events.size() + account_creation_aliases_by_username.size());
    const size_t alias_username_count = account_creation_aliases_by_username.size();

    for (const auto &kv : account_creation_aliases_by_username) {
        for (const AccountCreationAlias &a : kv.second) {
            if (a.username.empty() || a.account_id.empty() || a.timestamp.empty())
                continue;
            auto acc_it = accounts_by_id.find(a.account_id);
            if (acc_it == accounts_by_id.end() || acc_it->second.creation_timestamp.empty()) {
                ++stats.account_creation_alias_interval_ignored_no_account_creation;
                continue;
            }
            // Only intervalize aliases whose own timestamp is compatible with the final
            // accepted account-creation timestamp. This prevents late false autocreate
            // rows, later cleared from accounts_by_id, from surviving as target aliases.
            if (a.timestamp < acc_it->second.creation_timestamp) {
                ++stats.account_creation_alias_interval_ignored_no_account_creation;
                continue;
            }
            events.push_back(Event{a.timestamp, a.logid, 0, a.username, a.account_id, std::string(),
                                   keep_interval_audit_fields ? a.evidence : std::string()});
        }
    }

    // In publication mode this evidence is no longer needed after being flattened
    // into the interval-builder event list. Releasing it here avoids keeping the
    // large creation-alias map alive while sorting events and materializing intervals.
    // Publication mode also suppresses interval audit strings below; verbose mode
    // keeps them so username_intervals.csv remains fully documented.
    if (release_alias_evidence_after_event_copy) {
        release_container_memory(account_creation_aliases_by_username);
    }

    for (const RenameEvent &ev : rename_events) {
        if (ev.parse_status != "parsed" || ev.old_username.empty() || ev.new_username.empty() ||
            ev.timestamp.empty())
            continue;
        events.push_back(
            Event{ev.timestamp, ev.logid, 1, ev.old_username, std::string(), ev.new_username,
                  keep_interval_audit_fields ? std::string("renameuser") : std::string()});
    }

    std::sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
        if (a.timestamp != b.timestamp)
            return a.timestamp < b.timestamp;
        if (a.kind != b.kind)
            return a.kind < b.kind; // creation aliases before renames at the same instant
        return logid_less(a.logid, b.logid);
    });

    std::vector<UsernameInterval> intervals;
    intervals.reserve(events.size());
    std::unordered_map<std::string, std::vector<size_t>> open_by_username;
    open_by_username.reserve(alias_username_count * 2 + rename_events.size() + 1);

    auto active_open_indices = [&](const std::string &username, const std::string &timestamp) {
        std::vector<size_t> out;
        auto it = open_by_username.find(username);
        if (it == open_by_username.end())
            return out;
        for (size_t idx : it->second) {
            const UsernameInterval &iv = intervals[idx];
            if (iv.end_timestamp.empty() && iv.start_timestamp <= timestamp)
                out.push_back(idx);
        }
        return out;
    };

    auto open_interval = [&](const std::string &username, const std::string &account_id,
                             const std::string &timestamp, const std::string &evidence,
                             const std::string &logid) {
        if (username.empty() || account_id.empty() || timestamp.empty())
            return;
        std::vector<size_t> active = active_open_indices(username, timestamp);
        for (size_t idx : active) {
            const UsernameInterval &iv = intervals[idx];
            if (iv.account_id == account_id) {
                ++stats.account_creation_alias_duplicate_same;
                return;
            }
        }
        if (!active.empty())
            ++stats.account_creation_alias_interval_overlaps;
        intervals.push_back(UsernameInterval{username, account_id, timestamp, std::string(),
                                             keep_interval_audit_fields ? evidence : std::string(),
                                             keep_interval_audit_fields ? logid : std::string(),
                                             std::string()});
        open_by_username[username].push_back(intervals.size() - 1);
        ++stats.account_creation_alias_intervals_built;
    };

    for (const Event &ev : events) {
        if (ev.kind == 0) {
            open_interval(ev.username, ev.account_id, ev.timestamp, ev.evidence, ev.logid);
            continue;
        }

        std::vector<size_t> active = active_open_indices(ev.username, ev.timestamp);
        if (active.empty()) {
            ++stats.account_creation_alias_interval_rename_old_not_active;
            continue;
        }
        std::unordered_set<std::string> active_ids;
        for (size_t idx : active)
            active_ids.insert(intervals[idx].account_id);
        if (active_ids.size() != 1) {
            ++stats.account_creation_alias_interval_rename_old_ambiguous;
            continue;
        }
        const std::string account_id = *active_ids.begin();
        for (size_t idx : active) {
            if (intervals[idx].account_id == account_id && intervals[idx].end_timestamp.empty()) {
                intervals[idx].end_timestamp = ev.timestamp;
                if (keep_interval_audit_fields)
                    intervals[idx].end_logid = ev.logid;
                ++stats.account_creation_alias_intervals_closed_by_rename;
            }
        }
        open_interval(ev.new_username, account_id, ev.timestamp, "renameuser_interval", ev.logid);
    }

    std::unordered_map<std::string, std::vector<UsernameInterval>> by_username;
    by_username.reserve(open_by_username.size() * 2 + 1);
    for (const UsernameInterval &iv : intervals)
        by_username[iv.username].push_back(iv);
    for (auto &kv : by_username) {
        std::sort(kv.second.begin(), kv.second.end(),
                  [](const UsernameInterval &a, const UsernameInterval &b) {
                      if (a.start_timestamp != b.start_timestamp)
                          return a.start_timestamp < b.start_timestamp;
                      if (a.end_timestamp != b.end_timestamp)
                          return a.end_timestamp < b.end_timestamp;
                      return a.account_id < b.account_id;
                  });
    }
    return by_username;
}

static void invalidate_account_creation_timestamps_disagreeing_with_ids(
    std::unordered_map<std::string, AccountInfo> &accounts_by_id, Stats &stats,
    std::vector<std::string> &failure_examples) {
    std::vector<AccountInfo *> rows;
    rows.reserve(accounts_by_id.size());
    for (auto &kv : accounts_by_id) {
        AccountInfo &acc = kv.second;
        if (acc.account_id.empty() || acc.creation_timestamp.empty())
            continue;
        rows.push_back(&acc);
    }
    if (rows.size() < 2)
        return;

    std::sort(rows.begin(), rows.end(), [](const AccountInfo *a, const AccountInfo *b) {
        return logid_less(a->account_id, b->account_id);
    });

    bool has_order_violation = false;
    for (size_t i = 1; i < rows.size(); ++i) {
        if (rows[i]->creation_timestamp < rows[i - 1]->creation_timestamp) {
            has_order_violation = true;
            break;
        }
    }
    if (!has_order_violation)
        return;

    std::vector<std::string_view> timestamps;
    timestamps.reserve(rows.size());
    for (const AccountInfo *acc : rows)
        timestamps.push_back(acc->creation_timestamp);
    std::sort(timestamps.begin(), timestamps.end());
    timestamps.erase(std::unique(timestamps.begin(), timestamps.end()), timestamps.end());
    if (timestamps.size() < 2)
        return;

    class FenwickMax {
      public:
        explicit FenwickMax(size_t n) : tree_(n + 1, 0) {}
        void update(size_t i, uint32_t value) {
            for (; i < tree_.size(); i += (i & (~i + 1))) {
                if (value > tree_[i])
                    tree_[i] = value;
            }
        }
        uint32_t query(size_t i) const {
            uint32_t out = 0;
            for (; i > 0; i -= (i & (~i + 1))) {
                if (tree_[i] > out)
                    out = tree_[i];
            }
            return out;
        }

      private:
        std::vector<uint32_t> tree_;
    };

    const size_t m = timestamps.size();
    FenwickMax suffix_best(m);
    std::vector<uint32_t> right_len(rows.size(), 1);
    for (size_t rev = rows.size(); rev > 0; --rev) {
        const size_t i = rev - 1;
        const std::string_view ts(rows[i]->creation_timestamp);
        const size_t rank =
            static_cast<size_t>(std::lower_bound(timestamps.begin(), timestamps.end(), ts) -
                                timestamps.begin()) +
            1;
        const size_t reversed_rank = m - rank + 1;
        right_len[i] = suffix_best.query(reversed_rank) + 1;
        suffix_best.update(reversed_rank, right_len[i]);
    }

    uint32_t target_len = 0;
    for (uint32_t len : right_len)
        if (len > target_len)
            target_len = len;
    if (static_cast<size_t>(target_len) == rows.size())
        return;

    // Keep an earliest maximum-cardinality non-decreasing subsequence of creation
    // timestamps after sorting by numeric account ID.  All other accepted creation
    // timestamps are the minimum set that must be invalidated to make the released
    // account creation dates agree with account ID order.
    std::vector<unsigned char> keep(rows.size(), 0);
    std::string_view previous_timestamp;
    bool have_previous_timestamp = false;
    uint32_t needed = target_len;
    for (size_t i = 0; i < rows.size() && needed > 0; ++i) {
        const std::string_view ts(rows[i]->creation_timestamp);
        if ((!have_previous_timestamp || ts >= previous_timestamp) && right_len[i] >= needed) {
            keep[i] = 1;
            previous_timestamp = ts;
            have_previous_timestamp = true;
            --needed;
        }
    }

    for (size_t i = 0; i < rows.size(); ++i) {
        if (keep[i])
            continue;
        AccountInfo &acc = *rows[i];
        if (acc.creation_timestamp.empty())
            continue;
        const std::string old_timestamp = acc.creation_timestamp;
        const std::string old_evidence = acc.creation_evidence;
        ++stats.account_creation_timestamp_id_order_invalidated;
        add_failure_example(failure_examples, "account_creation_timestamp_id_order_invalidated", "",
                            old_timestamp, acc.username.empty() ? acc.account_id : acc.username,
                            "account=" + acc.account_id + " creation_evidence=" +
                                (old_evidence.empty() ? std::string("<empty>") : old_evidence));
        acc.creation_timestamp.clear();
        acc.creation_evidence = old_evidence.empty()
                                    ? "invalidated_account_id_timestamp_order"
                                    : "invalidated_" + old_evidence + "_account_id_timestamp_order";
    }
}

static void validate_thanks_logid_timestamp_order_visit(
    const ObservedEdge *row, const ObservedEdge *&max_timestamp_row,
    std::string_view &max_timestamp, Stats &stats, std::vector<std::string> &failure_examples) {
    const std::string_view ts(row->timestamp);
    if (max_timestamp_row && !ts.empty() && ts < max_timestamp) {
        ++stats.thanks_logid_timestamp_order_violations;
        add_failure_example(failure_examples, "thanks_logid_timestamp_order_violation", row->logid,
                            row->timestamp, row->source_username,
                            "lower_logid=" + max_timestamp_row->logid + " lower_logid_timestamp=" +
                                std::string(max_timestamp) + " later_logid=" + row->logid +
                                " later_logid_timestamp=" + row->timestamp);
    }
    if (!ts.empty() && (!max_timestamp_row || ts > max_timestamp)) {
        max_timestamp_row = row;
        max_timestamp = ts;
    }
}

static void validate_thanks_logid_timestamp_order(const std::vector<ObservedEdge> &edges,
                                                  Stats &stats,
                                                  std::vector<std::string> &failure_examples) {
    if (edges.size() < 2)
        return;

    bool already_in_logid_order = true;
    for (size_t i = 1; i < edges.size(); ++i) {
        if (logid_less(edges[i].logid, edges[i - 1].logid)) {
            already_in_logid_order = false;
            break;
        }
    }

    const ObservedEdge *max_timestamp_row = nullptr;
    std::string_view max_timestamp;

    if (already_in_logid_order) {
        for (const ObservedEdge &row : edges) {
            validate_thanks_logid_timestamp_order_visit(&row, max_timestamp_row, max_timestamp,
                                                        stats, failure_examples);
        }
        return;
    }

    std::vector<const ObservedEdge *> rows;
    rows.reserve(edges.size());
    for (const ObservedEdge &row : edges)
        rows.push_back(&row);
    std::sort(rows.begin(), rows.end(), [](const ObservedEdge *a, const ObservedEdge *b) {
        return logid_less(a->logid, b->logid);
    });

    for (const ObservedEdge *row : rows) {
        validate_thanks_logid_timestamp_order_visit(row, max_timestamp_row, max_timestamp, stats,
                                                    failure_examples);
    }
}

static void resolve_targets(std::vector<ObservedEdge> &edges,
                            const InternedStringMap &contributor_username_to_id,
                            const std::unordered_map<std::string, AccountInfo> &accounts_by_id,
                            const std::unordered_map<std::string, std::vector<UsernameInterval>>
                                &username_intervals_by_username,
                            Stats &stats) {
    auto creation_for_id = [&](const std::string &id) -> std::string {
        auto it = accounts_by_id.find(id);
        if (it == accounts_by_id.end())
            return {};
        return it->second.creation_timestamp;
    };

    auto active_interval_ids = [&](const std::string &username, const std::string &timestamp) {
        std::unordered_set<std::string> ids;
        auto it = username_intervals_by_username.find(username);
        if (it == username_intervals_by_username.end())
            return ids;
        for (const UsernameInterval &iv : it->second) {
            if (iv.start_timestamp <= timestamp &&
                (iv.end_timestamp.empty() || timestamp < iv.end_timestamp)) {
                ids.insert(iv.account_id);
            }
        }
        return ids;
    };

    for (ObservedEdge &row : edges) {
        const auto ids = active_interval_ids(row.target_username, row.timestamp);
        if (ids.size() > 1) {
            row.target_resolution = "ambiguous_multiple_active_intervals";
            ++stats.target_resolution_ambiguous_creation_alias;
            continue;
        }

        if (ids.size() == 1) {
            const std::string interval_id = *ids.begin();
            std::string c = creation_for_id(interval_id);
            if (!c.empty() && row.timestamp < c) {
                row.target_resolution = "missing_interval_account_created_after_thank";
                ++stats.target_resolution_missing_created_after_thank;
                continue;
            }

            row.target_account = interval_id;
            auto direct_it = contributor_username_to_id.find(std::string_view(row.target_username));
            if (direct_it != contributor_username_to_id.end() &&
                direct_it->second == std::string_view(interval_id)) {
                row.target_resolution = "interval_and_current_username_same";
                ++stats.target_resolution_direct_and_creation_alias_same;
            } else {
                // A dump log title can have been rewritten by a later rename.
                // Event-time interval ownership alone cannot establish its recipient.
                // Preserve the event as a label node; the supplement retains the
                // candidate interval and current-name evidence for external review.
                row.target_account.clear();
                row.target_resolution = "missing_historical_label_confirmation";
                ++stats.target_resolution_missing_unresolved_rename_chain;
            }
            continue;
        }

        // No time-bounded interval supports this username at this timestamp.  Fall back
        // only to the current contributor map, and label it explicitly as current-name
        // evidence rather than historical interval evidence.
        auto direct_it = contributor_username_to_id.find(std::string_view(row.target_username));
        if (direct_it == contributor_username_to_id.end()) {
            row.target_resolution = "missing_no_identity_evidence";
            ++stats.target_resolution_missing_no_current;
            continue;
        }

        const std::string direct_id(direct_it->second);
        const std::string direct_creation = creation_for_id(direct_id);
        if (!direct_creation.empty() && row.timestamp < direct_creation) {
            row.target_resolution = "missing_current_created_after_thank";
            ++stats.target_resolution_missing_created_after_thank;
        } else if (direct_creation.empty()) {
            row.target_account = direct_id;
            row.target_resolution = "current_username_no_creation_timestamp";
            ++stats.target_resolution_direct_current_no_creation;
        } else {
            row.target_account = direct_id;
            row.target_resolution = "current_username_no_interval_conflict";
            ++stats.target_resolution_direct_current;
        }
    }
}

static void decrement_target_resolution_stat_for_dropped_edge(const std::string &resolution,
                                                              Stats &stats) {
    auto dec = [](uint64_t &x) {
        if (x == 0)
            die("target-resolution counter underflow while dropping self-loop");
        --x;
    };
    if (resolution == "current_username_no_interval_conflict")
        dec(stats.target_resolution_direct_current);
    else if (resolution == "current_username_no_creation_timestamp")
        dec(stats.target_resolution_direct_current_no_creation);
    else if (resolution == "interval_exact")
        dec(stats.target_resolution_creation_alias);
    else if (resolution == "interval_and_current_username_same")
        dec(stats.target_resolution_direct_and_creation_alias_same);
    else if (resolution == "ambiguous_multiple_active_intervals")
        dec(stats.target_resolution_ambiguous_creation_alias);
    else if (resolution == "missing_current_created_after_thank" ||
             resolution == "missing_interval_account_created_after_thank")
        dec(stats.target_resolution_missing_created_after_thank);
    else if (resolution == "missing_no_identity_evidence")
        dec(stats.target_resolution_missing_no_current);
    else if (resolution == "missing_unresolved_rename_chain" ||
             resolution == "missing_historical_label_confirmation")
        dec(stats.target_resolution_missing_unresolved_rename_chain);
    else if (!resolution.empty())
        dec(stats.target_resolution_rename_chain);
}

static void drop_resolved_self_loops(std::vector<ObservedEdge> &edges, Stats &stats) {
    size_t write = 0;
    for (size_t read = 0; read < edges.size(); ++read) {
        ObservedEdge &row = edges[read];
        const bool self_loop = !row.source_id.empty() && !row.target_account.empty() &&
                               row.source_id == row.target_account;
        if (self_loop) {
            decrement_target_resolution_stat_for_dropped_edge(row.target_resolution, stats);
            ++stats.self_loop_edges_dropped;
            continue;
        }
        if (write != read)
            edges[write] = std::move(row);
        ++write;
    }
    if (write != edges.size())
        edges.resize(write);
    if (stats.self_loop_edges_dropped > stats.retained_observed_edges) {
        die("self-loop drop count exceeds retained observed edge count");
    }
    stats.retained_observed_edges -= stats.self_loop_edges_dropped;
}

static void validate_source_temporal_consistency(
    const std::vector<ObservedEdge> &edges,
    const std::unordered_map<std::string, AccountInfo> &accounts_by_id, Stats &stats,
    std::vector<std::string> &failure_examples) {
    for (const ObservedEdge &row : edges) {
        auto sit = accounts_by_id.find(row.source_id);
        if (sit != accounts_by_id.end() && !sit->second.creation_timestamp.empty() &&
            row.timestamp < sit->second.creation_timestamp) {
            ++stats.source_thanks_before_account_creation;
            add_failure_example(
                failure_examples, "source_thanks_before_account_creation", row.logid, row.timestamp,
                row.source_username,
                "source_account=" + row.source_id + " account_username=" + sit->second.username +
                    " account_creation_timestamp=" + sit->second.creation_timestamp +
                    " creation_evidence=" + sit->second.creation_evidence);
        }
        if (!row.target_account.empty()) {
            auto tit = accounts_by_id.find(row.target_account);
            if (tit != accounts_by_id.end() && !tit->second.creation_timestamp.empty() &&
                row.timestamp < tit->second.creation_timestamp) {
                ++stats.target_resolved_thanks_before_account_creation;
                add_failure_example(
                    failure_examples, "target_resolved_thanks_before_account_creation", row.logid,
                    row.timestamp, row.target_username,
                    "target_account=" + row.target_account +
                        " account_username=" + tit->second.username +
                        " account_creation_timestamp=" + tit->second.creation_timestamp +
                        " creation_evidence=" + tit->second.creation_evidence +
                        " target_resolution=" + row.target_resolution);
            }
        }
    }
}

static void write_observed_edges_csv(const fs::path &path, const std::string &site,
                                     const std::vector<ObservedEdge> &rows, Stats &stats) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        die("cannot open observed_edges CSV for writing: " + path.string());
    write_csv_cell(out, "site");
    out.put(',');
    write_csv_cell(out, "logid");
    out.put(',');
    write_csv_cell(out, "timestamp");
    out.put(',');
    write_csv_cell(out, "source_account");
    out.put(',');
    write_csv_cell(out, "source_username");
    out.put(',');
    write_csv_cell(out, "target_username");
    out.put('\n');
    for (const ObservedEdge &row : rows) {
        write_csv_cell(out, site);
        out.put(',');
        write_csv_cell(out, row.logid);
        out.put(',');
        write_csv_cell(out, row.timestamp);
        out.put(',');
        write_csv_cell(out, row.source_id);
        out.put(',');
        write_csv_cell(out, row.source_username);
        out.put(',');
        write_csv_cell(out, row.target_username);
        out.put('\n');
        ++stats.observed_edges_rows;
    }
    if (!out)
        die("failed writing observed_edges CSV: " + path.string());
}

static void write_resolved_edges_csv(const fs::path &path, const std::string &site,
                                     const std::vector<ObservedEdge> &rows, Stats &stats) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        die("cannot open resolved_edges CSV for writing: " + path.string());
    write_csv_cell(out, "site");
    out.put(',');
    write_csv_cell(out, "logid");
    out.put(',');
    write_csv_cell(out, "timestamp");
    out.put(',');
    write_csv_cell(out, "source_account");
    out.put(',');
    write_csv_cell(out, "target_account");
    out.put(',');
    write_csv_cell(out, "target_resolution");
    out.put('\n');
    for (const ObservedEdge &row : rows) {
        write_csv_cell(out, site);
        out.put(',');
        write_csv_cell(out, row.logid);
        out.put(',');
        write_csv_cell(out, row.timestamp);
        out.put(',');
        write_csv_cell(out, row.source_id);
        out.put(',');
        write_csv_cell(out, row.target_account);
        out.put(',');
        write_csv_cell(out, row.target_resolution);
        out.put('\n');
        ++stats.resolved_edges_rows;
    }
    if (!out)
        die("failed writing resolved_edges CSV: " + path.string());
}

static void write_accounts_csv(const fs::path &path, const std::string &site,
                               const InternedStringMap &contributor_id_to_username,
                               const std::unordered_map<std::string, AccountInfo> &accounts_by_id,
                               Stats &stats) {
    std::unordered_map<std::string, std::string> username_by_id;
    username_by_id.reserve(contributor_id_to_username.size() + accounts_by_id.size());
    for (const auto &kv : accounts_by_id) {
        username_by_id[kv.first] = kv.second.username;
    }
    // Prefer the global contributor map for the displayed current username when available.
    for (const auto &kv : contributor_id_to_username) {
        username_by_id[std::string(kv.first)] = std::string(kv.second);
    }

    std::vector<std::pair<std::string, std::string>> accounts;
    accounts.reserve(username_by_id.size());
    for (const auto &kv : username_by_id)
        accounts.emplace_back(kv.first, kv.second);
    std::sort(accounts.begin(), accounts.end(),
              [](const auto &a, const auto &b) { return logid_less(a.first, b.first); });

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        die("cannot open accounts CSV for writing: " + path.string());
    write_csv_cell(out, "site");
    out.put(',');
    write_csv_cell(out, "account");
    out.put(',');
    write_csv_cell(out, "current_username");
    out.put(',');
    write_csv_cell(out, "account_creation_timestamp");
    out.put(',');
    write_csv_cell(out, "creation_evidence");
    out.put('\n');
    for (const auto &kv : accounts) {
        const std::string &id = kv.first;
        const std::string &username = kv.second;
        std::string created, evidence;
        auto it = accounts_by_id.find(id);
        if (it != accounts_by_id.end()) {
            created = it->second.creation_timestamp;
            evidence = it->second.creation_evidence;
        }
        write_csv_cell(out, site);
        out.put(',');
        write_csv_cell(out, id);
        out.put(',');
        write_csv_cell(out, username);
        out.put(',');
        write_csv_cell(out, created);
        out.put(',');
        write_csv_cell(out, evidence);
        out.put('\n');
        ++stats.accounts_rows;
    }
    if (!out)
        die("failed writing accounts CSV: " + path.string());
}

static void write_rename_events_csv(const fs::path &path, const std::string &site,
                                    const std::vector<RenameEvent> &events, Stats &stats) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        die("cannot open rename_events CSV for writing: " + path.string());
    write_csv_cell(out, "site");
    out.put(',');
    write_csv_cell(out, "logid");
    out.put(',');
    write_csv_cell(out, "timestamp");
    out.put(',');
    write_csv_cell(out, "old_username");
    out.put(',');
    write_csv_cell(out, "new_username");
    out.put(',');
    write_csv_cell(out, "parse_status");
    out.put('\n');
    for (const RenameEvent &ev : events) {
        write_csv_cell(out, site);
        out.put(',');
        write_csv_cell(out, ev.logid);
        out.put(',');
        write_csv_cell(out, ev.timestamp);
        out.put(',');
        write_csv_cell(out, ev.old_username);
        out.put(',');
        write_csv_cell(out, ev.new_username);
        out.put(',');
        write_csv_cell(out, ev.parse_status);
        out.put('\n');
        ++stats.rename_events_rows;
    }
    if (!out)
        die("failed writing rename_events CSV: " + path.string());
}

static void write_excluded_deleted_csv(const fs::path &path, const std::string &site,
                                       const std::vector<DeletedExclusion> &exclusions,
                                       Stats &stats) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        die("cannot open excluded_deleted CSV for writing: " + path.string());
    write_csv_cell(out, "site");
    out.put(',');
    write_csv_cell(out, "logid");
    out.put(',');
    write_csv_cell(out, "timestamp");
    out.put(',');
    write_csv_cell(out, "source_username");
    out.put(',');
    write_csv_cell(out, "logtitle");
    out.put(',');
    write_csv_cell(out, "deleted_fields");
    out.put('\n');
    for (const DeletedExclusion &ex : exclusions) {
        write_csv_cell(out, site);
        out.put(',');
        write_csv_cell(out, ex.logid);
        out.put(',');
        write_csv_cell(out, ex.timestamp);
        out.put(',');
        write_csv_cell(out, ex.source_username);
        out.put(',');
        write_csv_cell(out, ex.logtitle);
        out.put(',');
        write_csv_cell(out, ex.deleted_fields);
        out.put('\n');
        ++stats.excluded_deleted_rows_written;
    }
    if (!out)
        die("failed writing excluded_deleted CSV: " + path.string());
}

static void write_username_intervals_csv(
    const fs::path &path, const std::string &site,
    const std::unordered_map<std::string, std::vector<UsernameInterval>> &by_username) {
    std::vector<UsernameInterval> rows;
    for (const auto &kv : by_username) {
        for (const UsernameInterval &iv : kv.second)
            rows.push_back(iv);
    }
    std::sort(rows.begin(), rows.end(), [](const UsernameInterval &a, const UsernameInterval &b) {
        if (a.username != b.username)
            return a.username < b.username;
        if (a.start_timestamp != b.start_timestamp)
            return a.start_timestamp < b.start_timestamp;
        if (a.end_timestamp != b.end_timestamp)
            return a.end_timestamp < b.end_timestamp;
        return logid_less(a.account_id, b.account_id);
    });

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        die("cannot open username_intervals CSV for writing: " + path.string());
    for (const char *h : {"site", "account", "username", "start_timestamp", "end_timestamp",
                          "start_evidence", "start_logid", "end_logid", "confidence"}) {
        if (h != std::string("site"))
            out.put(',');
        write_csv_cell(out, h);
    }
    out.put('\n');
    for (const UsernameInterval &iv : rows) {
        write_csv_cell(out, site);
        out.put(',');
        write_csv_cell(out, iv.account_id);
        out.put(',');
        write_csv_cell(out, iv.username);
        out.put(',');
        write_csv_cell(out, iv.start_timestamp);
        out.put(',');
        write_csv_cell(out, iv.end_timestamp);
        out.put(',');
        write_csv_cell(out, iv.start_evidence);
        out.put(',');
        write_csv_cell(out, iv.start_logid);
        out.put(',');
        write_csv_cell(out, iv.end_logid);
        out.put(',');
        write_csv_cell(out, iv.start_evidence == "renameuser_interval" ? "rename_inferred"
                                                                       : "id_level_creation_alias");
        out.put('\n');
    }
    if (!out)
        die("failed writing username_intervals CSV: " + path.string());
}

static void write_account_creation_candidates_csv(
    const fs::path &path, const std::string &site,
    const std::unordered_map<std::string, AccountInfo> &accounts_by_id,
    const std::unordered_map<std::string, UsernameOnlyCreation> &username_only_creation,
    const std::unordered_map<std::string, std::vector<AccountCreationAlias>> &aliases_by_username) {
    struct Row {
        std::string account, username, timestamp, evidence, logid, accepted, kind;
    };
    std::vector<Row> rows;
    rows.reserve(accounts_by_id.size() + username_only_creation.size());
    for (const auto &kv : accounts_by_id) {
        const AccountInfo &a = kv.second;
        if (!a.creation_timestamp.empty()) {
            rows.push_back(Row{a.account_id, a.username, a.creation_timestamp, a.creation_evidence,
                               "", "yes", "account_creation_selected"});
        } else {
            rows.push_back(Row{a.account_id, a.username, "", a.creation_evidence, "", "no",
                               "account_known_no_creation"});
        }
    }
    for (const auto &kv : username_only_creation) {
        const auto &u = kv.second;
        rows.push_back(Row{"", u.username, u.timestamp, u.evidence, u.logid,
                           "pending_or_used_if_current", "username_only_creation"});
    }
    for (const auto &kv : aliases_by_username) {
        for (const AccountCreationAlias &a : kv.second) {
            rows.push_back(Row{a.account_id, a.username, a.timestamp, a.evidence, a.logid,
                               "alias_evidence", "creation_username_alias"});
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.timestamp != b.timestamp)
            return a.timestamp < b.timestamp;
        if (a.username != b.username)
            return a.username < b.username;
        return logid_less(a.account, b.account);
    });
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        die("cannot open account_creation_candidates CSV for writing: " + path.string());
    write_csv_cell(out, "site");
    out.put(',');
    write_csv_cell(out, "account");
    out.put(',');
    write_csv_cell(out, "username");
    out.put(',');
    write_csv_cell(out, "timestamp");
    out.put(',');
    write_csv_cell(out, "evidence");
    out.put(',');
    write_csv_cell(out, "logid");
    out.put(',');
    write_csv_cell(out, "accepted");
    out.put(',');
    write_csv_cell(out, "kind");
    out.put('\n');
    for (const Row &r : rows) {
        write_csv_cell(out, site);
        out.put(',');
        write_csv_cell(out, r.account);
        out.put(',');
        write_csv_cell(out, r.username);
        out.put(',');
        write_csv_cell(out, r.timestamp);
        out.put(',');
        write_csv_cell(out, r.evidence);
        out.put(',');
        write_csv_cell(out, r.logid);
        out.put(',');
        write_csv_cell(out, r.accepted);
        out.put(',');
        write_csv_cell(out, r.kind);
        out.put('\n');
    }
    if (!out)
        die("failed writing account_creation_candidates CSV: " + path.string());
}

static void write_rename_resolution_csv(const fs::path &path, const std::string &site,
                                        const std::vector<RenameEvent> &events) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        die("cannot open rename_resolution CSV for writing: " + path.string());
    write_csv_cell(out, "site");
    out.put(',');
    write_csv_cell(out, "logid");
    out.put(',');
    write_csv_cell(out, "timestamp");
    out.put(',');
    write_csv_cell(out, "old_username");
    out.put(',');
    write_csv_cell(out, "new_username");
    out.put(',');
    write_csv_cell(out, "parse_status");
    out.put(',');
    write_csv_cell(out, "resolution_status");
    out.put('\n');
    for (const RenameEvent &ev : events) {
        write_csv_cell(out, site);
        out.put(',');
        write_csv_cell(out, ev.logid);
        out.put(',');
        write_csv_cell(out, ev.timestamp);
        out.put(',');
        write_csv_cell(out, ev.old_username);
        out.put(',');
        write_csv_cell(out, ev.new_username);
        out.put(',');
        write_csv_cell(out, ev.parse_status);
        out.put(',');
        write_csv_cell(out, ev.parse_status == "parsed"
                                ? "used_by_interval_builder_if_old_username_active"
                                : "not_used");
        out.put('\n');
    }
    if (!out)
        die("failed writing rename_resolution CSV: " + path.string());
}

static bool is_account_id_timestamp_order_invalidated_evidence(const std::string &evidence) {
    static constexpr std::string_view prefix = "invalidated_";
    static constexpr std::string_view suffix = "_account_id_timestamp_order";
    return evidence.size() > prefix.size() + suffix.size() &&
           evidence.compare(0, prefix.size(), prefix) == 0 && ends_with(evidence, suffix);
}

static std::string evidence_label_for_publication_codebook(const std::string &evidence) {
    static constexpr std::string_view prefix = "invalidated_";
    static constexpr std::string_view suffix = "_account_id_timestamp_order";
    if (evidence.size() > prefix.size() + suffix.size() &&
        evidence.compare(0, prefix.size(), prefix) == 0 && ends_with(evidence, suffix)) {
        return evidence.substr(prefix.size(), evidence.size() - prefix.size() - suffix.size());
    }
    return evidence;
}

struct PublicationNodeInfo {
    std::string id;
    std::string node_type = "account";
    std::string label;
    std::string creation_timestamp;
    std::string creation_evidence_label;
    int creation_evidence_code = 0;
    std::string first_block_by_user_timestamp;

    uint64_t sent = 0;
    uint64_t received = 0;
    uint64_t sent_to_resolved_target = 0;
    uint64_t sent_to_unresolved_or_ambiguous_target = 0;

    uint64_t received_interval_exact = 0;
    uint64_t received_interval_and_current_username_same = 0;
    uint64_t received_current_username_no_interval_conflict = 0;
    uint64_t received_current_username_no_creation_timestamp = 0;
    uint64_t received_other_resolved = 0;

    uint64_t received_synthetic_missing_no_identity_evidence = 0;
    uint64_t received_synthetic_ambiguous_multiple_active_intervals = 0;
    uint64_t received_synthetic_missing_current_created_after_thank = 0;
    uint64_t received_synthetic_missing_interval_account_created_after_thank = 0;
    uint64_t received_synthetic_other_unresolved = 0;
};

static std::string
current_username_for_account(const std::string &account_id,
                             const InternedStringMap &contributor_id_to_username,
                             const std::unordered_map<std::string, AccountInfo> &accounts_by_id) {
    auto cit = contributor_id_to_username.find(std::string_view(account_id));
    if (cit != contributor_id_to_username.end())
        return std::string(cit->second);
    auto ait = accounts_by_id.find(account_id);
    if (ait != accounts_by_id.end())
        return ait->second.username;
    return {};
}

static std::unordered_map<std::string, std::string>
build_synthetic_target_ids(const std::vector<ObservedEdge> &edges) {
    std::vector<std::string> unresolved_usernames;
    unresolved_usernames.reserve(1024);

    for (const ObservedEdge &row : edges) {
        if (row.target_account.empty() && !row.target_username.empty()) {
            unresolved_usernames.push_back(row.target_username);
        }
    }

    std::sort(unresolved_usernames.begin(), unresolved_usernames.end());
    unresolved_usernames.erase(
        std::unique(unresolved_usernames.begin(), unresolved_usernames.end()),
        unresolved_usernames.end());

    std::unordered_map<std::string, std::string> id_by_username;
    id_by_username.reserve(unresolved_usernames.size() * 2 + 1);
    for (size_t i = 0; i < unresolved_usernames.size(); ++i) {
        id_by_username.emplace(unresolved_usernames[i], "-" + std::to_string(i + 1));
    }
    return id_by_username;
}

// Evidence in this supplement describes the dump, not independently verified
// historical recipient identities. Intervals must not imply original title text.
static void write_target_resolution_audit_csv(
    const fs::path &path, const std::string &site, const std::vector<ObservedEdge> &rows,
    const InternedStringMap &current, const std::unordered_map<std::string, AccountInfo> &accounts,
    const std::unordered_map<std::string, std::vector<UsernameInterval>> &intervals,
    const std::vector<PublicationNodeInfo> *publication_nodes = nullptr) {
    const auto synthetic = build_synthetic_target_ids(rows);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        die("cannot open target audit: " + path.string());
    if (publication_nodes) {
        out << "record_type,site,id,node_type,label,creation_timestamp_missing,creation_evidence,creation_evidence_label,sent,received,sent_to_resolved_target,sent_to_unresolved_or_ambiguous_target,received_interval_exact,received_interval_and_current_username_same,received_current_username_no_interval_conflict,received_current_username_no_creation_timestamp,received_other_resolved,received_synthetic_missing_no_identity_evidence,received_synthetic_ambiguous_multiple_active_intervals,received_synthetic_missing_current_created_after_thank,received_synthetic_missing_interval_account_created_after_thank,received_synthetic_other_unresolved,logid,timestamp,source,target,observed_target_label,target_label_subtype,target_resolution,current_name_account_id,current_name_creation_timestamp,resolved_account_creation_timestamp,interval_evidence,requires_historical_review\n";
        for (const PublicationNodeInfo &n : *publication_nodes) {
            const std::vector<std::string> cells = {
                "node",
                site,
                n.id,
                n.node_type,
                n.label,
                n.creation_timestamp.empty() ? "1" : "0",
                std::to_string(n.creation_evidence_code),
                n.creation_evidence_label,
                std::to_string(n.sent),
                std::to_string(n.received),
                std::to_string(n.sent_to_resolved_target),
                std::to_string(n.sent_to_unresolved_or_ambiguous_target),
                std::to_string(n.received_interval_exact),
                std::to_string(n.received_interval_and_current_username_same),
                std::to_string(n.received_current_username_no_interval_conflict),
                std::to_string(n.received_current_username_no_creation_timestamp),
                std::to_string(n.received_other_resolved),
                std::to_string(n.received_synthetic_missing_no_identity_evidence),
                std::to_string(n.received_synthetic_ambiguous_multiple_active_intervals),
                std::to_string(n.received_synthetic_missing_current_created_after_thank),
                std::to_string(n.received_synthetic_missing_interval_account_created_after_thank),
                std::to_string(n.received_synthetic_other_unresolved),
                "",
                "",
                "",
                "",
                "",
                "",
                "",
                "",
                "",
                "",
                "",
                ""};
            for (size_t i = 0; i < cells.size(); ++i) {
                if (i)
                    out.put(',');
                write_csv_cell(out, cells[i]);
            }
            out.put('\n');
        }
    } else {
        out << "site,logid,timestamp,source,target,observed_target_label,target_label_subtype,target_resolution,current_name_account_id,current_name_creation_timestamp,resolved_account_creation_timestamp,interval_evidence,requires_historical_review\n";
    }
    for (const auto &row : rows) {
        auto ci = current.find(row.target_username);
        std::string cid = ci == current.end() ? "" : std::string(ci->second);
        auto creation = [&](const std::string &id) {
            auto ai = accounts.find(id);
            return ai == accounts.end() ? std::string{} : ai->second.creation_timestamp;
        };
        std::vector<std::string> evidence;
        auto ii = intervals.find(row.target_username);
        if (ii != intervals.end())
            for (const auto &iv : ii->second) {
                // Each tuple: ID|start inclusive|end exclusive|start logid|end logid|start evidence.
                evidence.push_back(iv.account_id + "|" + iv.start_timestamp + "|" +
                                   iv.end_timestamp + "|" + iv.start_logid + "|" + iv.end_logid +
                                   "|" + iv.start_evidence);
            }
        std::sort(evidence.begin(), evidence.end());
        const std::vector<std::string> cells = {
            site,
            row.logid,
            row.timestamp,
            row.source_id,
            row.target_account.empty() ? synthetic.at(row.target_username) : row.target_account,
            row.target_username,
            target_label_subtype(row.target_username),
            row.target_resolution,
            cid,
            creation(cid),
            creation(row.target_account),
            join_semicolon(evidence),
            row.target_resolution == "missing_historical_label_confirmation" ? "1" : "0"};
        if (publication_nodes) {
            const std::vector<std::string> unified = {
                "event",  cells[0], "",       "",        "",        "",       "",
                "",       "",       "",       "",        "",        "",       "",
                "",       "",       "",       "",        "",        "",       "",
                "",       cells[1], cells[2], cells[3],  cells[4],  cells[5], cells[6],
                cells[7], cells[8], cells[9], cells[10], cells[11], cells[12]};
            for (size_t i = 0; i < unified.size(); ++i) {
                if (i)
                    out.put(',');
                write_csv_cell(out, unified[i]);
            }
        } else {
            for (size_t i = 0; i < cells.size(); ++i) {
                if (i)
                    out.put(',');
                write_csv_cell(out, cells[i]);
            }
        }
        out.put('\n');
    }
    if (!out)
        die("failed writing target audit: " + path.string());
}

static void increment_synthetic_received_reason(PublicationNodeInfo &target,
                                                const std::string &resolution) {
    if (resolution == "missing_no_identity_evidence") {
        ++target.received_synthetic_missing_no_identity_evidence;
    } else if (resolution == "ambiguous_multiple_active_intervals") {
        ++target.received_synthetic_ambiguous_multiple_active_intervals;
    } else if (resolution == "missing_current_created_after_thank") {
        ++target.received_synthetic_missing_current_created_after_thank;
    } else if (resolution == "missing_interval_account_created_after_thank") {
        ++target.received_synthetic_missing_interval_account_created_after_thank;
    } else {
        ++target.received_synthetic_other_unresolved;
    }
}

static bool is_negative_integer_id(std::string_view s) {
    return s.size() > 1 && s[0] == '-' && is_digits(s.substr(1));
}

static bool negative_integer_id_less(std::string_view a, std::string_view b) {
    const std::string aa = trim_leading_zeros(a.substr(1));
    const std::string bb = trim_leading_zeros(b.substr(1));
    if (aa.size() != bb.size())
        return aa.size() > bb.size(); // larger absolute value is smaller as a negative integer
    if (aa != bb)
        return aa > bb;
    return a < b;
}

static std::vector<PublicationNodeInfo> build_publication_node_infos(
    const std::vector<ObservedEdge> &edges, const InternedStringMap &contributor_id_to_username,
    const std::unordered_map<std::string, AccountInfo> &accounts_by_id,
    const std::unordered_map<std::string, std::string> &synthetic_target_id_by_username,
    const std::unordered_map<std::string, std::string> &blocker_first_block_timestamp_by_id) {
    std::unordered_map<std::string, PublicationNodeInfo> by_id;
    by_id.reserve(edges.size() * 2 + synthetic_target_id_by_username.size() + 1);

    auto ensure_node = [&](const std::string &id) -> PublicationNodeInfo & {
        PublicationNodeInfo &n = by_id[id];
        if (n.id.empty())
            n.id = id;
        return n;
    };

    for (const ObservedEdge &row : edges) {
        if (!row.source_id.empty()) {
            PublicationNodeInfo &source = ensure_node(row.source_id);
            ++source.sent;
            if (row.target_account.empty())
                ++source.sent_to_unresolved_or_ambiguous_target;
            else
                ++source.sent_to_resolved_target;
        }

        if (!row.target_account.empty()) {
            PublicationNodeInfo &target = ensure_node(row.target_account);
            ++target.received;
            if (row.target_resolution == "interval_exact")
                ++target.received_interval_exact;
            else if (row.target_resolution == "interval_and_current_username_same")
                ++target.received_interval_and_current_username_same;
            else if (row.target_resolution == "current_username_no_interval_conflict")
                ++target.received_current_username_no_interval_conflict;
            else if (row.target_resolution == "current_username_no_creation_timestamp")
                ++target.received_current_username_no_creation_timestamp;
            else
                ++target.received_other_resolved;
        } else if (!row.target_username.empty()) {
            auto sit = synthetic_target_id_by_username.find(row.target_username);
            if (sit != synthetic_target_id_by_username.end()) {
                PublicationNodeInfo &target = ensure_node(sit->second);
                target.node_type = "synthetic_target_label";
                target.label = row.target_username;
                ++target.received;
                increment_synthetic_received_reason(target, row.target_resolution);
            }
        }
    }

    std::vector<std::string> evidence_labels;
    evidence_labels.reserve(by_id.size());

    for (auto &kv : by_id) {
        PublicationNodeInfo &n = kv.second;
        if (n.node_type == "account") {
            n.label =
                current_username_for_account(n.id, contributor_id_to_username, accounts_by_id);
            auto bit = blocker_first_block_timestamp_by_id.find(n.id);
            if (bit != blocker_first_block_timestamp_by_id.end()) {
                n.first_block_by_user_timestamp = bit->second;
            }
            auto ait = accounts_by_id.find(n.id);
            if (ait != accounts_by_id.end()) {
                n.creation_timestamp = ait->second.creation_timestamp;
                if (!ait->second.creation_evidence.empty()) {
                    const std::string codebook_label =
                        evidence_label_for_publication_codebook(ait->second.creation_evidence);
                    if (!codebook_label.empty())
                        evidence_labels.push_back(codebook_label);
                    if (!is_account_id_timestamp_order_invalidated_evidence(
                            ait->second.creation_evidence)) {
                        n.creation_evidence_label = codebook_label;
                    }
                }
            }
        }
    }

    std::sort(evidence_labels.begin(), evidence_labels.end());
    evidence_labels.erase(std::unique(evidence_labels.begin(), evidence_labels.end()),
                          evidence_labels.end());

    std::unordered_map<std::string, int> evidence_code_by_label;
    evidence_code_by_label.reserve(evidence_labels.size() * 2 + 1);
    for (size_t i = 0; i < evidence_labels.size(); ++i) {
        evidence_code_by_label[evidence_labels[i]] = static_cast<int>(i + 1);
    }

    std::vector<PublicationNodeInfo> nodes;
    nodes.reserve(by_id.size());
    for (auto &kv : by_id) {
        PublicationNodeInfo n = std::move(kv.second);
        if (!n.creation_evidence_label.empty()) {
            auto eit = evidence_code_by_label.find(n.creation_evidence_label);
            if (eit != evidence_code_by_label.end())
                n.creation_evidence_code = eit->second;
        }
        nodes.push_back(std::move(n));
    }

    std::sort(nodes.begin(), nodes.end(),
              [](const PublicationNodeInfo &a, const PublicationNodeInfo &b) {
                  const bool an = is_negative_integer_id(a.id);
                  const bool bn = is_negative_integer_id(b.id);
                  if (an != bn)
                      return !an; // real account nodes first, synthetic negative nodes last
                  if (an && bn)
                      return negative_integer_id_less(a.id, b.id);
                  return logid_less(a.id, b.id);
              });

    return nodes;
}

static void
validate_publication_first_block_by_user_timestamps(const std::vector<PublicationNodeInfo> &nodes,
                                                    Stats &stats,
                                                    std::vector<std::string> &failure_examples) {
    uint64_t account_nodes = 0;
    uint64_t account_nodes_with_timestamp = 0;
    uint64_t first_block_before_creation = 0;

    for (const PublicationNodeInfo &n : nodes) {
        if (n.node_type != "account")
            continue;
        ++account_nodes;
        if (n.first_block_by_user_timestamp.empty())
            continue;

        ++account_nodes_with_timestamp;
        if (!n.creation_timestamp.empty() &&
            n.first_block_by_user_timestamp < n.creation_timestamp) {
            ++first_block_before_creation;
            if (first_block_before_creation <= 10) {
                add_failure_example(failure_examples, "first_block_before_creation", n.id,
                                    n.first_block_by_user_timestamp, n.label,
                                    "creation_timestamp=" + n.creation_timestamp);
            }
        }
    }

    stats.publication_account_nodes = account_nodes;
    stats.publication_account_nodes_with_first_block_by_user_timestamp =
        account_nodes_with_timestamp;
    stats.publication_first_block_by_user_timestamp_none_fail = 0;
    stats.publication_first_block_by_user_timestamp_high_coverage_fail = 0;
    stats.publication_first_block_before_creation = first_block_before_creation;
    stats.publication_first_block_before_creation_fail = first_block_before_creation == 0 ? 0 : 1;

    if (account_nodes >= 300 && account_nodes_with_timestamp == 0) {
        stats.publication_first_block_by_user_timestamp_none_fail = 1;
        add_failure_example(failure_examples, "first_block_by_user_timestamp_none_fail", "", "", "",
                            "account_nodes=" + std::to_string(account_nodes) + ", defined=0");
    }
    if (account_nodes >= 300 && account_nodes_with_timestamp * 100u > account_nodes * 40u) {
        stats.publication_first_block_by_user_timestamp_high_coverage_fail = 1;
        add_failure_example(failure_examples, "first_block_by_user_timestamp_high_coverage_fail",
                            "", "", "",
                            "account_nodes=" + std::to_string(account_nodes) +
                                ", defined=" + std::to_string(account_nodes_with_timestamp));
    }
}

static void write_publication_edges_csv(
    const fs::path &path, const std::string &site, const std::vector<ObservedEdge> &rows,
    const std::unordered_map<std::string, std::string> &synthetic_target_id_by_username,
    Stats &stats) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        die("cannot open publication edges CSV for writing: " + path.string());

    write_csv_cell(out, "site");
    out.put(',');
    write_csv_cell(out, "logid");
    out.put(',');
    write_csv_cell(out, "timestamp");
    out.put(',');
    write_csv_cell(out, "source");
    out.put(',');
    write_csv_cell(out, "target");
    out.put('\n');

    for (const ObservedEdge &row : rows) {
        std::string target = row.target_account;
        if (target.empty() && !row.target_username.empty()) {
            auto sit = synthetic_target_id_by_username.find(row.target_username);
            if (sit != synthetic_target_id_by_username.end())
                target = sit->second;
        }

        write_csv_cell(out, site);
        out.put(',');
        write_csv_cell(out, row.logid);
        out.put(',');
        write_csv_cell(out, row.timestamp);
        out.put(',');
        write_csv_cell(out, row.source_id);
        out.put(',');
        write_csv_cell(out, target);
        out.put('\n');
        ++stats.resolved_edges_rows;
    }

    if (!out)
        die("failed writing publication edges CSV: " + path.string());
}

static void write_publication_nodes_csv(const fs::path &path, const std::string &site,
                                        const std::vector<PublicationNodeInfo> &nodes,
                                        Stats &stats) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        die("cannot open publication nodes CSV for writing: " + path.string());

    write_csv_cell(out, "site");
    out.put(',');
    write_csv_cell(out, "id");
    out.put(',');
    write_csv_cell(out, "node_type");
    out.put(',');
    write_csv_cell(out, "label");
    out.put(',');
    write_csv_cell(out, "creation_timestamp");
    out.put(',');
    write_csv_cell(out, "creation_evidence");
    out.put(',');
    write_csv_cell(out, "first_block_by_user_timestamp");
    out.put(',');
    write_csv_cell(out, "sent");
    out.put(',');
    write_csv_cell(out, "received");
    out.put('\n');

    for (const PublicationNodeInfo &n : nodes) {
        write_csv_cell(out, site);
        out.put(',');
        write_csv_cell(out, n.id);
        out.put(',');
        write_csv_cell(out, n.node_type);
        out.put(',');
        write_csv_cell(out, n.label);
        out.put(',');
        write_csv_cell(out, n.creation_timestamp);
        out.put(',');
        write_csv_cell(out, std::to_string(n.creation_evidence_code));
        out.put(',');
        write_csv_cell(out, n.first_block_by_user_timestamp);
        out.put(',');
        write_csv_cell(out, std::to_string(n.sent));
        out.put(',');
        write_csv_cell(out, std::to_string(n.received));
        out.put('\n');
        ++stats.accounts_rows;
    }

    if (!out)
        die("failed writing publication nodes CSV: " + path.string());
}

static bool thanks_logid_timestamp_order_violation_rate_exceeds_limit(const Stats &stats) {
    if (stats.thanks_logid_timestamp_order_violations == 0)
        return false;
    if (stats.retained_observed_edges == 0)
        return true;
    // Fail only above 0.01% = 1 / 10,000 of retained thank rows.
    return stats.thanks_logid_timestamp_order_violations * 10000u > stats.retained_observed_edges;
}

static bool validation_ok(const Stats &stats) {
    bool ok = true;
    if (stats.unmatched_logitem_at_eof)
        ok = false;
    if (stats.siteinfo_missing)
        ok = false;
    if (stats.siteinfo_parse_failures != 0)
        ok = false;
    if (stats.namespace_count == 0)
        ok = false;
    if (stats.parse_failures != 0)
        ok = false;
    if (stats.non_thanks_after_parse != 0)
        ok = false;
    if (stats.thanks_action_not_thank != 0)
        ok = false;
    if (stats.thanks_params_invalid != 0)
        ok = false;
    if (stats.timestamp_missing != 0)
        ok = false;
    if (stats.timestamp_invalid != 0)
        ok = false;
    if (stats.contributor_missing != 0)
        ok = false;
    if (stats.target_logtitle_missing != 0)
        ok = false;
    if (stats.target_logtitle_bad_prefix != 0)
        ok = false;
    if (stats.target_namespace_not_in_siteinfo != 0)
        ok = false;
    if (stats.target_namespace_inconsistent != 0)
        ok = false;
    if (stats.source_username_missing != 0)
        ok = false;
    if (stats.source_id_missing != 0)
        ok = false;
    if (stats.source_id_invalid || stats.identity_id_invalid || stats.logaction_id_invalid)
        ok = false;
    if (stats.source_username_id_conflict != 0)
        ok = false;
    if (stats.source_id_username_conflict != 0)
        ok = false;
    // Reused/usurped contributor usernames are expected in historical logging dumps.
    // They are counted and ambiguous names are removed from timeless direct lookup,
    // but they are not a fatal publication validation error.
    if (stats.global_contributor_id_username_conflict != 0)
        ok = false;
    if (stats.logaction_id_missing != 0)
        ok = false;
    if (stats.logaction_id_duplicate != 0)
        ok = false;
    if (thanks_logid_timestamp_order_violation_rate_exceeds_limit(stats))
        ok = false;
    if (stats.publication_first_block_by_user_timestamp_none_fail != 0)
        ok = false;
    if (stats.publication_first_block_by_user_timestamp_high_coverage_fail != 0)
        ok = false;
    if (stats.publication_first_block_before_creation_fail != 0)
        ok = false;
    if (stats.thanks_candidate_blocks + stats.thanks_candidate_blocks_at_or_after_input_date !=
        stats.raw_thanks_type_occurrences)
        ok = false;
    if (stats.retained_observed_edges + stats.excluded_deleted + stats.self_loop_edges_dropped +
            stats.thanks_candidate_blocks_at_or_after_input_date !=
        stats.raw_thanks_type_occurrences)
        ok = false;
    if (stats.observed_edges_rows != 0 &&
        stats.observed_edges_rows != stats.retained_observed_edges)
        ok = false;
    if (stats.resolved_edges_rows != 0 &&
        stats.resolved_edges_rows != stats.retained_observed_edges)
        ok = false;
    const uint64_t target_status_total = target_resolution_status_total(stats);
    if (target_status_total != 0 && target_status_total != stats.retained_observed_edges)
        ok = false;
    if (stats.excluded_deleted_rows_written != 0 &&
        stats.excluded_deleted_rows_written != stats.excluded_deleted)
        ok = false;
    if (stats.source_thanks_before_account_creation != 0)
        ok = false;
    if (stats.target_resolved_thanks_before_account_creation != 0)
        ok = false;
    return ok;
}

static void write_validation_summary_csv(const fs::path &path, const std::string &site,
                                         const fs::path &input, uint64_t input_size_bytes,
                                         const std::string &md5, const Stats &s, bool ok) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        die("cannot open validation summary CSV for writing: " + path.string());
    const std::vector<std::pair<std::string, std::string>> fields = {
        {"source_id_invalid", std::to_string(s.source_id_invalid)},
        {"newusers_zero_param_ignored", std::to_string(s.newusers_zero_param_ignored)},
        {"identity_id_invalid", std::to_string(s.identity_id_invalid)},
        {"logaction_id_invalid", std::to_string(s.logaction_id_invalid)},
        {"extractor_version", std::string(EXTRACT_THANKS_VERSION)},
        {"site", site},
        {"wiki", site + "wiki"},
        {"input_date", input_date_from_input_filename(input)},
        {"input_timestamp_cutoff", timestamp_cutoff_from_input_filename(input)},
        {"input_file", input.filename().string()},
        {"md5", md5},
        {"input_size_bytes", std::to_string(input_size_bytes)},
        {"logitems", std::to_string(s.logitem_blocks)},
        {"logitems_at_or_after_input_date",
         std::to_string(s.logitem_blocks_at_or_after_input_date)},
        {"raw_thanks", std::to_string(s.raw_thanks_type_occurrences)},
        {"raw_block", std::to_string(s.raw_block_type_occurrences)},
        {"thanks_candidate_blocks_at_or_after_input_date",
         std::to_string(s.thanks_candidate_blocks_at_or_after_input_date)},
        {"thanks_candidate_blocks", std::to_string(s.thanks_candidate_blocks)},
        {"retained_observed_edges", std::to_string(s.retained_observed_edges)},
        {"self_loop_edges_dropped", std::to_string(s.self_loop_edges_dropped)},
        {"block_candidate_blocks_at_or_after_input_date",
         std::to_string(s.block_candidate_blocks_at_or_after_input_date)},
        {"block_candidate_blocks", std::to_string(s.block_candidate_blocks)},
        {"block_action_block", std::to_string(s.block_action_block)},
        {"block_action_not_block", std::to_string(s.block_action_not_block)},
        {"block_parse_failures", std::to_string(s.block_parse_failures)},
        {"block_timestamp_missing", std::to_string(s.block_timestamp_missing)},
        {"block_timestamp_invalid", std::to_string(s.block_timestamp_invalid)},
        {"block_contributor_deleted_or_missing",
         std::to_string(s.block_contributor_deleted_or_missing)},
        {"block_source_id_missing", std::to_string(s.block_source_id_missing)},
        {"blocker_first_timestamps_recorded", std::to_string(s.blocker_first_timestamps_recorded)},
        {"publication_account_nodes", std::to_string(s.publication_account_nodes)},
        {"publication_account_nodes_with_first_block_by_user_timestamp",
         std::to_string(s.publication_account_nodes_with_first_block_by_user_timestamp)},
        {"publication_first_block_by_user_timestamp_none_fail",
         std::to_string(s.publication_first_block_by_user_timestamp_none_fail)},
        {"publication_first_block_by_user_timestamp_high_coverage_fail",
         std::to_string(s.publication_first_block_by_user_timestamp_high_coverage_fail)},
        {"publication_first_block_before_creation",
         std::to_string(s.publication_first_block_before_creation)},
        {"publication_first_block_before_creation_fail",
         std::to_string(s.publication_first_block_before_creation_fail)},
        {"excluded_deleted", std::to_string(s.excluded_deleted)},
        {"observed_edges_rows", std::to_string(s.observed_edges_rows)},
        {"resolved_edges_rows", std::to_string(s.resolved_edges_rows)},
        {"accounts_rows", std::to_string(s.accounts_rows)},
        {"rename_events_rows", std::to_string(s.rename_events_rows)},
        {"excluded_deleted_rows_written", std::to_string(s.excluded_deleted_rows_written)},
        {"excluded_timestamp_deleted", std::to_string(s.excluded_timestamp_deleted)},
        {"excluded_contributor_deleted", std::to_string(s.excluded_contributor_deleted)},
        {"excluded_logtitle_deleted", std::to_string(s.excluded_logtitle_deleted)},
        {"parse_failures", std::to_string(s.parse_failures)},
        {"non_thanks_after_parse", std::to_string(s.non_thanks_after_parse)},
        {"thanks_action_not_thank", std::to_string(s.thanks_action_not_thank)},
        {"thanks_params_invalid", std::to_string(s.thanks_params_invalid)},
        {"timestamp_missing", std::to_string(s.timestamp_missing)},
        {"timestamp_invalid", std::to_string(s.timestamp_invalid)},
        {"contributor_missing", std::to_string(s.contributor_missing)},
        {"target_logtitle_missing", std::to_string(s.target_logtitle_missing)},
        {"target_logtitle_bad_prefix", std::to_string(s.target_logtitle_bad_prefix)},
        {"target_namespace_not_in_siteinfo", std::to_string(s.target_namespace_not_in_siteinfo)},
        {"target_namespace_inconsistent", std::to_string(s.target_namespace_inconsistent)},
        {"source_username_missing", std::to_string(s.source_username_missing)},
        {"source_id_missing", std::to_string(s.source_id_missing)},
        {"source_username_id_conflict", std::to_string(s.source_username_id_conflict)},
        {"source_id_username_conflict", std::to_string(s.source_id_username_conflict)},
        {"global_contributor_identities", std::to_string(s.global_contributor_identities)},
        {"global_contributor_username_id_conflict",
         std::to_string(s.global_contributor_username_id_conflict)},
        {"global_contributor_id_username_conflict",
         std::to_string(s.global_contributor_id_username_conflict)},
        {"logaction_id_missing", std::to_string(s.logaction_id_missing)},
        {"logaction_id_duplicate", std::to_string(s.logaction_id_duplicate)},
        {"siteinfo_missing", bool_text(s.siteinfo_missing)},
        {"siteinfo_parse_failures", std::to_string(s.siteinfo_parse_failures)},
        {"namespace_count", std::to_string(s.namespace_count)},
        {"newusers_candidate_blocks", std::to_string(s.newusers_candidate_blocks)},
        {"newusers_total", std::to_string(s.newusers_total)},
        {"newusers_action_create", std::to_string(s.newusers_action_create)},
        {"newusers_action_autocreate", std::to_string(s.newusers_action_autocreate)},
        {"newusers_action_newusers", std::to_string(s.newusers_action_newusers)},
        {"newusers_action_create2", std::to_string(s.newusers_action_create2)},
        {"newusers_action_byemail", std::to_string(s.newusers_action_byemail)},
        {"newusers_action_forcecreatelocal", std::to_string(s.newusers_action_forcecreatelocal)},
        {"newusers_action_other", std::to_string(s.newusers_action_other)},
        {"newusers_parse_failures", std::to_string(s.newusers_parse_failures)},
        {"newusers_params_userid_matches_contributor_id",
         std::to_string(s.newusers_params_userid_matches_contributor_id)},
        {"newusers_params_userid_differs_from_contributor_id",
         std::to_string(s.newusers_params_userid_differs_from_contributor_id)},
        {"account_creation_by_contributor", std::to_string(s.account_creation_by_contributor)},
        {"account_creation_by_params_userid", std::to_string(s.account_creation_by_params_userid)},
        {"account_creation_by_logtitle_only", std::to_string(s.account_creation_by_logtitle_only)},
        {"account_creation_by_logtitle_only_current_username",
         std::to_string(s.account_creation_by_logtitle_only_current_username)},
        {"account_creation_logtitle_only_ignored_not_current",
         std::to_string(s.account_creation_logtitle_only_ignored_not_current)},
        {"account_creation_logtitle_only_ignored_after_observed_activity",
         std::to_string(s.account_creation_logtitle_only_ignored_after_observed_activity)},
        {"account_creation_duplicate_same", std::to_string(s.account_creation_duplicate_same)},
        {"account_creation_duplicate_earlier",
         std::to_string(s.account_creation_duplicate_earlier)},
        {"account_creation_conflict_username",
         std::to_string(s.account_creation_conflict_username)},
        {"account_creation_conflict_userid", std::to_string(s.account_creation_conflict_userid)},
        {"account_creation_username_differs_from_current",
         std::to_string(s.account_creation_username_differs_from_current)},
        {"account_creation_ignored_after_observed_activity",
         std::to_string(s.account_creation_ignored_after_observed_activity)},
        {"account_creation_aliases_recorded", std::to_string(s.account_creation_aliases_recorded)},
        {"account_creation_alias_duplicate_same",
         std::to_string(s.account_creation_alias_duplicate_same)},
        {"account_creation_alias_intervals_built",
         std::to_string(s.account_creation_alias_intervals_built)},
        {"account_creation_alias_intervals_closed_by_rename",
         std::to_string(s.account_creation_alias_intervals_closed_by_rename)},
        {"account_creation_alias_interval_overlaps",
         std::to_string(s.account_creation_alias_interval_overlaps)},
        {"account_creation_alias_interval_rename_old_not_active",
         std::to_string(s.account_creation_alias_interval_rename_old_not_active)},
        {"account_creation_alias_interval_rename_old_ambiguous",
         std::to_string(s.account_creation_alias_interval_rename_old_ambiguous)},
        {"account_creation_alias_interval_ignored_no_account_creation",
         std::to_string(s.account_creation_alias_interval_ignored_no_account_creation)},
        {"newusers_timestamp_missing", std::to_string(s.newusers_timestamp_missing)},
        {"newusers_timestamp_invalid", std::to_string(s.newusers_timestamp_invalid)},
        {"newusers_username_missing", std::to_string(s.newusers_username_missing)},
        {"newusers_userid_missing", std::to_string(s.newusers_userid_missing)},
        {"newusers_missing_userid_pending_username",
         std::to_string(s.newusers_missing_userid_pending_username)},
        {"newusers_missing_userid_recovered_from_username",
         std::to_string(s.newusers_missing_userid_recovered_from_username)},
        {"newusers_missing_userid_ignored_username_not_current",
         std::to_string(s.newusers_missing_userid_ignored_username_not_current)},
        {"newusers_missing_userid_ignored_after_observed_activity",
         std::to_string(s.newusers_missing_userid_ignored_after_observed_activity)},
        {"newusers_logtitle_missing", std::to_string(s.newusers_logtitle_missing)},
        {"newusers_logtitle_bad_prefix", std::to_string(s.newusers_logtitle_bad_prefix)},
        {"newusers_logtitle_not_user_namespace_or_bad_prefix",
         std::to_string(s.newusers_logtitle_bad_prefix)},
        {"newusers_params_userid_missing", std::to_string(s.newusers_params_userid_missing)},
        {"newusers_missing_username_pending_logtitle",
         std::to_string(s.newusers_missing_username_pending_logtitle)},
        {"newusers_missing_username_recovered_from_logtitle",
         std::to_string(s.newusers_missing_username_recovered_from_logtitle)},
        {"newusers_missing_username_ignored_id_username_mismatch",
         std::to_string(s.newusers_missing_username_ignored_id_username_mismatch)},
        {"newusers_missing_username_ignored_id_not_current",
         std::to_string(s.newusers_missing_username_ignored_id_not_current)},
        {"newusers_missing_username_ignored_after_observed_activity",
         std::to_string(s.newusers_missing_username_ignored_after_observed_activity)},
        {"renameuser_candidate_blocks", std::to_string(s.renameuser_candidate_blocks)},
        {"renameuser_total", std::to_string(s.renameuser_total)},
        {"renameuser_parsed", std::to_string(s.renameuser_parsed)},
        {"renameuser_unparsed", std::to_string(s.renameuser_unparsed)},
        {"renameuser_action_not_renameuser", std::to_string(s.renameuser_action_not_renameuser)},
        {"renameuser_timestamp_invalid", std::to_string(s.renameuser_timestamp_invalid)},
        {"source_thanks_before_account_creation",
         std::to_string(s.source_thanks_before_account_creation)},
        {"target_resolved_thanks_before_account_creation",
         std::to_string(s.target_resolved_thanks_before_account_creation)},
        {"account_creation_timestamp_id_order_invalidated",
         std::to_string(s.account_creation_timestamp_id_order_invalidated)},
        {"thanks_logid_timestamp_order_violations",
         std::to_string(s.thanks_logid_timestamp_order_violations)},
        {"thanks_logid_timestamp_order_violation_fail_threshold_percent", "0.01"},
        {"thanks_logid_timestamp_order_violation_rate_exceeds_limit",
         bool_text(thanks_logid_timestamp_order_violation_rate_exceeds_limit(s))},
        {"target_resolution_direct_current", std::to_string(s.target_resolution_direct_current)},
        {"target_resolution_direct_current_no_creation",
         std::to_string(s.target_resolution_direct_current_no_creation)},
        {"target_resolution_rename_chain", std::to_string(s.target_resolution_rename_chain)},
        {"target_resolution_direct_and_chain_same",
         std::to_string(s.target_resolution_direct_and_chain_same)},
        {"target_resolution_creation_alias", std::to_string(s.target_resolution_creation_alias)},
        {"target_resolution_direct_and_creation_alias_same",
         std::to_string(s.target_resolution_direct_and_creation_alias_same)},
        {"target_resolution_ambiguous", std::to_string(s.target_resolution_ambiguous)},
        {"target_resolution_ambiguous_creation_alias",
         std::to_string(s.target_resolution_ambiguous_creation_alias)},
        {"target_resolution_missing_created_after_thank",
         std::to_string(s.target_resolution_missing_created_after_thank)},
        {"target_resolution_missing_no_current",
         std::to_string(s.target_resolution_missing_no_current)},
        {"target_resolution_missing_unresolved_rename_chain",
         std::to_string(s.target_resolution_missing_unresolved_rename_chain)},
        {"target_resolution_ambiguous_multiple_active_intervals",
         std::to_string(s.target_resolution_ambiguous_creation_alias)},
        {"target_resolution_ambiguous_total", std::to_string(target_resolution_ambiguous_total(s))},
        {"target_resolution_unresolved_or_ambiguous_total",
         std::to_string(target_resolution_unresolved_or_ambiguous_total(s))},
        {"target_resolution_status_total", std::to_string(target_resolution_status_total(s))},
        {"unmatched_logitem_at_eof", bool_text(s.unmatched_logitem_at_eof)},
        {"validation_ok", bool_text(ok)}};
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i)
            out.put(',');
        write_csv_cell(out, fields[i].first);
    }
    out.put('\n');
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i)
            out.put(',');
        write_csv_cell(out, fields[i].second);
    }
    out.put('\n');
    if (!out)
        die("failed writing validation summary CSV: " + path.string());
}

static void process_available_blocks(
    std::string &buffer, size_t &scan, bool &siteinfo_parsed, NamespaceInfo &namespaces,
    std::string &file_target_namespace, std::unordered_set<std::string> &seen_logaction_ids,
    InternedStringMap &contributor_username_to_id, InternedStringMap &contributor_id_to_username,
    InternedStringSet &ambiguous_contributor_usernames,
    InternedTimestampMap &first_observed_contributor_timestamp_by_id,
    std::unordered_map<std::string, std::string> &blocker_first_block_timestamp_by_id,
    StringInterner &contributor_string_interner,
    std::unordered_map<std::string, AccountInfo> &accounts_by_id,
    std::unordered_map<std::string, UsernameOnlyCreation> &username_only_creation,
    std::unordered_map<std::string, std::vector<AccountCreationAlias>>
        &account_creation_aliases_by_username,
    std::vector<PendingIdLogtitleCreation> &pending_id_logtitle_creations,
    std::vector<PendingUsernameCreation> &pending_username_creations,
    std::vector<ObservedEdge> &edges, std::vector<DeletedExclusion> &exclusions,
    std::vector<RenameEvent> &rename_events, Stats &stats,
    std::vector<std::string> &failure_examples, const std::string &timestamp_cutoff) {
    auto compact_prefix = [&](size_t len) {
        if (len > 0) {
            buffer.erase(0, len);
            scan = 0;
        }
    };
    while (!siteinfo_parsed) {
        size_t site_start = buffer.find(SITEINFO_OPEN_TAG);
        if (site_start == std::string::npos) {
            size_t logitem_start = buffer.find(OPEN_TAG);
            if (logitem_start != std::string::npos) {
                stats.siteinfo_missing = true;
                return;
            }
            const size_t keep = std::max(SITEINFO_OPEN_TAG.size(), OPEN_TAG.size()) - 1;
            if (buffer.size() > keep)
                buffer.erase(0, buffer.size() - keep);
            scan = 0;
            return;
        }
        size_t site_close = buffer.find(SITEINFO_CLOSE_TAG, site_start);
        if (site_close == std::string::npos) {
            if (site_start > 0)
                compact_prefix(site_start);
            return;
        }
        size_t site_end = site_close + SITEINFO_CLOSE_TAG.size();
        std::string_view siteinfo_block(buffer.data() + site_start, site_end - site_start);
        namespaces = parse_siteinfo_block(siteinfo_block, stats);
        stats.namespace_count = namespaces.all.size();
        if (namespaces.all.empty() || namespaces.user.empty())
            stats.siteinfo_missing = true;
        siteinfo_parsed = true;
        compact_prefix(site_end);
    }
    while (true) {
        size_t start = buffer.find(OPEN_TAG, scan);
        if (start == std::string::npos) {
            const size_t keep = OPEN_TAG.size() - 1;
            if (buffer.size() > keep)
                buffer.erase(0, buffer.size() - keep);
            scan = 0;
            return;
        }
        size_t close = buffer.find(CLOSE_TAG, start);
        if (close == std::string::npos) {
            if (start > 0)
                compact_prefix(start);
            return;
        }
        size_t end = close + CLOSE_TAG.size();
        std::string_view block(buffer.data() + start, end - start);
        ++stats.logitem_blocks;

        std::string block_timestamp;
        extract_first_element_text(block, "timestamp", block_timestamp);
        if (!timestamp_cutoff.empty() && valid_account_creation_timestamp(block_timestamp) &&
            block_timestamp >= timestamp_cutoff) {
            ++stats.logitem_blocks_at_or_after_input_date;
            if (block.find(THANKS_NEEDLE) != std::string_view::npos) {
                ++stats.thanks_candidate_blocks_at_or_after_input_date;
            }
            if (block.find(BLOCK_NEEDLE) != std::string_view::npos) {
                ++stats.block_candidate_blocks_at_or_after_input_date;
            }
            scan = end;
            if (scan > 64u * 1024u * 1024u || scan > buffer.size() / 2)
                compact_prefix(scan);
            continue;
        }

        parse_global_contributor_block_fast(
            block, contributor_username_to_id, contributor_id_to_username,
            ambiguous_contributor_usernames, first_observed_contributor_timestamp_by_id,
            contributor_string_interner, stats, failure_examples);

        if (block.find(BLOCK_NEEDLE) != std::string_view::npos) {
            parse_block_action_block(block, blocker_first_block_timestamp_by_id, stats,
                                     failure_examples);
        }

        if (block.find(THANKS_NEEDLE) != std::string_view::npos) {
            ++stats.thanks_candidate_blocks;
            ParsedThanks parsed = parse_thanks_block(block, namespaces.user, file_target_namespace,
                                                     seen_logaction_ids, stats, failure_examples);
            if (parsed.excluded_deleted)
                exclusions.push_back(std::move(parsed.exclusion));
            else if (parsed.valid) {
                edges.push_back(std::move(parsed.edge));
                ++stats.retained_observed_edges;
            }
        }
        if (block.find(NEWUSERS_NEEDLE) != std::string_view::npos) {
            parse_newusers_block(block, namespaces.user, accounts_by_id, username_only_creation,
                                 account_creation_aliases_by_username,
                                 pending_id_logtitle_creations, pending_username_creations, stats,
                                 failure_examples);
        }
        if (block.find(RENAMEUSER_NEEDLE) != std::string_view::npos) {
            parse_renameuser_block(block, namespaces.user, rename_events, stats);
        }
        scan = end;
        if (scan > 64u * 1024u * 1024u || scan > buffer.size() / 2)
            compact_prefix(scan);
    }
}

static Stats process_file(const fs::path &input, const Options &opt) {
    Stats stats;
    const std::string site = site_from_input_filename(input);
    const std::string timestamp_cutoff = timestamp_cutoff_from_input_filename(input);
    OutputPaths paths = output_paths_for(input, opt);
    fs::create_directories(paths.observed_edges.parent_path().empty()
                               ? fs::path(".")
                               : paths.observed_edges.parent_path());
    ensure_outputs_do_not_exist(paths, opt);

    const uint64_t input_size_bytes = fs::file_size(input);
    const std::string md5 = md5sum_file(input);

    gzFile gz = gzopen(input.string().c_str(), "rb");
    if (!gz)
        die("cannot open input file through zlib: " + input.string());
    gzbuffer(gz, static_cast<unsigned>(std::min<size_t>(opt.buffer_bytes, 128u * 1024u * 1024u)));

    std::vector<ObservedEdge> edges;
    std::vector<DeletedExclusion> exclusions;
    std::vector<RenameEvent> rename_events;
    std::vector<PendingIdLogtitleCreation> pending_id_logtitle_creations;
    std::vector<PendingUsernameCreation> pending_username_creations;
    edges.reserve(1024);
    exclusions.reserve(16);
    rename_events.reserve(1024);
    pending_id_logtitle_creations.reserve(1024);
    pending_username_creations.reserve(1024);

    std::vector<char> chunk(opt.buffer_bytes);
    std::string buffer;
    buffer.reserve(opt.buffer_bytes * 2);
    size_t scan = 0;
    StreamNeedleCounter thanks_stream_counter(THANKS_NEEDLE);
    StreamNeedleCounter block_stream_counter(BLOCK_NEEDLE);
    bool siteinfo_parsed = false;
    NamespaceInfo namespaces;
    std::string file_target_namespace;
    std::unordered_set<std::string> seen_logaction_ids;
    StringInterner contributor_string_interner;
    InternedStringMap contributor_username_to_id;
    InternedStringMap contributor_id_to_username;
    InternedStringSet ambiguous_contributor_usernames;
    InternedTimestampMap first_observed_contributor_timestamp_by_id;
    std::unordered_map<std::string, std::string> blocker_first_block_timestamp_by_id;
    std::unordered_map<std::string, AccountInfo> accounts_by_id;
    std::unordered_map<std::string, UsernameOnlyCreation> username_only_creation;
    std::unordered_map<std::string, std::vector<AccountCreationAlias>>
        account_creation_aliases_by_username;
    std::vector<std::string> failure_examples;
    contributor_string_interner.reserve(2u * 1024u * 1024u);
    contributor_username_to_id.reserve(1024 * 1024);
    contributor_id_to_username.reserve(1024 * 1024);
    ambiguous_contributor_usernames.reserve(1024);
    first_observed_contributor_timestamp_by_id.reserve(1024 * 1024);
    blocker_first_block_timestamp_by_id.reserve(1024 * 64);
    accounts_by_id.reserve(1024 * 1024);
    username_only_creation.reserve(1024 * 1024);
    account_creation_aliases_by_username.reserve(1024 * 1024);
    seen_logaction_ids.reserve(1024 * 1024);

    while (true) {
        int nread = gzread(gz, chunk.data(), static_cast<unsigned int>(chunk.size()));
        if (nread < 0) {
            int errnum = 0;
            const char *msg = gzerror(gz, &errnum);
            gzclose(gz);
            die("gzip/zlib read error in " + input.string() + ": " + (msg ? msg : "unknown error"));
        }
        if (nread == 0)
            break;
        thanks_stream_counter.add(chunk.data(), static_cast<size_t>(nread));
        block_stream_counter.add(chunk.data(), static_cast<size_t>(nread));
        buffer.append(chunk.data(), static_cast<size_t>(nread));
        process_available_blocks(
            buffer, scan, siteinfo_parsed, namespaces, file_target_namespace, seen_logaction_ids,
            contributor_username_to_id, contributor_id_to_username, ambiguous_contributor_usernames,
            first_observed_contributor_timestamp_by_id, blocker_first_block_timestamp_by_id,
            contributor_string_interner, accounts_by_id, username_only_creation,
            account_creation_aliases_by_username, pending_id_logtitle_creations,
            pending_username_creations, edges, exclusions, rename_events, stats, failure_examples,
            timestamp_cutoff);
    }
    if (!siteinfo_parsed)
        stats.siteinfo_missing = true;
    stats.raw_thanks_type_occurrences = thanks_stream_counter.count();
    stats.raw_block_type_occurrences = block_stream_counter.count();
    if (buffer.find(OPEN_TAG) != std::string::npos)
        stats.unmatched_logitem_at_eof = true;
    int zrc = gzclose(gz);
    if (zrc != Z_OK)
        die("gzip/zlib close error for: " + input.string());

    apply_pending_id_logtitle_creations(
        pending_id_logtitle_creations, contributor_username_to_id, contributor_id_to_username,
        first_observed_contributor_timestamp_by_id, accounts_by_id, username_only_creation,
        account_creation_aliases_by_username, stats, failure_examples);
    apply_pending_username_creations_to_current_ids(
        pending_username_creations, contributor_username_to_id,
        first_observed_contributor_timestamp_by_id, accounts_by_id, username_only_creation, stats,
        failure_examples);
    // First discard false late ID-level candidates.  This must happen before applying
    // weaker username-only create2 evidence; otherwise a valid earlier create2 timestamp
    // can be skipped and then the late ID-level candidate gets cleared, leaving a blank.
    discard_account_creations_after_observed_activity(
        accounts_by_id, first_observed_contributor_timestamp_by_id, stats);
    apply_username_only_creations_to_current_accounts(
        username_only_creation, contributor_username_to_id,
        first_observed_contributor_timestamp_by_id, accounts_by_id,
        account_creation_aliases_by_username, stats);
    // Discard any username-only recovery that is later than already observed activity.
    discard_account_creations_after_observed_activity(
        accounts_by_id, first_observed_contributor_timestamp_by_id, stats);
    invalidate_account_creation_timestamps_disagreeing_with_ids(accounts_by_id, stats,
                                                                failure_examples);
    validate_thanks_logid_timestamp_order(edges, stats, failure_examples);
    // These two structures are needed only while parsing and applying creation
    // evidence.  Releasing them before target resolution significantly lowers
    // peak RSS on large wikis without changing any output decisions.
    release_container_memory(first_observed_contributor_timestamp_by_id);
    release_container_memory(seen_logaction_ids);

    bool ok = validation_ok(stats);
    if (ok) {
        sort_edges(edges);
        sort_rename_events(rename_events);
        const bool publication_mode = !(VERBOSE_CAPABLE && opt.verbose);
        if (publication_mode) {
            release_container_memory(username_only_creation);
            release_container_memory(pending_id_logtitle_creations);
            release_container_memory(pending_username_creations);
            release_container_memory(exclusions);
        }
        auto username_intervals_by_username = build_account_creation_alias_intervals(
            account_creation_aliases_by_username, accounts_by_id, rename_events, stats, false,
            true);
        resolve_targets(edges, contributor_username_to_id, accounts_by_id,
                        username_intervals_by_username, stats);
        drop_resolved_self_loops(edges, stats);
        validate_source_temporal_consistency(edges, accounts_by_id, stats, failure_examples);

        ok = validation_ok(stats);
        if (ok) {
            if (VERBOSE_CAPABLE && opt.verbose) {
                write_observed_edges_csv(paths.observed_edges, site, edges, stats);
                write_resolved_edges_csv(paths.resolved_edges, site, edges, stats);
                write_accounts_csv(paths.accounts, site, contributor_id_to_username, accounts_by_id,
                                   stats);
                write_rename_events_csv(paths.rename_events, site, rename_events, stats);
                write_excluded_deleted_csv(paths.excluded_deleted, site, exclusions, stats);
                write_username_intervals_csv(paths.username_intervals, site,
                                             username_intervals_by_username);
                write_account_creation_candidates_csv(paths.account_creation_candidates, site,
                                                      accounts_by_id, username_only_creation,
                                                      account_creation_aliases_by_username);
                write_rename_resolution_csv(paths.rename_resolution, site, rename_events);
                write_target_resolution_audit_csv(paths.target_resolution_audit, site, edges,
                                                  contributor_username_to_id, accounts_by_id,
                                                  username_intervals_by_username);
            } else {
                auto synthetic_target_id_by_username = build_synthetic_target_ids(edges);
                std::vector<PublicationNodeInfo> publication_nodes = build_publication_node_infos(
                    edges, contributor_id_to_username, accounts_by_id,
                    synthetic_target_id_by_username, blocker_first_block_timestamp_by_id);
                validate_publication_first_block_by_user_timestamps(publication_nodes, stats,
                                                                    failure_examples);
                ok = validation_ok(stats);
                if (!ok) {
                    // Do not write publication CSVs when the node-level blocker-timestamp sanity checks fail.
                } else {
                    release_container_memory(contributor_id_to_username);

                    write_publication_edges_csv(paths.network_edges, site, edges,
                                                synthetic_target_id_by_username, stats);
                    write_publication_nodes_csv(paths.network_nodes, site, publication_nodes,
                                                stats);
                    write_target_resolution_audit_csv(
                        paths.target_resolution_audit, site, edges, contributor_username_to_id,
                        accounts_by_id, username_intervals_by_username, &publication_nodes);
                }
            }
            ok = validation_ok(stats);
        }
    }
    if (VERBOSE_CAPABLE && opt.verbose) {
        write_validation_summary_csv(paths.validation_summary, site, input, input_size_bytes, md5,
                                     stats, ok);
    }

    std::cerr
        << (ok ? "OK" : "FAIL") << " " << input << " | version=" << EXTRACT_THANKS_VERSION
        << " newusers_zero_param_ignored=" << stats.newusers_zero_param_ignored
        << " output_mode=" << (opt.verbose ? "verbose" : "publication") << " site=" << site
        << " md5=" << md5 << " target_namespace="
        << (file_target_namespace.empty() ? "<none>" : file_target_namespace)
        << " namespaces=" << stats.namespace_count << " input_timestamp_cutoff=" << timestamp_cutoff
        << " raw_<type>thanks</type>=" << stats.raw_thanks_type_occurrences
        << " thanks_at_or_after_input_date=" << stats.thanks_candidate_blocks_at_or_after_input_date
        << " retained_observed_edges=" << stats.retained_observed_edges
        << " self_loop_edges_dropped=" << stats.self_loop_edges_dropped
        << " block_action_block=" << stats.block_action_block
        << " publication_account_nodes=" << stats.publication_account_nodes
        << " account_nodes_with_first_block_by_user_timestamp="
        << stats.publication_account_nodes_with_first_block_by_user_timestamp
        << " first_block_by_user_timestamp_none_fail="
        << stats.publication_first_block_by_user_timestamp_none_fail
        << " first_block_by_user_timestamp_high_coverage_fail="
        << stats.publication_first_block_by_user_timestamp_high_coverage_fail
        << " first_block_before_creation=" << stats.publication_first_block_before_creation
        << " first_block_before_creation_fail="
        << stats.publication_first_block_before_creation_fail
        << " excluded_deleted=" << stats.excluded_deleted
        << " target_resolution_ambiguous_total=" << target_resolution_ambiguous_total(stats)
        << " target_resolution_missing_no_current=" << stats.target_resolution_missing_no_current
        << " source_thanks_before_account_creation=" << stats.source_thanks_before_account_creation
        << " target_resolved_thanks_before_account_creation="
        << stats.target_resolved_thanks_before_account_creation
        << " account_creation_timestamp_id_order_invalidated="
        << stats.account_creation_timestamp_id_order_invalidated
        << " thanks_logid_timestamp_order_violations="
        << stats.thanks_logid_timestamp_order_violations
        << " thanks_logid_timestamp_order_violation_rate_exceeds_limit="
        << bool_yesno(thanks_logid_timestamp_order_violation_rate_exceeds_limit(stats))
        << " global_contributor_username_id_conflict="
        << stats.global_contributor_username_id_conflict
        << " global_contributor_id_username_conflict="
        << stats.global_contributor_id_username_conflict
        << " parse_failures=" << stats.parse_failures
        << " unmatched_logitem_at_eof=" << bool_yesno(stats.unmatched_logitem_at_eof);

    if (VERBOSE_CAPABLE && opt.verbose) {
        std::cerr << " observed_edges=" << paths.observed_edges
                  << " resolved_edges=" << paths.resolved_edges << " accounts=" << paths.accounts
                  << " rename_events=" << paths.rename_events
                  << " excluded_deleted=" << paths.excluded_deleted
                  << " username_intervals=" << paths.username_intervals
                  << " account_creation_candidates=" << paths.account_creation_candidates
                  << " rename_resolution=" << paths.rename_resolution
                  << " target_resolution_audit=" << paths.target_resolution_audit
                  << " validation_summary=" << paths.validation_summary;
    } else {
        std::cerr << " edges=" << paths.network_edges << " nodes=" << paths.network_nodes
                  << " audit=" << paths.target_resolution_audit;
    }

    std::cerr << "\n";
    if (!ok && !failure_examples.empty()) {
        std::cerr << "First validation examples for " << input << ":\n";
        for (const std::string &ex : failure_examples)
            std::cerr << "  - " << ex << "\n";
    }
    if (!ok)
        die("validation failed for " + input.string());
    return stats;
}

static void usage(const char *argv0) {
    std::cerr
        << "Usage: " << argv0 << " [--out-dir DIR] [--buffer-mb N] [--force]"
        << (VERBOSE_CAPABLE ? " [--verbose|--debug]" : "") << " file1.xml.gz [file2.xml.gz ...]\n"
        << "\n"
        << "Input filenames must match:\n"
        << "  ^([a-z0-9_]{2,20})wiki-(20[0-9][0-9][01][0-9][0-3][0-9])-pages-logging\\.xml(\\.gz)?$\n"
        << "\n"
        << "Default publication mode writes three compact network-science files per input,\n"
        << "inside OUT_DIR/YYYYMMDD/ or the input directory/YYYYMMDD/:\n"
        << "  SITEwiki.thanks.edges.csv\n"
        << "  SITEwiki.thanks.nodes.csv\n"
        << "  SITEwiki.thanks.target_resolution_audit.csv\n"
        << "\n"
        << "Default schemas:\n"
        << "  edges.csv: site,logid,timestamp,source,target\n"
        << "    source is always a real account ID. target is a real account ID when\n"
        << "    resolved, otherwise a deterministic negative synthetic node ID for the\n"
        << "    observed unresolved target username.\n"
        << "  nodes.csv: site,id,node_type,label,creation_timestamp,creation_evidence,first_block_by_user_timestamp,sent,received\n"
        << "    node_type is account for real MediaWiki accounts and synthetic_target_label\n"
        << "    for negative unresolved-name nodes. creation_evidence is an integer code;\n"
        << "    0 means no accepted creation evidence.\n"
        << "  target_resolution_audit.csv: node-summary and event records; node summaries with the creation_evidence codebook label,\n"
        << "    missing-creation flag, sent-to-unresolved counts, received-edge\n"
        << "    resolution-support counts, and synthetic-node unresolved-reason counts.\n"
        << "\n"
        << (VERBOSE_CAPABLE
                ? "Verbose/debug mode (--verbose or --debug) writes the full audit set instead:\n"
                : "This binary was compiled publication-only (EXTRACT_THANKS_VERBOSE_CAPABLE=0); verbose/debug output is unavailable.\n")
        << "  observed_edges.csv, resolved_edges.csv, accounts.csv, rename_events.csv,\n"
        << "  excluded_deleted.csv, username_intervals.csv, account_creation_candidates.csv,\n"
        << "  rename_resolution.csv, target_resolution_audit.csv, validation_summary.csv.\n"
        << "\n"
        << "Method summary:\n"
        << "  A trivial CSV of contributor.username,timestamp,logtitle is only a raw string\n"
        << "  table. This extractor builds an account-level directed network by combining\n"
        << "  contributor.id source accounts, action-aware newusers parsing, accepted account\n"
        << "  creation timestamps, User-namespace creation aliases, and time-bounded username\n"
        << "  intervals closed/opened by renameuser events. In default mode, unresolved\n"
        << "  target usernames become negative synthetic nodes for direct graph use; in\n"
        << "  verbose/debug mode they remain blank with exact audit statuses.\n"
        << "\n"
        << "Hard validation checks include thanks-count conservation, duplicate logids,\n"
        << "  relevant parse failures, namespace consistency, contributor username/id\n"
        << "  conflicts, thank logid/timestamp order above 0.01%, target-resolution\n"
        << "  accounting, and source/target events before accepted account creation. Creation\n"
        << "  timestamps that contradict account-id order are invalidated before output.\n";
}

static Options parse_args(int argc, char **argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--out-dir") {
            if (++i >= argc)
                die("missing DIR after --out-dir");
            opt.out_dir = argv[i];
            opt.use_out_dir = true;
        } else if (a == "--buffer-mb") {
            if (++i >= argc)
                die("missing N after --buffer-mb");
            char *end = nullptr;
            errno = 0;
            unsigned long mb = std::strtoul(argv[i], &end, 10);
            if (errno || !end || *end || mb == 0 || mb > 1024)
                die("invalid --buffer-mb value");
            opt.buffer_bytes = static_cast<size_t>(mb) * 1024u * 1024u;
        } else if (a == "--force") {
            opt.force = true;
        } else if (a == "--verbose" || a == "--debug") {
            if (!VERBOSE_CAPABLE) {
                die("this binary was compiled with EXTRACT_THANKS_VERBOSE_CAPABLE=0; rebuild with -DEXTRACT_THANKS_VERBOSE_CAPABLE=1 to use --verbose/--debug");
            }
            opt.verbose = true;
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else if (!a.empty() && a[0] == '-') {
            die("unknown option: " + a);
        } else {
            opt.inputs.emplace_back(a);
        }
    }
    if (opt.inputs.empty())
        die("no input files provided");
    return opt;
}

int main(int argc, char **argv) {
    try {
        Options opt = parse_args(argc, argv);
        xmlInitParser();
        uint64_t files = 0, total_raw = 0, total_edges = 0, total_excluded = 0;
        for (const fs::path &input : opt.inputs) {
            Stats s = process_file(input, opt);
            ++files;
            total_raw += s.raw_thanks_type_occurrences;
            total_edges += s.retained_observed_edges;
            total_excluded += s.excluded_deleted;
        }
        xmlCleanupParser();
        std::cerr << "DONE files=" << files << " total_raw_<type>thanks</type>=" << total_raw
                  << " total_retained_observed_edges=" << total_edges
                  << " total_excluded_deleted_rows=" << total_excluded << "\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        xmlCleanupParser();
        return 1;
    }
}
