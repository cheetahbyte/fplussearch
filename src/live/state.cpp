#include "live/live.hpp"
#include "search/matching.hpp"

#include <algorithm>

namespace fplussearch {

std::shared_ptr<const Live::State> Live::state() const {
  std::lock_guard lk(state_m_);
  return state_;
}

Results Live::search(const State& s, const Query& q, size_t limit) {
  std::lock_guard lk(search_m_);  // the engine's pool runs one search at a time
  if (!q.syms.empty()) return s.sx ? engine_.search_symbols(*s.ix, *s.sx, q, limit) : Results{};
  return engine_.search(*s.ix, q, limit, &s.overlay);
}

std::vector<std::string> Live::files(const State& s, const Query& q, size_t max) {
  std::vector<uint32_t> hits;
  {
    std::lock_guard lk(search_m_);
    std::mutex m;
    engine_.each_file(*s.ix, q, [&](uint32_t e) {
      if ((s.dead[e >> 6] >> (e & 63)) & 1) return;
      std::lock_guard g(m);
      if (hits.size() < max) hits.push_back(e);
    });
  }
  std::vector<std::string> out;
  out.reserve(hits.size());
  for (uint32_t e : hits) out.push_back(s.ix->path(e));
  for (const auto& [path, x] : s.extras)
    if (out.size() < max && !x->dir && (q.scope.empty() || path.starts_with(q.scope + "/")) && path_matches(q, path))
      out.push_back(path);
  std::sort(out.begin(), out.end());
  return out;
}

Live::Status Live::status() const {
  Status st;
  const auto s = state();
  st.owner = owner_;
  st.busy = busy_;
  if (!s) return st;
  st.ready = true;
  st.entries = s->ix->count() - s->dead_count + s->extras.size();
  st.dirs = s->ix->dirs;
  st.overlay = s->extras.size();
  st.removed = s->dead_count;
  st.symbols = s->sx ? s->sx->occ.n : 0;
  st.event_id = s->event_id;
  if (content_) {
    st.content_docs = content_->docs();
    st.content_bytes = content_->bytes();
    st.content_pending = content_busy_;
  }
  return st;
}

std::shared_ptr<const SymbolIndex> Live::load_syms(const Index& ix) const {
  auto sx = std::make_shared<SymbolIndex>();
  return load_symbols(*sx, symbol_file(opt_.cache_file), ix) ? sx : nullptr;
}

std::shared_ptr<Live::State> Live::fresh_state(std::shared_ptr<const Index> ix) const {
  auto s = std::make_shared<State>();
  s->sx = load_syms(*ix);
  s->dead.assign((ix->count() + 63) / 64, 0);
  s->event_id = ix->event_id;
  s->ix = std::move(ix);
  return s;
}

void Live::publish(std::shared_ptr<State> s) {
  s->overlay.dead = &s->dead;
  s->overlay.extras.clear();
  s->overlay.extras.reserve(s->extras.size());
  for (const auto& [path, x] : s->extras) s->overlay.extras.push_back(x.get());
  {
    std::lock_guard lk(state_m_);
    state_ = std::move(s);
  }
  if (on_change_) on_change_();
}

}  // namespace fplussearch
