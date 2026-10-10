#include "daemon/protocol.hpp"
#include "live/live.hpp"
#include "search/matching.hpp"
#include "support/json.hpp"
#include "symbols/symbol_index.hpp"

#include <chrono>
#include <cstdio>
#include <unordered_map>

namespace fplussearch {

namespace {

std::string number(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", v);
  return buf;
}

void hit_json(std::string& out, const Live::State& s, size_t i, const Results& r) {
  const Index& ix = *s.ix;
  out += "{\"path\":";
  if (const Extra* x = r.extra[i]) {
    json_escape(out, x->path);
    out += ",\"dir\":" + std::string(x->dir ? "true" : "false");
    out += ",\"size\":" + std::to_string(x->size) + ",\"mtime\":" + std::to_string(x->mtime);
  } else {
    const uint32_t e = r.top[i];
    json_escape(out, ix.path(e));
    out += ",\"dir\":" + std::string(ix.is_dir(e) ? "true" : "false");
    out += ",\"size\":" + std::to_string(ix.is_dir(e) ? 0 : ix.size(e)) + ",\"mtime\":" + std::to_string(ix.mtime[e]);
  }
  out += ",\"score\":" + std::to_string(r.score[i]) + "}";
}

void symbols_json(std::string& out, const Live::State& s, const Results& r) {
  std::vector<std::string> paths;
  std::vector<std::string_view> names;
  std::unordered_map<std::string, std::vector<size_t>> files;
  paths.reserve(r.top.size());
  names.reserve(r.top.size());
  for (const uint32_t o : r.top) {
    paths.push_back(s.ix->path(s.sx->occ[o]));
    names.push_back(s.sx->symbol(o));
    files[paths.back()].push_back(paths.size() - 1);
  }
  std::vector<SymbolLocation> locations(r.top.size());
  for (const auto& [path, hits] : files) {
    std::vector<std::string_view> requested;
    requested.reserve(hits.size());
    for (const size_t i : hits) requested.push_back(names[i]);
    const auto found = locate_symbols(path, requested);
    for (size_t j = 0; j < hits.size(); ++j) locations[hits[j]] = found[j];
  }
  out += ",\"hits\":[";
  for (size_t i = 0; i < r.top.size(); ++i) {
    const std::string& path = paths[i];
    const std::string_view name = names[i];
    const SymbolLocation loc = locations[i];
    if (i) out += ',';
    out += "{\"path\":";
    json_escape(out, path);
    out += ",\"line\":" + std::to_string(loc.line) + ",\"symbol\":";
    json_escape(out, name);
    out += ",\"kind\":";
    json_escape(out, loc.found ? kind_name(loc.kind) : "?");
    out += '}';
  }
  out += ']';
}

void status_json(std::string& out, const Live& live) {
  const Live::Status st = live.status();
  out += ",\"ready\":" + std::string(st.ready ? "true" : "false");
  out += ",\"entries\":" + std::to_string(st.entries) + ",\"dirs\":" + std::to_string(st.dirs);
  out += ",\"overlay\":" + std::to_string(st.overlay) + ",\"removed\":" + std::to_string(st.removed);
  out += ",\"symbols\":" + std::to_string(st.symbols) + ",\"event_id\":" + std::to_string(st.event_id);
  out += ",\"owner\":" + std::string(st.owner ? "true" : "false");
  out += ",\"busy\":" + std::string(st.busy ? "true" : "false");
  out += ",\"full_disk_access\":" + std::string(has_full_disk_access() ? "true" : "false");
  out += ",\"content_docs\":" + std::to_string(st.content_docs);
  out += ",\"content_bytes\":" + std::to_string(st.content_bytes);
  out += ",\"content_pending\":" + std::string(st.content_pending ? "true" : "false");
}

std::string error_json(std::string_view msg, const std::string& id) {
  std::string out = "{\"ok\":false" + id + ",\"error\":";
  json_escape(out, msg);
  return out + "}";
}

}  // namespace

std::string handle_request(Live& live, std::string_view line, const std::string& home) {
  JsonValue req;
  if (!json_parse(line, req) || req.type != JsonValue::Object)
    return error_json("bad request: expected a JSON object", "");
  std::string id;
  if (const JsonValue* v = req.get("id")) {
    id = ",\"id\":";
    if (v->type == JsonValue::String) json_escape(id, v->s);
    else id += number(v->n);
  }
  auto str = [&](std::string_view k) { return req.str(k); };
  auto num = [&](std::string_view k, double d) { return req.num(k, d); };
  const std::string op = str("op");
  std::string out = "{\"ok\":true" + id;
  if (op == "status") {
    status_json(out, live);
    return out + "}";
  }
  const auto s = live.state();
  if (!s) return error_json("indexing (the first run scans the whole disk)", id);
  const Query q = parse_query(str("q"), home);
  if (op == "grep" || !q.grep.empty()) {
    const ContentIndex* content = live.content();
    if (!content) return error_json("content search is off", id);
    GrepOptions o;
    o.pattern = req.str("pattern").empty() ? q.grep : req.str("pattern");
    if (o.pattern.empty()) return error_json("grep needs a pattern", id);
    o.regex = req.get("mode") ? req.str("mode") == "regex" : q.grep_mode == GrepMode::Regex;
    o.limit = size_t(num("limit", q.limit ? double(q.limit) : 50));
    o.per_file = size_t(num("per_file", 5));
    o.budget_ms = int(num("budget_ms", 250));
    o.scope = q.scope;
    Query filters = q;
    filters.grep.clear();
    if (!filters.tokens.empty() || !filters.exts.empty() || filters.types != kTypeAny || filters.name_re ||
        filters.path_re)
      o.keep = [filters](std::string_view path) { return path_matches(filters, path); };
    const auto t0 = std::chrono::steady_clock::now();
    GrepResult g;
    if (o.scope.empty() || content->covers(o.scope)) {
      g = content->grep(o);
    } else {
      // Outside the content index (in:/etc): read the matching files directly.
      g = content->grep_files(o, live.files(*s, filters, 200000));
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!g.error.empty()) return error_json(g.error, id);
    out += ",\"ms\":" + number(ms) + ",\"candidates\":" + std::to_string(g.candidates) +
           ",\"read\":" + std::to_string(g.read) + ",\"complete\":" + (g.complete ? "true" : "false") + ",\"files\":[";
    for (size_t i = 0; i < g.files.size(); ++i) {
      if (i) out += ',';
      out += "{\"path\":";
      json_escape(out, g.files[i].path);
      out += ",\"lines\":[";
      for (size_t j = 0; j < g.files[i].lines.size(); ++j) {
        if (j) out += ',';
        out += "{\"line\":" + std::to_string(g.files[i].lines[j].line) + ",\"text\":";
        json_escape(out, g.files[i].lines[j].text);
        out += '}';
      }
      out += "]}";
    }
    return out + "]}";
  }
  if (!op.empty()) return error_json("unknown op " + op, id);
  if (!q.error.empty()) {
    out += ",\"warning\":";
    json_escape(out, q.error);
  }
  const size_t limit = size_t(num("limit", q.limit ? double(q.limit) : 50));
  const auto t0 = std::chrono::steady_clock::now();
  const Results r = live.search(*s, q, limit);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  out += ",\"total\":" + std::to_string(r.total) + ",\"ms\":" + number(ms);
  if (!q.syms.empty()) {
    if (s->sx) symbols_json(out, *s, r);
    else out += ",\"hits\":[]";
    return out + "}";
  }
  out += ",\"hits\":[";
  for (size_t i = 0; i < r.top.size(); ++i) {
    if (i) out += ',';
    hit_json(out, *s, i, r);
  }
  return out + "]}";
}

}  // namespace fplussearch
