// SPDX-License-Identifier: Apache-2.0
// Message format (little-endian, same-architecture MPI exchange):
//   header  : "ABIO" | version u8 | layout u8 | 0 u16 | agents u64 | block u32
//   | chunks u32 offsets : u64[chunks + 1], absolute chunk starts; the last
//   equals the message size chunk   : u32 agent counts per type | u32 behavior
//   counts per type
//             per agent type: fields (rows or columns) | u8 behavior count per
//             agent per behavior type: u16 owner | u8 slot | fields (rows or
//             columns)
#include "abmio/exchange.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace abmio {
namespace {

constexpr char kMagic[4] = {'A', 'B', 'I', 'O'};
constexpr uint8_t kVersion = 1;
constexpr uint64_t kHeaderSize = 24;
constexpr int kTA = gen::kNumAgentTypes;
constexpr int kTB = gen::kNumBehaviorTypes;
constexpr uint64_t kChunkHeader = 4 * (kTA + kTB);
constexpr size_t kMaxFields = 64;

struct FieldMap {
  uint64_t start[kMaxFields];
  uint64_t stride[kMaxFields];
};
struct ChunkPlan {
  FieldMap af[kTA + 1], bf[kTB + 1];
  uint64_t nbeh[kTA + 1] = {}, owner[kTB + 1] = {}, slot[kTB + 1] = {};
  uint64_t size = 0;
};

template <class T>
void Put(char* p, T v) {
  std::memcpy(p, &v, sizeof(T));
}
template <class T>
T Get(const char* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

void MapFields(Layout layout, const std::vector<uint32_t>& sizes, uint64_t n,
               uint64_t& pos, FieldMap& m) {
  if (sizes.size() > kMaxFields)
    throw std::logic_error("too many fields in one type");
  if (layout == Layout::kRows) {
    uint64_t rec = 0, off = 0;
    for (auto s : sizes)
      rec += s;
    for (size_t f = 0; f < sizes.size(); ++f) {
      m.start[f] = pos + off;
      m.stride[f] = rec;
      off += sizes[f];
    }
    pos += rec * n;
  } else {
    for (size_t f = 0; f < sizes.size(); ++f) {
      m.start[f] = pos;
      m.stride[f] = sizes[f];
      pos += uint64_t{sizes[f]} * n;
    }
  }
}

ChunkPlan Plan(Layout layout, const uint32_t* na, const uint32_t* nb) {
  ChunkPlan p;
  uint64_t pos = kChunkHeader;
  for (int t = 1; t <= kTA; ++t) {
    MapFields(layout, gen::AgentFieldSizes(t), na[t], pos, p.af[t]);
    p.nbeh[t] = pos;
    pos += na[t];
  }
  for (int t = 1; t <= kTB; ++t) {
    p.owner[t] = pos;
    pos += 2ull * nb[t];
    p.slot[t] = pos;
    pos += nb[t];
    MapFields(layout, gen::BehaviorFieldSizes(t), nb[t], pos, p.bf[t]);
  }
  p.size = pos;
  return p;
}

struct Writer {
  char* base;
  const FieldMap* m;
  uint64_t i;
  size_t f = 0;
  template <class T>
  void operator()(T& x) {
    std::memcpy(base + m->start[f] + i * m->stride[f], &x, sizeof(T));
    ++f;
  }
};
struct Reader {
  const char* base;
  const FieldMap* m;
  uint64_t i;
  size_t f = 0;
  template <class T>
  void operator()(T& x) {
    std::memcpy(&x, base + m->start[f] + i * m->stride[f], sizeof(T));
    ++f;
  }
};

}  // namespace

uint64_t Pack(const std::vector<Agent*>& agents, Layout layout,
              std::vector<char>& out, uint32_t block) {
  if (block == 0 || block > 65535)
    throw std::invalid_argument("block must be in [1, 65535]");
  if (layout != Layout::kRows && layout != Layout::kColumns)
    throw std::invalid_argument("unknown layout");
  const uint64_t n = agents.size();
  const uint64_t nch = (n + block - 1) / block;
  if (nch > std::numeric_limits<uint32_t>::max())
    throw std::invalid_argument("too many blocks");
  uint64_t pos = kHeaderSize + 8 * (nch + 1);
  if (out.size() < pos)
    out.resize(pos);
  std::vector<uint8_t> type(block);
  std::vector<uint16_t> idx(block);
  for (uint64_t c = 0; c < nch; ++c) {
    const uint64_t k0 = c * block, k1 = std::min<uint64_t>(n, k0 + block);
    uint32_t na[kTA + 1] = {}, nb[kTB + 1] = {};
    for (uint64_t k = k0; k < k1; ++k) {
      const Agent* a = agents[k];
      const int t = a->TypeId();
      if (t < 1 || t > kTA)
        throw std::invalid_argument("agent type not registered");
      if (a->behaviors.size() > 255)
        throw std::invalid_argument("more than 255 behaviors on one agent");
      for (const Behavior* b : a->behaviors) {
        const int bt = b->TypeId();
        if (bt < 1 || bt > kTB)
          throw std::invalid_argument("behavior type not registered");
        ++nb[bt];
      }
      type[k - k0] = static_cast<uint8_t>(t);
      idx[k - k0] = static_cast<uint16_t>(na[t]++);
    }
    const ChunkPlan p = Plan(layout, na, nb);
    if (out.size() < pos + p.size)
      out.resize(std::max<uint64_t>(2 * out.size(), pos + p.size));
    Put(out.data() + kHeaderSize + 8 * c, pos);
    char* q = out.data() + pos;
    for (int t = 1; t <= kTA; ++t)
      Put(q + 4 * (t - 1), na[t]);
    for (int t = 1; t <= kTB; ++t)
      Put(q + 4 * (kTA + t - 1), nb[t]);
    uint32_t start[kTA + 1] = {}, bi[kTB + 1] = {};
    for (int t = 2; t <= kTA; ++t)
      start[t] = start[t - 1] + na[t - 1];
    for (uint64_t k = k0; k < k1; ++k) {
      Agent* a = agents[k];
      const int t = type[k - k0];
      const uint64_t i = idx[k - k0];
      Writer w{q, &p.af[t], i};
      gen::VisitAgentById(w, *a, t);
      q[p.nbeh[t] + i] = static_cast<char>(a->behaviors.size());
      for (size_t s = 0; s < a->behaviors.size(); ++s) {
        Behavior* b = a->behaviors[s];
        const int bt = b->TypeId();
        const uint64_t j = bi[bt]++;
        Put(q + p.owner[bt] + 2 * j, static_cast<uint16_t>(start[t] + i));
        q[p.slot[bt] + j] = static_cast<char>(s);
        Writer wb{q, &p.bf[bt], j};
        gen::VisitBehaviorById(wb, *b, bt);
      }
    }
    pos += p.size;
  }
  Put(out.data() + kHeaderSize + 8 * nch, pos);
  char* h = out.data();
  std::memcpy(h, kMagic, 4);
  h[4] = static_cast<char>(kVersion);
  h[5] = static_cast<char>(layout);
  Put<uint16_t>(h + 6, 0);
  Put(h + 8, n);
  Put(h + 16, block);
  Put(h + 20, static_cast<uint32_t>(nch));
  return pos;
}

Status Unpack(const char* data, uint64_t size, Arena& arena) {
  arena.view.clear();
  if (size < kHeaderSize)
    return Status::kTruncated;
  if (std::memcmp(data, kMagic, 4) != 0 ||
      static_cast<uint8_t>(data[4]) != kVersion)
    return Status::kBadHeader;
  const auto layout = static_cast<Layout>(data[5]);
  if (layout != Layout::kRows && layout != Layout::kColumns)
    return Status::kBadHeader;
  const uint64_t n = Get<uint64_t>(data + 8);
  const uint32_t block = Get<uint32_t>(data + 16),
                 nch = Get<uint32_t>(data + 20);
  if (block == 0 || block > 65535 || n > size)
    return Status::kBadHeader;  // every agent needs >= 1 byte
  if (nch != (n + block - 1) / block)
    return Status::kBadHeader;
  const uint64_t table_end = kHeaderSize + 8 * (uint64_t{nch} + 1);
  if (table_end > size)
    return Status::kTruncated;
  if (Get<uint64_t>(data + kHeaderSize) != table_end && nch > 0)
    return Status::kBadHeader;
  if (Get<uint64_t>(data + kHeaderSize + 8 * uint64_t{nch}) != size)
    return Status::kSizeMismatch;

  arena.view.resize(n);
  uint64_t g = 0, pi[kTA + 1] = {};
  std::vector<uint32_t> prefix;
  std::vector<uint8_t> seen;
  for (uint64_t c = 0; c < nch; ++c) {
    const uint64_t off = Get<uint64_t>(data + kHeaderSize + 8 * c);
    const uint64_t end = Get<uint64_t>(data + kHeaderSize + 8 * (c + 1));
    if (off < table_end || end > size || end < off ||
        end - off < kChunkHeader) {
      arena.view.clear();
      return Status::kBadHeader;
    }
    const char* q = data + off;
    uint32_t na[kTA + 1] = {}, nb[kTB + 1] = {};
    uint64_t chunk_agents = 0, chunk_behaviors = 0;
    for (int t = 1; t <= kTA; ++t)
      chunk_agents += na[t] = Get<uint32_t>(q + 4 * (t - 1));
    for (int t = 1; t <= kTB; ++t)
      chunk_behaviors += nb[t] = Get<uint32_t>(q + 4 * (kTA + t - 1));
    const uint64_t expected = (c + 1 < nch) ? block : n - c * block;
    if (chunk_agents != expected || chunk_behaviors > 255 * chunk_agents) {
      arena.view.clear();
      return Status::kSizeMismatch;
    }
    const ChunkPlan p = Plan(layout, na, nb);
    if (p.size != end - off) {
      arena.view.clear();
      return Status::kSizeMismatch;
    }
    Agent** lv = arena.view.data() + g;
    uint64_t l = 0, slots = 0;
    prefix.assign(chunk_agents + 1, 0);
    for (int t = 1; t <= kTA; ++t) {
      auto& pool = arena.pool[t];
      while (pool.size() < pi[t] + na[t])
        pool.emplace_back(gen::NewAgent(t));
      for (uint64_t i = 0; i < na[t]; ++i, ++l) {
        Agent* a = pool[pi[t] + i].get();
        Reader r{q, &p.af[t], i};
        gen::VisitAgentById(r, *a, t);
        const size_t want = static_cast<uint8_t>(q[p.nbeh[t] + i]);
        while (a->behaviors.size() > want) {
          delete a->behaviors.back();
          a->behaviors.pop_back();
        }
        a->behaviors.resize(want, nullptr);
        prefix[l] = static_cast<uint32_t>(slots);
        slots += want;
        lv[l] = a;
      }
      pi[t] += na[t];
    }
    if (slots != chunk_behaviors) {
      arena.view.clear();
      return Status::kSizeMismatch;
    }
    seen.assign(slots, 0);
    for (int bt = 1; bt <= kTB; ++bt) {
      for (uint64_t j = 0; j < nb[bt]; ++j) {
        const uint16_t owner = Get<uint16_t>(q + p.owner[bt] + 2 * j);
        const uint8_t slot = static_cast<uint8_t>(q[p.slot[bt] + j]);
        if (owner >= chunk_agents || slot >= lv[owner]->behaviors.size()) {
          arena.view.clear();
          return Status::kBadIndex;
        }
        uint8_t& mark = seen[prefix[owner] + slot];
        if (mark) {
          arena.view.clear();
          return Status::kCorrupt;
        }
        mark = 1;
        Behavior*& b = lv[owner]->behaviors[slot];
        if (b == nullptr || b->TypeId() != bt) {
          delete b;
          b = gen::NewBehavior(bt);
        }
        Reader r{q, &p.bf[bt], j};
        gen::VisitBehaviorById(r, *b, bt);
      }
    }
    g += l;
  }
  return g == n ? Status::kOk : Status::kCorrupt;
}

}  // namespace abmio
