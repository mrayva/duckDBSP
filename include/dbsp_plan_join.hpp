#pragma once

#include "dbsp_plan_aggregate.hpp"

namespace dbsp_native {
// Incremental inner join (B3). Bilinear delta rule per step:
//   Δout = Δl ⋈ R_old + L_old ⋈ Δr + Δl ⋈ Δr
// Each side is indexed by its equi-key values; rows with a NULL key are
// never indexed (SQL: NULL never matches). Residual (non-equality)
// conditions are checked per candidate pair via Value comparison. With no
// conditions at all this degenerates to a cross product (single bucket).
// Output row = left columns followed by right columns (LogicalJoin layout).
// Shared join arrangement (I1): one table-side equi-key index maintained
// ONCE per table delta by the CDC layer and consumed read-only by every
// join that registered for the same (table, key expressions) fingerprint.
// Updated at the START of propagation, so consuming joins see the NEW
// state for this side; their bilinear formula drops the Δl⋈Δr term
// accordingly (v1 shares at most one side per join, which keeps view
// initialization = plain replay of the local side).
// Decode a packed bucket row straight from arena/overlay bytes into a
// hash-seeded DuckDBRow. Probe rows become scratch-map keys immediately, so
// the hash is always paid — hash_row_fast on the decoded values is the same
// bits as the lazy ColumnVec::hash at a fraction of the cost (see its doc
// in dbsp_checkpoint.hpp), and assign() replaces a COW-checked push_back
// per value with one payload allocation.
inline DuckDBRow decode_probe_row(const char *p) {
  std::vector<duckdb::Value> vals;
  packed::decode_values(p, vals);
  DuckDBRow row;
  const size_t h = hash_row_fast(vals);
  row.columns.assign(std::move(vals));
  row.columns.set_hash(h);
  return row;
}

struct SharedArrangement {
  using RowWeights = std::unordered_map<DuckDBRow, int64_t, DuckDBRowHash>;
  using Index = std::unordered_map<DuckDBRow, RowWeights, DuckDBRowHash>;

  std::string table;
  bool null_safe = false;
  bool track_weights = false; // per-row totals (outer pads / marks)
  bool track_counters = false; // total + null-key weights (marks)
  // Column projection applied to raw table rows before indexing — join
  // sides are usually MAP_COLS(SOURCE), so the arrangement stores rows in
  // the shape the join consumes
  bool project = false;
  std::vector<duckdb::idx_t> column_idxs;

  Index index;
  RowWeights weights;
  int64_t total = 0;
  int64_t nulls = 0;

  // Phase 2d inc2: packed storage (dbsp_packed_row.hpp). Set at
  // registration when the side types are codec-clean and the arrangement
  // tracks no counters (MARK) — probes decode buckets into the consumer's
  // scratch, exactly like the spilled/projected paths.
  bool packed_ok = false;
  std::unordered_map<std::string, std::vector<std::pair<std::string, int64_t>>>
      packed;
  // Recovery inc 3: adopted durable layer (immutable, key-sorted; loaded
  // from the fingerprint sidecar when the source table's watermark
  // matched at register time). `packed` above becomes the delta overlay;
  // weights sum across layers at probe.
  flatpacked::FlatPackedIndex flat;

  bool probe_packed(const DuckDBRow &key, RowWeights &out,
                    const std::vector<duckdb::idx_t> *proj) const {
    out.clear();
    std::string kb;
    if (!packed::encode_row(kb, key)) {
      return false;
    }
    auto it = packed.find(kb);
    const flatpacked::DirEnt *fe = flat.find(kb);
    const bool have_overlay = it != packed.end() && !it->second.empty();
    if (!have_overlay && fe == nullptr) {
      return false;
    }
    auto emit = [&](const char *p, int64_t w) {
      if (proj) {
        std::vector<duckdb::Value> vals;
        packed::decode_values(p, vals);
        std::vector<duckdb::Value> pv;
        pv.reserve(proj->size());
        for (auto idx : *proj) {
          pv.push_back(idx < vals.size() ? vals[idx] : duckdb::Value());
        }
        DuckDBRow projected;
        const size_t h = hash_row_fast(pv);
        projected.columns.assign(std::move(pv));
        projected.columns.set_hash(h);
        out[std::move(projected)] += w; // collapse under projection
      } else {
        out[decode_probe_row(p)] += w;
      }
    };
    if (fe != nullptr && !have_overlay) {
      out.reserve(fe->bucket_n);
      for (uint32_t b = 0; b < fe->bucket_n; b++) {
        const auto &be = flat.bucket_at(fe->bucket_off + b);
        emit(reinterpret_cast<const char *>(flat.arena_data() + be.row_off),
             be.weight);
      }
    } else if (fe == nullptr) {
      out.reserve(it->second.size());
      for (const auto &[bytes, w] : it->second) {
        emit(bytes.data(), w);
      }
    } else {
      // merge by row bytes: flat weight + overlay delta
      std::unordered_map<std::string, int64_t> merged;
      for (uint32_t b = 0; b < fe->bucket_n; b++) {
        const auto &be = flat.bucket_at(fe->bucket_off + b);
        merged.emplace(
            std::string(reinterpret_cast<const char *>(flat.arena_data() +
                                                       be.row_off),
                        be.row_len),
            be.weight);
      }
      for (const auto &[bytes, w] : it->second) {
        merged[bytes] += w;
      }
      for (const auto &[bytes, w] : merged) {
        if (w != 0) {
          emit(bytes.data(), w);
        }
      }
    }
    // projection collapse can cancel to zero-weight rows; drop them
    for (auto mit = out.begin(); mit != out.end();) {
      if (mit->second == 0) {
        mit = out.erase(mit);
      } else {
        ++mit;
      }
    }
    return !out.empty();
  }

  // Fold flat + overlay into one FlatPackedIndex (sidecar save).
  void fold_packed(flatpacked::FlatPackedIndex &out) const {
    out.clear();
    auto add_bucket = [&](const std::string &kb,
                          const std::vector<std::pair<std::string, int64_t>>
                              &rows) {
      if (rows.empty()) {
        return;
      }
      flatpacked::DirEnt de;
      de.key_off = out.append_bytes(kb);
      de.key_len = static_cast<uint32_t>(kb.size());
      de.bucket_off = out.buckets.size();
      de.bucket_n = static_cast<uint32_t>(rows.size());
      for (const auto &[rb, w] : rows) {
        flatpacked::BucketEnt be;
        be.row_off = out.append_bytes(rb);
        be.row_len = static_cast<uint32_t>(rb.size());
        be.weight = w;
        out.buckets.push_back(be);
      }
      out.dir.push_back(de);
    };
    std::vector<std::pair<std::string, int64_t>> scratch;
    for (uint64_t i = 0; i < flat.dir_size(); i++) {
      const auto &de = flat.dir_at(i);
      const std::string kb(
          reinterpret_cast<const char *>(flat.arena_data() + de.key_off),
          de.key_len);
      auto ov = packed.find(kb);
      scratch.clear();
      if (ov == packed.end()) {
        for (uint32_t b = 0; b < de.bucket_n; b++) {
          const auto &be = flat.bucket_at(de.bucket_off + b);
          scratch.emplace_back(
              std::string(reinterpret_cast<const char *>(flat.arena_data() +
                                                         be.row_off),
                          be.row_len),
              be.weight);
        }
      } else {
        std::unordered_map<std::string, int64_t> m;
        for (uint32_t b = 0; b < de.bucket_n; b++) {
          const auto &be = flat.bucket_at(de.bucket_off + b);
          m.emplace(std::string(reinterpret_cast<const char *>(
                                    flat.arena_data() + be.row_off),
                                be.row_len),
                    be.weight);
        }
        for (const auto &[rb, dw] : ov->second) {
          m[rb] += dw;
        }
        for (const auto &[rb, w] : m) {
          if (w != 0) {
            scratch.emplace_back(rb, w);
          }
        }
      }
      add_bucket(kb, scratch);
    }
    for (const auto &[kb, rows] : packed) {
      if (flat.find(kb) != nullptr) {
        continue; // already merged above
      }
      scratch.assign(rows.begin(), rows.end());
      add_bucket(kb, scratch);
    }
    out.finish_build();
  }

  // Fold ONLY the keys touched since adopt into replacement buckets (the
  // delta-append sidecar payload): merge flat + overlay per dirty key,
  // O(touched buckets) instead of the whole arrangement. An empty
  // replacement stays in `out` — it masks a base bucket the overlay
  // emptied.
  void fold_packed_touched(flatpacked::ReplacementBuckets &out) const {
    out.clear();
    out.reserve(packed.size());
    for (const auto &[kb, rows] : packed) {
      std::vector<std::pair<std::string, int64_t>> repl;
      const auto *fe = flat.find(kb);
      if (fe == nullptr) {
        std::unordered_map<std::string, int64_t> m;
        for (const auto &[rb, dw] : rows) {
          m[rb] += dw;
        }
        for (const auto &[rb, w] : m) {
          if (w != 0) {
            repl.emplace_back(rb, w);
          }
        }
      } else {
        std::unordered_map<std::string, int64_t> m;
        for (uint32_t b = 0; b < fe->bucket_n; b++) {
          const auto &be = flat.bucket_at(fe->bucket_off + b);
          m.emplace(std::string(reinterpret_cast<const char *>(
                                    flat.arena_data() + be.row_off),
                                be.row_len),
                    be.weight);
        }
        for (const auto &[rb, dw] : rows) {
          m[rb] += dw;
        }
        for (const auto &[rb, w] : m) {
          if (w != 0) {
            repl.emplace_back(rb, w);
          }
        }
      }
      out.emplace_back(kb, std::move(repl));
    }
  }

  // STREAMING INITIAL FILL (M-tier create): during backfill the rows
  // stream into flat PODs — a small key->id map (join keys are low
  // cardinality), one row-bytes arena, and 24B entries — instead of the
  // mutable bucket maps (whose nodes+strings peaked at ~12GB during a
  // 144M backfill). finish_initial_fill() integer-sorts the entries,
  // merges duplicate (key,row) weights, and builds the FlatPackedIndex
  // AROUND the row arena (rows never copy; unique key bytes append at
  // the arena tail). Only plain packed arrangements stream; everything
  // else keeps the ordinary apply() path and the RAM fold fallback.
  struct BulkBuilder {
    struct Ent {
      uint32_t key_id;
      uint32_t rb_len;
      uint64_t rb_off;
      int64_t w;
    };
    std::unordered_map<std::string, uint32_t> key_ids;
    std::vector<std::string> key_bytes;
    std::vector<Ent> entries;
    std::vector<uint8_t> rb_arena;
  };
  std::unique_ptr<BulkBuilder> bulk;

  void begin_initial_fill() {
    if (packed_ok && !track_weights && !track_counters && !is_spilled() &&
        packed.empty() && flat.empty()) {
      bulk = std::make_unique<BulkBuilder>();
    }
  }

  void finish_initial_fill() {
    if (!bulk) {
      compact_to_flat(); // non-streaming shapes keep the fold fallback
      return;
    }
    auto b = std::move(bulk);
    // Key order for the directory = key BYTES order (find() binary
    // searches by bytes); entries group by integer id first (fast sort).
    std::vector<uint32_t> key_order(b->key_bytes.size());
    for (uint32_t i = 0; i < key_order.size(); i++) {
      key_order[i] = i;
    }
    std::sort(key_order.begin(), key_order.end(),
              [&](uint32_t a, uint32_t c) {
                return b->key_bytes[a] < b->key_bytes[c];
              });
    const uint8_t *ra = b->rb_arena.data();
    std::sort(b->entries.begin(), b->entries.end(),
              [&](const BulkBuilder::Ent &x, const BulkBuilder::Ent &y) {
                if (x.key_id != y.key_id) {
                  return x.key_id < y.key_id;
                }
                return flatpacked::cmp_bytes(ra + x.rb_off, x.rb_len,
                                             ra + y.rb_off, y.rb_len) < 0;
              });
    // Per-key entry ranges (entries sorted by key_id).
    std::vector<std::pair<size_t, size_t>> range(b->key_bytes.size(),
                                                 {0, 0});
    for (size_t i = 0; i < b->entries.size();) {
      size_t j = i + 1;
      while (j < b->entries.size() &&
             b->entries[j].key_id == b->entries[i].key_id) {
        j++;
      }
      range[b->entries[i].key_id] = {i, j};
      i = j;
    }
    flatpacked::FlatPackedIndex built;
    built.arena = std::move(b->rb_arena); // rows never copy
    for (const uint32_t id : key_order) {
      auto [lo, hi] = range[id];
      flatpacked::DirEnt de;
      de.bucket_off = built.buckets.size();
      size_t i = lo;
      while (i < hi) {
        size_t j = i + 1;
        int64_t w = b->entries[i].w;
        while (j < hi &&
               flatpacked::cmp_bytes(
                   built.arena.data() + b->entries[j].rb_off,
                   b->entries[j].rb_len,
                   built.arena.data() + b->entries[i].rb_off,
                   b->entries[i].rb_len) == 0) {
          w += b->entries[j].w;
          j++;
        }
        if (w != 0) {
          flatpacked::BucketEnt be;
          be.row_off = b->entries[i].rb_off;
          be.row_len = b->entries[i].rb_len;
          be.weight = w;
          built.buckets.push_back(be);
        }
        i = j;
      }
      de.bucket_n = static_cast<uint32_t>(built.buckets.size() -
                                          de.bucket_off);
      if (de.bucket_n == 0) {
        continue; // key net-emptied
      }
      de.key_off = built.arena.size();
      de.key_len = static_cast<uint32_t>(b->key_bytes[id].size());
      built.arena.insert(built.arena.end(), b->key_bytes[id].begin(),
                         b->key_bytes[id].end());
      built.dir.push_back(de);
    }
    // dir was emitted in key-bytes order already — no finish_build sort.
    flat = std::move(built);
    packed.clear();
  }

  // Lever B (create-path RAM): fold the mutable packed bucket maps into
  // the immutable flat arena the moment the initial fill completes — the
  // same two-layer (flat + overlay) shape a sidecar adopt produces, built
  // locally. The maps cost ~3x the arena (string keys + node overhead,
  // ~12GB at 144M); the arena is contiguous. Probes already merge
  // flat+overlay; subsequent deltas land in `packed` as overlay deltas —
  // exactly the adopt-time invariant. Plain packed arrangements only
  // (pads/marks/spilled keep their paths); no-op when a flat layer
  // already exists (folding would need a merge and the overlay is small
  // then anyway). Callers run at backfill completion, before any
  // concurrent probes.
  void compact_to_flat() {
    if (!packed_ok || track_weights || track_counters || is_spilled() ||
        packed.empty() || !flat.empty()) {
      return;
    }
    flatpacked::FlatPackedIndex folded;
    fold_packed(folded);
    flat = std::move(folded);
    packed.clear();
  }

  // D3c lazy restore: arrangement was registered over a deferred-baseline
  // table and holds no rows yet. CDCManager backfills it (and clears the
  // flag) when the table's baseline materializes — always before any delta
  // is propagated through a consuming join node.
  bool needs_backfill = false;

  // Delta-append sidecars: identity (register-time watermark) of the base
  // file `flat` was adopted from; -1 = flat was not adopted from a file,
  // so a delta save has no base to chain to and the save folds fully.
  int64_t flat_file_wm_count = -1;
  std::string flat_file_wm_hash;
  // Watermark of the last sidecar write this session (full or delta): a
  // save at the same watermark skips. Saves are serialized by the
  // dbsp_save caller; these fields are only touched on that path.
  int64_t sidecar_saved_wm_count = -1;
  std::string sidecar_saved_wm_hash;

  // K2: index spilled to a disk bucket log (dbsp_spill). Probes go
  // through probe_spilled() with an internal mutex — concurrent
  // same-level views (I2) may probe simultaneously. Weights/counters
  // stay in RAM (shared sides never self-pad, so they are empty/tiny).
  std::unique_ptr<SpilledBucketIndex> spilled;
  mutable std::mutex spill_probe_mutex;

  bool is_spilled() const { return spilled != nullptr; }

  static RowDigest digest_of_row(const DuckDBRow &row) {
    std::vector<duckdb::Value> vals;
    vals.reserve(row.columns.size());
    for (size_t i = 0; i < row.columns.size(); i++) {
      vals.push_back(row.columns[i]);
    }
    std::vector<uint8_t> bytes;
    serialize_row(vals, bytes);
    return digest_bytes(bytes.data(), bytes.size());
  }

  // Fill `out` with the bucket for `key` (projected through `proj` when
  // given — O4 consumers see their own column shape); false when absent
  bool probe_spilled(const DuckDBRow &key, RowWeights &out,
                     const std::vector<duckdb::idx_t> *proj = nullptr) const {
    out.clear();
    std::lock_guard<std::mutex> guard(spill_probe_mutex);
    const auto *bucket = spilled->probe(digest_of_row(key));
    if (!bucket) {
      return false;
    }
    for (const auto &[vals, w] : *bucket) {
      DuckDBRow row;
      std::vector<duckdb::Value> copy;
      if (proj) {
        copy.reserve(proj->size());
        for (auto idx : *proj) {
          copy.push_back(idx < vals.size() ? vals[idx] : duckdb::Value());
        }
      } else {
        copy = vals;
      }
      const size_t h = hash_row_fast(copy);
      row.columns.assign(std::move(copy));
      row.columns.set_hash(h);
      out[std::move(row)] += w; // projection can collapse distinct rows
    }
    return true;
  }

  // O4: RAM-resident arrangement probe with consumer projection
  bool probe_projected(const DuckDBRow &key, RowWeights &out,
                       const std::vector<duckdb::idx_t> &proj) const {
    out.clear();
    auto it = index.find(key);
    if (it == index.end()) {
      return false;
    }
    for (const auto &[row, w] : it->second) {
      DuckDBRow projected;
      std::vector<duckdb::Value> vals;
      vals.reserve(proj.size());
      for (auto idx : proj) {
        vals.push_back(idx < row.columns.size() ? row.columns[idx]
                                                : duckdb::Value());
      }
      const size_t h = hash_row_fast(vals);
      projected.columns.assign(std::move(vals));
      projected.columns.set_hash(h);
      out[std::move(projected)] += w; // collapse under projection
    }
    return true;
  }

  void enable_spill(const std::string &path) {
    if (spilled) {
      return;
    }
    spilled = std::make_unique<SpilledBucketIndex>(path);
    auto emit_bucket = [&](const std::string &kb,
                           const std::vector<std::pair<std::string, int64_t>>
                               &bucket) {
      SpilledBucketIndex::Bucket delta;
      delta.reserve(bucket.size());
      DuckDBRow key;
      packed::decode_row(kb, key);
      for (const auto &[rb, w] : bucket) {
        DuckDBRow row;
        packed::decode_row(rb, row);
        std::vector<duckdb::Value> vals;
        vals.reserve(row.columns.size());
        for (size_t i = 0; i < row.columns.size(); i++) {
          vals.push_back(row.columns[i]);
        }
        delta.emplace_back(std::move(vals), w);
      }
      spilled->update(digest_of_row(key), delta);
    };
    if (!flat.empty()) {
      // Rows may live in the immutable flat arena (compact_to_flat or a
      // sidecar adopt) with `packed` holding only overlay deltas: migrate
      // the MERGED view of both layers, then drop them.
      flatpacked::FlatPackedIndex merged;
      fold_packed(merged);
      std::vector<std::pair<std::string, int64_t>> bucket;
      for (const auto &de : merged.dir) {
        const std::string kb(
            reinterpret_cast<const char *>(merged.arena.data() + de.key_off),
            de.key_len);
        bucket.clear();
        bucket.reserve(de.bucket_n);
        for (uint32_t b = 0; b < de.bucket_n; b++) {
          const auto &be = merged.buckets[de.bucket_off + b];
          bucket.emplace_back(
              std::string(reinterpret_cast<const char *>(merged.arena.data() +
                                                         be.row_off),
                          be.row_len),
              be.weight);
        }
        emit_bucket(kb, bucket);
      }
      flat.clear();
      flat_file_wm_count = -1; // no flat layer -> no delta-chain base
      flat_file_wm_hash.clear();
    } else {
      for (const auto &[kb, bucket] : packed) {
        emit_bucket(kb, bucket);
      }
    }
    packed.clear();
    for (const auto &[key, bucket] : index) {
      SpilledBucketIndex::Bucket delta;
      delta.reserve(bucket.size());
      for (const auto &[row, w] : bucket) {
        std::vector<duckdb::Value> vals;
        vals.reserve(row.columns.size());
        for (size_t i = 0; i < row.columns.size(); i++) {
          vals.push_back(row.columns[i]);
        }
        delta.emplace_back(std::move(vals), w);
      }
      spilled->update(digest_of_row(key), delta);
    }
    index.clear();
  }

  void disable_spill() {
    if (!spilled) {
      return;
    }
    spilled->scan([&](const SpilledBucketIndex::Bucket &bucket) {
      for (const auto &[vals, w] : bucket) {
        DuckDBRow row;
        std::vector<duckdb::Value> copy = vals;
        row.columns.assign(std::move(copy));
        DuckDBRow key;
        if (!eval_key(row, key)) {
          continue;
        }
        if (packed_ok) {
          std::string kb, rb;
          if (packed::encode_row(kb, key) && packed::encode_row(rb, row)) {
            packed[std::move(kb)].emplace_back(std::move(rb), w);
            continue;
          }
        }
        index[key][row] += w;
      }
    });
    spilled.reset();
  }

  // Key evaluation owned by the arrangement (first registrant's bound
  // expressions; keep_alive pins that plan for the arrangement's lifetime)
  std::shared_ptr<PlanKeepAlive> keep_alive;
  std::unique_ptr<BatchEvaluator> key_eval;
  std::vector<std::unique_ptr<RowExprEval>> row_key_evals; // single-row

  bool eval_key(const DuckDBRow &row, DuckDBRow &key) const {
    std::vector<duckdb::Value> vals;
    vals.reserve(row_key_evals.size());
    for (const auto &k : row_key_evals) {
      duckdb::Value v = k->eval(row);
      if (v.IsNull() && !null_safe) {
        return false;
      }
      vals.push_back(std::move(v));
    }
    key.columns.assign(std::move(vals));
    return true;
  }

  void apply(const DuckDBZSet &delta) {
    // Project raw table rows into the join side's shape first (mirrors
    // the view's own MAP_COLS node), then batched key extraction and the
    // same integration the join used
    std::vector<DuckDBRow> projected;
    std::vector<const DuckDBRow *> rows;
    std::vector<int64_t> ws;
    rows.reserve(delta.size());
    ws.reserve(delta.size());
    if (project) {
      projected.reserve(delta.size());
      for (const auto &[row, w] : delta) {
        DuckDBRow out;
        std::vector<duckdb::Value> vals;
        vals.reserve(column_idxs.size());
        for (auto idx : column_idxs) {
          vals.push_back(idx < row.columns.size() ? row.columns[idx]
                                                  : duckdb::Value());
        }
        out.columns.assign(std::move(vals));
        projected.push_back(std::move(out));
        ws.push_back(w);
      }
      for (const auto &r : projected) {
        rows.push_back(&r);
      }
    } else {
      for (const auto &[row, w] : delta) {
        rows.push_back(&row);
        ws.push_back(w);
      }
    }
    std::vector<DuckDBRow> keys(rows.size());
    std::vector<char> valid(rows.size(), 1);
    if (key_eval && key_eval->expr_count() > 0) {
      size_t base = 0;
      while (base < rows.size()) {
        const duckdb::idx_t chunk = static_cast<duckdb::idx_t>(
            std::min<size_t>(BatchEvaluator::kBatch, rows.size() - base));
        key_eval->fill(rows.data() + base, chunk);
        std::vector<std::vector<duckdb::Value>> kv(chunk);
        for (size_t k = 0; k < key_eval->expr_count(); k++) {
          duckdb::Vector &v = key_eval->execute(k);
          const auto &type = key_eval->return_type(k);
          for (duckdb::idx_t i = 0; i < chunk; i++) {
            duckdb::Value val = BatchEvaluator::read_result(v, type, i);
            if (val.IsNull() && !null_safe) {
              valid[base + i] = 0;
            }
            kv[i].push_back(std::move(val));
          }
        }
        for (duckdb::idx_t i = 0; i < chunk; i++) {
          keys[base + i].columns.assign(std::move(kv[i]));
        }
        base += chunk;
      }
    }
    // Spilled mode: group contributions per key first, one disk-bucket
    // merge per touched key (updates run pre-views, single-threaded)
    std::unordered_map<RowDigest, SpilledBucketIndex::Bucket, RowDigestHash>
        spill_batch;
    std::string kb, rb; // hoisted: encode_row clears; a fresh pair per row
                        // was two malloc/free per delta row (same fix as
                        // integrate_packed's hoist)
    for (size_t i = 0; i < rows.size(); i++) {
      const DuckDBRow &row = *rows[i];
      const int64_t w = ws[i];
      if (track_weights) {
        int64_t &t = weights[row];
        t += w;
        if (t == 0) {
          weights.erase(row);
        }
      }
      if (track_counters) {
        total += w;
        if (!valid[i]) {
          nulls += w;
        }
      }
      if (!valid[i]) {
        continue;
      }
      if (spilled) {
        std::vector<duckdb::Value> vals;
        vals.reserve(row.columns.size());
        for (size_t c = 0; c < row.columns.size(); c++) {
          vals.push_back(row.columns[c]);
        }
        spill_batch[digest_of_row(keys[i])].emplace_back(std::move(vals), w);
        continue;
      }
      if (packed_ok) {
        if (!packed::encode_row(kb, keys[i]) ||
            !packed::encode_row(rb, row)) {
          throw std::runtime_error("packed arrangement: unencodable row");
        }
        if (bulk) {
          // Streaming initial fill: flat PODs, no bucket maps.
          auto kit = bulk->key_ids.try_emplace(
              kb, static_cast<uint32_t>(bulk->key_bytes.size()));
          if (kit.second) {
            bulk->key_bytes.push_back(kb);
          }
          BulkBuilder::Ent e;
          e.key_id = kit.first->second;
          e.rb_len = static_cast<uint32_t>(rb.size());
          e.rb_off = bulk->rb_arena.size();
          e.w = w;
          bulk->rb_arena.insert(bulk->rb_arena.end(), rb.begin(), rb.end());
          bulk->entries.push_back(e);
          continue;
        }
        auto &bucket = packed[std::move(kb)];
        bool merged = false;
        for (size_t b = 0; b < bucket.size(); b++) {
          if (bucket[b].first == rb) {
            bucket[b].second += w;
            if (bucket[b].second == 0) {
              bucket[b] = std::move(bucket.back());
              bucket.pop_back();
            }
            merged = true;
            break;
          }
        }
        if (!merged) {
          bucket.emplace_back(std::move(rb), w);
        }
        continue;
      }
      auto &bucket = index[keys[i]];
      int64_t &weight = bucket[row];
      weight += w;
      if (weight == 0) {
        bucket.erase(row);
        if (bucket.empty()) {
          index.erase(keys[i]);
        }
      }
    }
    for (auto &[digest, delta_bucket] : spill_batch) {
      spilled->update(digest, delta_bucket);
    }
  }
};

// A join side's request to consume a shared arrangement; collected during
// circuit construction, resolved by CDCManager after sources are tracked
struct ArrangementRequest {
  std::string fingerprint;
  std::string table;
  bool left_side = false;
  bool null_safe = false;
  bool track_weights = false;
  bool track_counters = false;
  // Key expressions REMAPPED into full-table column space (canonical
  // across consumers with different projections); side_types = full
  // table layout
  std::vector<const duckdb::Expression *> key_exprs;
  duckdb::vector<duckdb::LogicalType> side_types;
  // O4: consumer's projection applied to bucket rows at probe time
  // (empty = identity, zero-copy fast path)
  std::vector<duckdb::idx_t> consumer_projection;
  bool project = false; // arrangement-side projection (unused since O4)
  std::vector<duckdb::idx_t> column_idxs;
  // Skip this table's init replay (the arrangement already holds full
  // state). For a both-sides-shared join only ONE side is skipped: the
  // other side's replay ⋈ this arrangement bootstraps the full join.
  bool init_skip = true;
  std::shared_ptr<PlanKeepAlive> keep_alive; // pins exprs for the arrangement
  class PlanJoinNode *node = nullptr;
};

class PlanJoinNode : public dbsp::Node {
public:
  using InputFn = std::function<const DuckDBZSet &()>;

  struct KeyPair {
    std::unique_ptr<RowExprEval> left, right;
  };
  struct Residual {
    std::unique_ptr<RowExprEval> left, right;
    duckdb::ExpressionType cmp;
  };

  PlanJoinNode(dbsp::NodeId id, InputFn left_fn, InputFn right_fn,
               std::vector<KeyPair> keys, std::vector<Residual> residuals,
               duckdb::JoinType join_type = duckdb::JoinType::INNER,
               duckdb::vector<duckdb::LogicalType> left_types = {},
               duckdb::vector<duckdb::LogicalType> right_types = {},
               bool null_safe_keys = false,
               std::shared_ptr<PlanKeepAlive> keep_alive = nullptr,
               std::vector<const duckdb::Expression *> left_key_exprs = {},
               std::vector<const duckdb::Expression *> right_key_exprs = {},
               std::string name = "plan_join")
      : dbsp::Node(id, std::move(name)), left_fn_(std::move(left_fn)),
        right_fn_(std::move(right_fn)), keys_(std::move(keys)),
        residuals_(std::move(residuals)), join_type_(join_type),
        left_types_(std::move(left_types)),
        right_types_(std::move(right_types)),
        null_safe_keys_(null_safe_keys) {
    // H4: whole-delta key extraction runs batched; the per-row KeyPair
    // evals stay for point lookups (match counts, pads, marks)
    keep_alive_join_ = keep_alive;
    if (keep_alive && !left_key_exprs.empty()) {
      batch_left_keys_ = std::make_unique<BatchEvaluator>(
          keep_alive, std::move(left_key_exprs), left_types_);
      batch_right_keys_ = std::make_unique<BatchEvaluator>(
          keep_alive, std::move(right_key_exprs), right_types_);
    }
    pads_left_ = join_type_ == duckdb::JoinType::LEFT ||
                 join_type_ == duckdb::JoinType::OUTER;
    pads_right_ = join_type_ == duckdb::JoinType::RIGHT ||
                  join_type_ == duckdb::JoinType::OUTER;
    marks_ = join_type_ == duckdb::JoinType::MARK;
    // Phase 2d: packed storage for the local indexes (see member block).
    packed_ok_ = !marks_ && !pads_right_ && packed::types_ok(left_types_) &&
                 packed::types_ok(right_types_);
    // N3: under spill mode, local indexes of pure probe-target sides go
    // to disk bucket logs (self-padding / mark-preserved sides keep RAM —
    // their pad/weight reconciliation walks full-row structures). Files
    // open lazily; a side later covered by a shared arrangement simply
    // never writes its local log.
    if (g_spill_mode.load()) {
      if (!(pads_left_ || marks_)) {
        local_spill_left_ = std::make_unique<SpilledBucketIndex>(
            g_spill_dir + "/join_" +
            std::to_string(g_spill_file_seq.fetch_add(1)) + "_l.dbspill");
      }
      if (!pads_right_) {
        local_spill_right_ = std::make_unique<SpilledBucketIndex>(
            g_spill_dir + "/join_" +
            std::to_string(g_spill_file_seq.fetch_add(1)) + "_r.dbspill");
      }
    }
  }

  void step() override {
    output_.clear();
    has_output_ = false;
    const DuckDBZSet &dl = left_fn_();
    const DuckDBZSet &dr = right_fn_();
    if (dl.empty() && dr.empty()) {
      return;
    }

    if (marks_) {
      // MARK join is left-preserving: every left row appears exactly once
      // with a three-valued match mark — no bilinear emission at all
      // Category detection needs the BEFORE counters. A shared right side
      // is already post-delta (updated before views step): derive before =
      // after − this step's contribution. A local right side is pre-delta:
      // read, then integrate.
      bool was_nonempty, had_nulls;
      if (shared_right_) {
        DeltaKeys kr_probe = materialize_keys(dr, /*left=*/false);
        int64_t d_total = 0, d_nulls = 0;
        for (size_t i = 0; i < kr_probe.rows.size(); i++) {
          d_total += kr_probe.weights[i];
          if (!kr_probe.valid[i]) {
            d_nulls += kr_probe.weights[i];
          }
        }
        was_nonempty = (shared_right_->total - d_total) > 0;
        had_nulls = (shared_right_->nulls - d_nulls) > 0;
      } else {
        was_nonempty = right_total_ > 0;
        had_nulls = right_null_ > 0;
        integrate(materialize_keys(dr, /*left=*/false), /*left=*/false);
      }
      if (!shared_left_) {
        integrate(materialize_keys(dl, /*left=*/true), /*left=*/true);
      }
      const bool category_changed =
          was_nonempty != (mark_right_total() > 0) ||
          had_nulls != (mark_right_null() > 0);
      reconcile_marks(dl, dr, category_changed);
      has_output_ = !output_.empty();
      return;
    }

    // Materialize both deltas once with batch-evaluated keys; every pass
    // below (probe passes AND integration) reuses them
    DeltaKeys kl = materialize_keys(dl, /*left=*/true);
    DeltaKeys kr = materialize_keys(dr, /*left=*/false);

    // Δl ⋈ R (local sides: OLD state — integration runs after the
    // passes; a shared side: NEW state — the arrangement was updated
    // before views stepped, and the Δl⋈Δr term is dropped to compensate:
    // Δl⋈R_new + L_old⋈Δr == Δl⋈R_old + L_old⋈Δr + Δl⋈Δr)
    // Intra-operator sharding (L2): pure inner equi-joins (no residuals,
    // no pads, no marks) may split large probe passes across threads —
    // probes are read-only, each shard emits into its own Z-set
    const int shards_cfg = g_intraop_shards.load(std::memory_order_relaxed);
    const bool shardable = shards_cfg > 1 && residuals_.empty() &&
                           !marks_ && !pads_left_ && !pads_right_ &&
                           !packed_ok_ &&
                           kl.rows.size() + kr.rows.size() >= 4096;

    if (shardable) {
      run_sharded_probe(kl, /*probe_left_side=*/false, shards_cfg);
    } else if ((shared_right_ &&
                (shared_right_->is_spilled() || shared_right_->packed_ok ||
                 !shared_proj_right_.empty())) ||
               (!shared_right_ && (local_spill_right_ || packed_ok_))) {
      for (size_t i = 0; i < kl.rows.size(); i++) {
        if (!kl.valid[i]) {
          continue;
        }
        const RowWeights *bucket = probe_side(/*left=*/false, kl.keys[i]);
        if (!bucket) {
          continue;
        }
        for (const auto &[rrow, rw] : *bucket) {
          try_emit(*kl.rows[i], rrow, kl.weights[i] * rw);
        }
      }
    } else {
      // Hot path: hoist the index ref out of the loop (per-row branch
      // resolution cost ~20% of join throughput)
      const Index &right_probe = side_index(/*left=*/false);
      for (size_t i = 0; i < kl.rows.size(); i++) {
        if (!kl.valid[i]) {
          continue;
        }
        auto it = right_probe.find(kl.keys[i]);
        if (it == right_probe.end()) {
          continue;
        }
        for (const auto &[rrow, rw] : it->second) {
          try_emit(*kl.rows[i], rrow, kl.weights[i] * rw);
        }
      }
    }

    // L ⋈ Δr
    if (shardable) {
      run_sharded_probe(kr, /*probe_left_side=*/true, shards_cfg);
    } else if ((shared_left_ &&
                (shared_left_->is_spilled() || shared_left_->packed_ok ||
                 !shared_proj_left_.empty())) ||
               (!shared_left_ && (local_spill_left_ || packed_ok_))) {
      for (size_t i = 0; i < kr.rows.size(); i++) {
        if (!kr.valid[i]) {
          continue;
        }
        const RowWeights *bucket = probe_side(/*left=*/true, kr.keys[i]);
        if (!bucket) {
          continue;
        }
        for (const auto &[lrow, lw] : *bucket) {
          try_emit(lrow, *kr.rows[i], lw * kr.weights[i]);
        }
      }
    } else {
      const Index &left_probe = side_index(/*left=*/true);
      for (size_t i = 0; i < kr.rows.size(); i++) {
        if (!kr.valid[i]) {
          continue;
        }
        auto it = left_probe.find(kr.keys[i]);
        if (it == left_probe.end()) {
          continue;
        }
        for (const auto &[lrow, lw] : it->second) {
          try_emit(lrow, *kr.rows[i], lw * kr.weights[i]);
        }
      }
    }

    // Δl ⋈ Δr (both sides changed in the same step, e.g. self-joins).
    // Shared sides expose POST-delta state, which shifts this term:
    //   no side shared:   Δl⋈R_old + L_old⋈Δr + Δl⋈Δr  → emit it (+)
    //   one side shared:  Δl⋈R_new + L_old⋈Δr           → drop it
    //   both shared:      Δl⋈R_new + L_new⋈Δr − Δl⋈Δr  → emit it (−)
    const bool both_shared = shared_left_ && shared_right_;
    const bool any_shared = shared_left_ || shared_right_;
    if ((!any_shared || both_shared) && !kl.rows.empty() &&
        !kr.rows.empty()) {
      const int64_t sign = both_shared ? -1 : 1;
      Index dr_index;
      for (size_t i = 0; i < kr.rows.size(); i++) {
        if (kr.valid[i]) {
          dr_index[kr.keys[i]][*kr.rows[i]] += kr.weights[i];
        }
      }
      for (size_t i = 0; i < kl.rows.size(); i++) {
        if (!kl.valid[i]) {
          continue;
        }
        auto it = dr_index.find(kl.keys[i]);
        if (it == dr_index.end()) {
          continue;
        }
        for (const auto &[rrow, rw] : it->second) {
          try_emit(*kl.rows[i], rrow, sign * kl.weights[i] * rw);
        }
      }
    }

    flush_emits(); // batched output-hash preseed for everything buffered

    // Integrate deltas into the LOCAL side indexes (shared sides are
    // maintained by the CDC layer)
    if (!shared_left_) {
      integrate(kl, /*left=*/true);
    }
    if (!shared_right_) {
      integrate(kr, /*left=*/false);
    }

    // Outer-join NULL padding: reconcile the pad of every row whose match
    // count may have changed. Desired pad weight = the row's total weight
    // when it has no (residual-passing) matches, else 0; emit the diff.
    if (pads_left_) {
      reconcile_pads(dl, dr, /*left=*/true);
    }
    if (pads_right_) {
      reconcile_pads(dr, dl, /*left=*/false);
    }
    has_output_ = !output_.empty();
  }

  void reset() override {
    left_index_.clear();
    right_index_.clear();
    packed_left_.clear();
    packed_right_.clear();
    packed_wleft_.clear();
    packed_wright_.clear();
    flat_left_.clear();
    flat_right_.clear();
    flat_wleft_.clear();
    flat_wright_.clear();
    if (local_spill_left_) {
      local_spill_left_->discard();
    }
    if (local_spill_right_) {
      local_spill_right_->discard();
    }
    left_weights_.clear();
    right_weights_.clear();
    left_pad_.clear();
    right_pad_.clear();
    mark_state_.clear();
    right_total_ = 0;
    right_null_ = 0;
    output_.clear();
    has_output_ = false;
  }

  bool has_output() const override { return has_output_; }

  const DuckDBZSet &output() const { return output_; }

  void clear_output() override {
    output_.clear();
    has_output_ = false;
  }

  void account_state(StateBytes &out, StateAccounting &acct) const override {
    auto index_bytes = [&](const Index &idx) {
      size_t b = 0;
      for (const auto &[key, rows] : idx) {
        b += acct.row_bytes(key) + 32;
        for (const auto &[row, w] : rows) {
          (void)w;
          b += acct.row_bytes(row) + 32;
        }
      }
      return b;
    };
    out.arrangement += index_bytes(left_index_) + index_bytes(right_index_);
    for (const auto *pidx : {&packed_left_, &packed_right_}) {
      for (const auto &[kb, bucket] : *pidx) {
        out.arrangement += kb.capacity() + 48;
        for (const auto &[rb, weight] : bucket) {
          (void)weight;
          out.arrangement += rb.capacity() + 24;
        }
      }
    }
    for (const auto *pw : {&packed_wleft_, &packed_wright_}) {
      for (const auto &[rb, weight] : *pw) {
        (void)weight;
        out.arrangement += rb.capacity() + 48;
      }
    }
    out.arrangement += flat_left_.resident_bytes() +
                       flat_right_.resident_bytes() +
                       flat_wleft_.resident_bytes() +
                       flat_wright_.resident_bytes();
    for (const auto &[row, entry] : mark_state_) {
      (void)entry;
      out.arrangement += acct.row_bytes(row) + 48;
    }
    out.other += acct.zset_bytes(output_);
  }

  void set_shared_arrangement(bool left,
                              std::shared_ptr<const SharedArrangement> arr,
                              std::vector<duckdb::idx_t> projection = {}) {
    (left ? shared_left_ : shared_right_) = std::move(arr);
    (left ? shared_proj_left_ : shared_proj_right_) = std::move(projection);
  }

private:
  using RowWeights = std::unordered_map<DuckDBRow, int64_t, DuckDBRowHash>;
  using Index = std::unordered_map<DuckDBRow, RowWeights, DuckDBRowHash>;

  const Index &side_index(bool left) const {
    if (left && shared_left_) {
      return shared_left_->index;
    }
    if (!left && shared_right_) {
      return shared_right_->index;
    }
    return left ? left_index_ : right_index_;
  }

  // Probe one side for `key`. Returns nullptr on no match. For a spilled
  // shared side the bucket materializes into this node's scratch (nodes
  // are per-view, so scratches are thread-private under I2 parallel
  // propagation; the arrangement serializes its own cache internally).
  // The pointer is valid until the next probe_side call on that side.
  const RowWeights *probe_side(bool left, const DuckDBRow &key) {
    const auto &sh = left ? shared_left_ : shared_right_;
    const auto &proj = left ? shared_proj_left_ : shared_proj_right_;
    if (sh && sh->is_spilled()) {
      RowWeights &scratch = left ? probe_scratch_left_ : probe_scratch_right_;
      return sh->probe_spilled(key, scratch,
                               proj.empty() ? nullptr : &proj)
                 ? &scratch
                 : nullptr;
    }
    if (sh && sh->packed_ok) {
      RowWeights &scratch = left ? probe_scratch_left_ : probe_scratch_right_;
      return sh->probe_packed(key, scratch, proj.empty() ? nullptr : &proj)
                 ? &scratch
                 : nullptr;
    }
    if (sh && !proj.empty()) {
      // O4: arrangement holds full rows; project to this consumer's shape
      RowWeights &scratch = left ? probe_scratch_left_ : probe_scratch_right_;
      return sh->probe_projected(key, scratch, proj) ? &scratch : nullptr;
    }
    if (!sh) {
      SpilledBucketIndex *spill =
          left ? local_spill_left_.get() : local_spill_right_.get();
      if (spill) {
        RowWeights &scratch =
            left ? probe_scratch_left_ : probe_scratch_right_;
        return probe_local_spill(*spill, key, scratch) ? &scratch : nullptr;
      }
    }
    if (!sh && packed_ok_) {
      // Local packed index — a SHARED side must fall through to
      // side_index() (the arrangement), which the branch above this one
      // only handles for spilled/projected arrangements.
      RowWeights &scratch = left ? probe_scratch_left_ : probe_scratch_right_;
      return probe_packed(left, key, scratch) ? &scratch : nullptr;
    }
    const Index &idx = side_index(left);
    auto it = idx.find(key);
    return it == idx.end() ? nullptr : &it->second;
  }

  static bool probe_local_spill(SpilledBucketIndex &spill,
                                const DuckDBRow &key, RowWeights &out) {
    out.clear();
    const auto *bucket =
        spill.probe(SharedArrangement::digest_of_row(key));
    if (!bucket) {
      return false;
    }
    out.reserve(bucket->size());
    for (const auto &[vals, w] : *bucket) {
      DuckDBRow row;
      std::vector<duckdb::Value> copy = vals;
      const size_t h = hash_row_fast(copy);
      row.columns.assign(std::move(copy));
      row.columns.set_hash(h);
      out.emplace(std::move(row), w);
    }
    return true;
  }

  // Returns false when any key value is NULL (row can never match) —
  // unless keys are null-safe (IS NOT DISTINCT FROM, DELIM joins), where
  // NULL is an ordinary key value (DuckDBRow equality treats NULL == NULL)
  bool eval_key(const DuckDBRow &row, bool left, DuckDBRow &key) {
    key.columns.reserve(keys_.size());
    for (auto &k : keys_) {
      duckdb::Value v = left ? k.left->eval(row) : k.right->eval(row);
      if (v.IsNull() && !null_safe_keys_) {
        return false;
      }
      key.columns.push_back(v);
    }
    return true;
  }

  bool residuals_pass(const DuckDBRow &lrow, const DuckDBRow &rrow) {
    for (auto &res : residuals_) {
      duckdb::Value lv = res.left->eval(lrow);
      duckdb::Value rv = res.right->eval(rrow);
      if (lv.IsNull() || rv.IsNull()) {
        return false;
      }
      bool pass = false;
      switch (res.cmp) {
      case duckdb::ExpressionType::COMPARE_GREATERTHAN:
        pass = rv < lv;
        break;
      case duckdb::ExpressionType::COMPARE_LESSTHAN:
        pass = lv < rv;
        break;
      case duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO:
        pass = !(lv < rv);
        break;
      case duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO:
        pass = !(rv < lv);
        break;
      case duckdb::ExpressionType::COMPARE_NOTEQUAL:
        pass = lv != rv;
        break;
      case duckdb::ExpressionType::COMPARE_EQUAL:
        pass = lv == rv;
        break;
      default:
        return false;
      }
      if (!pass) {
        return false;
      }
    }
    return true;
  }

  void try_emit(const DuckDBRow &lrow, const DuckDBRow &rrow,
                int64_t weight) {
    if (weight == 0 || !residuals_pass(lrow, rrow)) {
      return;
    }
    std::vector<duckdb::Value> vals;
    vals.reserve(lrow.columns.size() + rrow.columns.size());
    vals.insert(vals.end(), lrow.columns.begin(), lrow.columns.end());
    vals.insert(vals.end(), rrow.columns.begin(), rrow.columns.end());
    DuckDBRow combined;
    combined.columns.assign(std::move(vals));
    // buffer for a batched, vectorized output-hash preseed (flush_emits);
    // inserting here would pay the lazy per-Value hash per match
    pending_rows_.push_back(std::move(combined));
    pending_weights_.push_back(weight);
    if (pending_rows_.size() == BatchEvaluator::kBatch) {
      flush_emits();
    }
  }

  // DP3a for join outputs: fill a typed chunk from the buffered concat
  // rows, fold per-column hash vectors (exact lazy-formula replication,
  // same test_row_hash contract), pre-seed, insert. Serial path only —
  // sharded probes emit into shard-local Z-sets elsewhere.
  void flush_emits() {
    if (pending_rows_.empty()) {
      return;
    }
    if (!keep_alive_join_) {
      // legacy construction without a keep-alive: no evaluator possible,
      // insert with the lazy hash path
      for (size_t i = 0; i < pending_rows_.size(); i++) {
        output_.insert(std::move(pending_rows_[i]), pending_weights_[i]);
      }
      pending_rows_.clear();
      pending_weights_.clear();
      return;
    }
    if (!out_eval_) {
      duckdb::vector<duckdb::LogicalType> types;
      for (const auto &t : left_types_) {
        types.push_back(t);
      }
      for (const auto &t : right_types_) {
        types.push_back(t);
      }
      out_eval_ = std::make_unique<BatchEvaluator>(
          keep_alive_join_, std::vector<const duckdb::Expression *>{},
          std::move(types));
    }
    const duckdb::idx_t n = pending_rows_.size();
    std::vector<const DuckDBRow *> ptrs(n);
    for (duckdb::idx_t i = 0; i < n; i++) {
      ptrs[i] = &pending_rows_[i];
    }
    out_eval_->fill(ptrs.data(), n);
    auto &chunk = out_eval_->input_chunk();
    std::vector<size_t> hashes(n, 0);
    duckdb::Vector hash_scratch(duckdb::LogicalType::HASH);
    for (duckdb::idx_t c = 0; c < chunk.ColumnCount(); c++) {
      fold_vector_hashes(chunk.data[c], n, hash_scratch, hashes);
    }
    for (duckdb::idx_t i = 0; i < n; i++) {
      pending_rows_[i].columns.set_hash(hashes[i]);
      output_.insert(std::move(pending_rows_[i]), pending_weights_[i]);
    }
    pending_rows_.clear();
    pending_weights_.clear();
  }

  // One side's delta with batch-evaluated keys. valid[i] == false means a
  // NULL key column under non-null-safe semantics (row can never match);
  // with null-safe keys every row is valid and NULLs are key values.
  struct DeltaKeys {
    std::vector<const DuckDBRow *> rows;
    std::vector<int64_t> weights;
    std::vector<DuckDBRow> keys;
    std::vector<char> valid;
  };

  // Range-split one probe pass across shards. `dk` is the delta being
  // probed; probe_left_side is the side whose index gets probed (the
  // OPPOSITE of dk's side). Only called for residual-free inner joins:
  // the emit is a pure concat, and shard-local scratches make spilled
  // probes thread-safe (the arrangement's cache has its own mutex).
  void run_sharded_probe(const DeltaKeys &dk, bool probe_left_side,
                         int shards_cfg) {
    const size_t n = dk.rows.size();
    if (n == 0) {
      return;
    }
    const size_t shards =
        std::min<size_t>(static_cast<size_t>(shards_cfg), (n + 511) / 512);
    if (shards <= 1) {
      for (size_t i = 0; i < n; i++) {
        probe_one(dk, i, probe_left_side, output_, nullptr);
      }
      return;
    }
    std::vector<DuckDBZSet> outs(shards);
    std::vector<std::thread> threads;
    threads.reserve(shards);
    const size_t chunk = (n + shards - 1) / shards;
    for (size_t t = 0; t < shards; t++) {
      threads.emplace_back([&, t]() {
        RowWeights scratch;
        const size_t lo = t * chunk;
        const size_t hi = std::min(n, lo + chunk);
        for (size_t i = lo; i < hi; i++) {
          probe_one(dk, i, probe_left_side, outs[t], &scratch);
        }
      });
    }
    for (auto &th : threads) {
      th.join();
    }
    for (auto &out : outs) {
      for (const auto &[row, w] : out) {
        output_.insert(row, w);
      }
    }
  }

  void probe_one(const DeltaKeys &dk, size_t i, bool probe_left_side,
                 DuckDBZSet &out, RowWeights *scratch) {
    if (!dk.valid[i]) {
      return;
    }
    const RowWeights *bucket;
    const auto &sh = probe_left_side ? shared_left_ : shared_right_;
    const auto &proj =
        probe_left_side ? shared_proj_left_ : shared_proj_right_;
    SpilledBucketIndex *lspill =
        probe_left_side ? local_spill_left_.get() : local_spill_right_.get();
    if (sh && sh->is_spilled() && scratch) {
      bucket = sh->probe_spilled(dk.keys[i], *scratch,
                                 proj.empty() ? nullptr : &proj)
                   ? scratch
                   : nullptr;
    } else if (sh && !proj.empty() && scratch) {
      bucket = sh->probe_projected(dk.keys[i], *scratch, proj) ? scratch
                                                               : nullptr;
    } else if (!sh && lspill && scratch) {
      // Local spilled index: LRU cache is node-private but shared across
      // shard threads — serialize via the node's spill probe mutex
      std::lock_guard<std::mutex> g(local_spill_mutex_);
      bucket =
          probe_local_spill(*lspill, dk.keys[i], *scratch) ? scratch : nullptr;
    } else {
      bucket = probe_side(probe_left_side, dk.keys[i]);
    }
    if (!bucket) {
      return;
    }
    for (const auto &[orow, ow] : *bucket) {
      const int64_t w = dk.weights[i] * ow;
      if (w == 0) {
        continue;
      }
      std::vector<duckdb::Value> vals;
      const DuckDBRow &lrow = probe_left_side ? orow : *dk.rows[i];
      const DuckDBRow &rrow = probe_left_side ? *dk.rows[i] : orow;
      vals.reserve(lrow.columns.size() + rrow.columns.size());
      vals.insert(vals.end(), lrow.columns.begin(), lrow.columns.end());
      vals.insert(vals.end(), rrow.columns.begin(), rrow.columns.end());
      DuckDBRow combined;
      combined.columns.assign(std::move(vals));
      out.insert(std::move(combined), w);
    }
  }

  DeltaKeys materialize_keys(const DuckDBZSet &delta, bool left) {
    DeltaKeys out;
    const size_t n = delta.size();
    out.rows.reserve(n);
    out.weights.reserve(n);
    out.keys.resize(n);
    out.valid.assign(n, 1);
    for (const auto &[row, w] : delta) {
      out.rows.push_back(&row);
      out.weights.push_back(w);
    }
    if (keys_.empty()) {
      return out; // keyless join: single empty key, all valid
    }
    BatchEvaluator *be =
        left ? batch_left_keys_.get() : batch_right_keys_.get();
    if (!be) {
      // No batch evaluators wired (unit-constructed node): per-row path
      for (size_t i = 0; i < out.rows.size(); i++) {
        DuckDBRow key;
        if (eval_key(*out.rows[i], left, key)) {
          out.keys[i] = std::move(key);
        } else {
          out.valid[i] = 0;
        }
      }
      return out;
    }
    size_t base = 0;
    while (base < out.rows.size()) {
      const duckdb::idx_t chunk = static_cast<duckdb::idx_t>(
          std::min<size_t>(BatchEvaluator::kBatch, out.rows.size() - base));
      be->fill(out.rows.data() + base, chunk);
      std::vector<std::vector<duckdb::Value>> key_vals(chunk);
      for (auto &kv : key_vals) {
        kv.reserve(be->expr_count());
      }
      for (size_t k = 0; k < be->expr_count(); k++) {
        duckdb::Vector &v = be->execute(k);
        const auto &type = be->return_type(k);
        for (duckdb::idx_t i = 0; i < chunk; i++) {
          duckdb::Value val = BatchEvaluator::read_result(v, type, i);
          if (val.IsNull() && !null_safe_keys_) {
            out.valid[base + i] = 0;
          }
          key_vals[i].push_back(std::move(val));
        }
      }
      for (duckdb::idx_t i = 0; i < chunk; i++) {
        out.keys[base + i].columns.assign(std::move(key_vals[i]));
      }
      base += chunk;
    }
    return out;
  }

  void integrate(const DeltaKeys &dk, bool left) {
    if (packed_ok_ && !(left ? local_spill_left_ : local_spill_right_)) {
      integrate_packed(dk, left);
      return;
    }
    Index &index = left ? left_index_ : right_index_;
    const bool track_weights = left ? (pads_left_ || marks_) : pads_right_;
    RowWeights &weights = left ? left_weights_ : right_weights_;
    SpilledBucketIndex *spill =
        left ? local_spill_left_.get() : local_spill_right_.get();
    std::unordered_map<RowDigest, SpilledBucketIndex::Bucket, RowDigestHash>
        spill_batch;
    for (size_t i = 0; i < dk.rows.size(); i++) {
      const DuckDBRow &row = *dk.rows[i];
      const int64_t w = dk.weights[i];
      if (track_weights) {
        int64_t &total = weights[row];
        total += w;
        if (total == 0) {
          weights.erase(row);
        }
      }
      if (!dk.valid[i]) {
        if (!left && marks_) {
          right_total_ += w;
          right_null_ += w; // NULL key on the subquery side
        }
        continue;
      }
      if (!left && marks_) {
        right_total_ += w;
      }
      if (spill) {
        std::vector<duckdb::Value> vals;
        vals.reserve(row.columns.size());
        for (size_t c = 0; c < row.columns.size(); c++) {
          vals.push_back(row.columns[c]);
        }
        spill_batch[SharedArrangement::digest_of_row(dk.keys[i])]
            .emplace_back(std::move(vals), w);
        continue;
      }
      auto &rows = index[dk.keys[i]];
      int64_t &weight = rows[row];
      weight += w;
      if (weight == 0) {
        rows.erase(row);
        if (rows.empty()) {
          index.erase(dk.keys[i]);
        }
      }
    }
    for (auto &[digest, delta_bucket] : spill_batch) {
      spill->update(digest, delta_bucket);
    }
  }

  // Weighted count of residual-passing matches for one row of the padded
  // side against the other side's integrated index. NULL keys never match.
  int64_t match_count(const DuckDBRow &row, bool left) {
    DuckDBRow key;
    if (!eval_key(row, left, key)) {
      return 0;
    }
    const RowWeights *bucket = probe_side(!left, key);
    if (!bucket) {
      return 0;
    }
    int64_t count = 0;
    for (const auto &[orow, ow] : *bucket) {
      const bool pass = left ? residuals_pass(row, orow)
                             : residuals_pass(orow, row);
      if (pass) {
        count += ow;
      }
    }
    return count;
  }

  DuckDBRow pad_row(const DuckDBRow &row, bool left) const {
    DuckDBRow out;
    if (left) {
      out.columns = row.columns;
      for (const auto &t : right_types_) {
        out.columns.push_back(duckdb::Value(t));
      }
    } else {
      out.columns.reserve(left_types_.size() + row.columns.size());
      for (const auto &t : left_types_) {
        out.columns.push_back(duckdb::Value(t));
      }
      out.columns.insert(out.columns.end(), row.columns.begin(),
                         row.columns.end());
    }
    return out;
  }

  // Reconcile pads for every row whose match count may have changed:
  // rows in this side's delta, plus integrated rows sharing an equi key
  // with the other side's delta (keyless joins share one empty key, so a
  // non-empty other-side delta touches every row — correct, if not cheap).
  void reconcile_pads(const DuckDBZSet &own_delta,
                      const DuckDBZSet &other_delta, bool left) {
    std::unordered_map<DuckDBRow, char, DuckDBRowHash> affected;
    for (const auto &[row, w] : own_delta) {
      affected.emplace(row, 0);
    }
    if (!other_delta.empty()) {
      std::unordered_map<DuckDBRow, char, DuckDBRowHash> seen_keys;
      for (const auto &[orow, ow] : other_delta) {
        DuckDBRow key;
        if (!eval_key(orow, !left, key)) {
          continue;
        }
        if (!seen_keys.emplace(key, 0).second) {
          continue;
        }
        // probe_side covers all storage modes (shared / spilled / packed /
        // boxed); the scratch is copied into `affected` immediately.
        const RowWeights *bucket = probe_side(left, key);
        if (!bucket) {
          continue;
        }
        for (const auto &[row, w] : *bucket) {
          affected.emplace(row, 0);
        }
      }
    }

    RowWeights &pads = left ? left_pad_ : right_pad_;
    for (const auto &[row, unused] : affected) {
      const int64_t total = own_row_weight(row, left);
      const int64_t desired =
          (total > 0 && match_count(row, left) == 0) ? total : 0;
      auto pit = pads.find(row);
      const int64_t current = pit == pads.end() ? 0 : pit->second;
      if (desired == current) {
        continue;
      }
      output_.insert(pad_row(row, left), desired - current);
      if (desired == 0) {
        pads.erase(row);
      } else {
        pads[row] = desired;
      }
    }
  }

  // Three-valued mark per SQL IN semantics: TRUE when a residual-passing
  // match exists; FALSE when the subquery side is empty (even for NULL
  // probes); otherwise NULL when the probe key is NULL or the subquery
  // side contains a NULL key; else FALSE.
  int64_t mark_right_total() const {
    return shared_right_ ? shared_right_->total : right_total_;
  }
  int64_t mark_right_null() const {
    return shared_right_ ? shared_right_->nulls : right_null_;
  }

  duckdb::Value mark_value(const DuckDBRow &row) {
    if (match_count(row, /*left=*/true) > 0) {
      return duckdb::Value::BOOLEAN(true);
    }
    if (mark_right_total() <= 0) {
      return duckdb::Value::BOOLEAN(false);
    }
    DuckDBRow key;
    if (!eval_key(row, /*left=*/true, key) || mark_right_null() > 0) {
      return duckdb::Value(duckdb::LogicalType::BOOLEAN);
    }
    return duckdb::Value::BOOLEAN(false);
  }

  void reconcile_marks(const DuckDBZSet &dl, const DuckDBZSet &dr,
                       bool category_changed) {
    std::unordered_map<DuckDBRow, char, DuckDBRowHash> affected;
    if (category_changed) {
      // Emptiness or NULL-presence flipped: every unmatched mark changes
      for (const auto &[row, w] :
           (shared_left_ ? shared_left_->weights : left_weights_)) {
        affected.emplace(row, 0);
      }
      for (const auto &[row, entry] : mark_state_) {
        affected.emplace(row, 0); // rows whose weight just went to 0
      }
    } else {
      for (const auto &[row, w] : dl) {
        affected.emplace(row, 0);
      }
      if (!dr.empty()) {
        std::unordered_map<DuckDBRow, char, DuckDBRowHash> seen_keys;
        for (const auto &[orow, ow] : dr) {
          DuckDBRow key;
          if (!eval_key(orow, /*left=*/false, key)) {
            continue;
          }
          if (!seen_keys.emplace(key, 0).second) {
            continue;
          }
          const Index &lidx = side_index(/*left=*/true);
          auto it = lidx.find(key);
          if (it == lidx.end()) {
            continue;
          }
          for (const auto &[row, w] : it->second) {
            affected.emplace(row, 0);
          }
        }
      }
    }

    const RowWeights &lw =
        shared_left_ ? shared_left_->weights : left_weights_;
    for (const auto &[row, unused] : affected) {
      auto wit = lw.find(row);
      const int64_t weight = wit == lw.end() ? 0 : wit->second;
      duckdb::Value mark =
          weight > 0 ? mark_value(row) : duckdb::Value::BOOLEAN(false);

      auto sit = mark_state_.find(row);
      if (sit != mark_state_.end()) {
        const auto &[old_mark, old_w] = sit->second;
        if (weight > 0 && old_w == weight &&
            old_mark.IsNull() == mark.IsNull() &&
            (old_mark.IsNull() ||
             old_mark.GetValue<bool>() == mark.GetValue<bool>())) {
          continue; // unchanged
        }
        DuckDBRow old_row = row;
        old_row.columns.push_back(old_mark);
        output_.insert(old_row, -old_w);
        mark_state_.erase(sit);
      }
      if (weight > 0) {
        DuckDBRow new_row = row;
        new_row.columns.push_back(mark);
        output_.insert(new_row, weight);
        mark_state_.emplace(row, std::make_pair(mark, weight));
      }
    }
  }

  InputFn left_fn_, right_fn_;
  std::vector<KeyPair> keys_;
  std::vector<Residual> residuals_;
public:
  // Checkpointing (D3b, extended Task 3): INNER/LEFT/RIGHT joins serialize
  // their equi-key indexes (private ones for non-shared sides; shared
  // arrangements are checkpointed by the CDC layer) plus, for LEFT/RIGHT,
  // the pad bookkeeping reconcile_pads needs to resume correctly (see
  // serialize_state). FULL (OUTER) and MARK joins carry bookkeeping this
  // pass does not cover, and spilled indexes live on disk — both stay
  // UNSUPPORTED (rebuild-by-replay).
  StateKind state_kind() const override {
    if (marks_ || local_spill_left_ || local_spill_right_) {
      return StateKind::UNSUPPORTED;
    }
    if (join_type_ == duckdb::JoinType::INNER ||
        join_type_ == duckdb::JoinType::LEFT ||
        join_type_ == duckdb::JoinType::RIGHT) {
      return StateKind::SERIALIZABLE;
    }
    return StateKind::UNSUPPORTED; // FULL (OUTER) — both sides pad, unbuilt
  }

  // reconcile_pads (see above) reads exactly four pieces of per-node state
  // to decide each row's desired pad weight and diff it against what was
  // last emitted: the padded side's own-row weight totals (left_weights_ /
  // right_weights_, already serialized for INNER), the other side's
  // equi-key index to recompute match_count (left_index_ / right_index_,
  // already serialized), and — new in Task 3 — the currently-emitted pad
  // weight per row (left_pad_ / right_pad_) so a post-restore delta emits
  // the correct diff instead of re-emitting a pad that was already output
  // pre-checkpoint. right_total_/right_null_ are MARK-only bookkeeping
  // (only ever written when marks_, which stays UNSUPPORTED) and are
  // therefore NOT read by reconcile_pads — correctly omitted here.
  static constexpr uint64_t kPackedStateMagic = 0xDB5B2DFACC0FFEE1ULL;

  void serialize_state(std::vector<uint8_t> &out) const override {
    BlobWriter w;
    if (packed_ok_) {
      // Packed-native layout (magic-tagged; an old boxed blob starts with a
      // small map size and can never collide). Pads stay boxed rows.
      w.u64(kPackedStateMagic);
      // Fold flat (restored) + overlay (post-restore deltas) into one
      // stream. With no flat layer this is exactly the old map dump; with
      // a clean overlay it is a straight flat re-emit.
      auto write_pindex = [&w](const PackedIndex &idx,
                               const flatpacked::FlatPackedIndex &flat) {
        if (flat.empty()) {
          w.u64(idx.size());
          for (const auto &[kb, bucket] : idx) {
            w.bytes(reinterpret_cast<const uint8_t *>(kb.data()), kb.size());
            w.u64(bucket.size());
            for (const auto &[rb, weight] : bucket) {
              w.bytes(reinterpret_cast<const uint8_t *>(rb.data()),
                      rb.size());
              w.i64(weight);
            }
          }
          return;
        }
        // Merged key set: every flat key + overlay-only keys. Buckets
        // merge by row bytes with weights summed; zero-weight rows and
        // empty buckets are dropped.
        std::vector<std::pair<std::string, PackedBucket>> merged_extra;
        std::vector<const std::pair<const std::string, PackedBucket> *>
            overlay_only;
        for (const auto &kv : idx) {
          if (flat.find(kv.first) == nullptr) {
            overlay_only.push_back(&kv);
          }
        }
        // First pass counts live keys. All reads go through the
        // mapped-aware accessors (dir_size/dir_at/bucket_at/arena_data):
        // the owned `dir` vector is EMPTY for an mmap-adopted sidecar, and
        // reading it directly would silently serialize an empty index.
        uint64_t live = 0;
        const uint8_t *arena_base = flat.arena_data();
        std::vector<PackedBucket> flat_merged(flat.dir_size());
        for (size_t i = 0; i < flat.dir_size(); i++) {
          const auto &de = flat.dir_at(i);
          const std::string kb(
              reinterpret_cast<const char *>(arena_base + de.key_off),
              de.key_len);
          PackedBucket &bucket = flat_merged[i];
          auto ov = idx.find(kb);
          if (ov == idx.end()) {
            bucket.reserve(de.bucket_n);
            for (uint32_t b = 0; b < de.bucket_n; b++) {
              const auto &be = flat.bucket_at(de.bucket_off + b);
              bucket.emplace_back(
                  std::string(
                      reinterpret_cast<const char *>(arena_base + be.row_off),
                      be.row_len),
                  be.weight);
            }
          } else {
            std::unordered_map<std::string, int64_t> m;
            for (uint32_t b = 0; b < de.bucket_n; b++) {
              const auto &be = flat.bucket_at(de.bucket_off + b);
              m.emplace(std::string(reinterpret_cast<const char *>(
                                        arena_base + be.row_off),
                                    be.row_len),
                        be.weight);
            }
            for (const auto &[rb, dw] : ov->second) {
              m[rb] += dw;
            }
            for (auto &[rb, wt] : m) {
              if (wt != 0) {
                bucket.emplace_back(rb, wt);
              }
            }
          }
          if (!bucket.empty()) {
            live++;
          }
        }
        for (const auto *kv : overlay_only) {
          if (!kv->second.empty()) {
            live++;
          }
        }
        w.u64(live);
        for (size_t i = 0; i < flat.dir_size(); i++) {
          if (flat_merged[i].empty()) {
            continue;
          }
          const auto &de = flat.dir_at(i);
          w.bytes(arena_base + de.key_off, de.key_len);
          w.u64(flat_merged[i].size());
          for (const auto &[rb, weight] : flat_merged[i]) {
            w.bytes(reinterpret_cast<const uint8_t *>(rb.data()), rb.size());
            w.i64(weight);
          }
        }
        for (const auto *kv : overlay_only) {
          if (kv->second.empty()) {
            continue;
          }
          w.bytes(reinterpret_cast<const uint8_t *>(kv->first.data()),
                  kv->first.size());
          w.u64(kv->second.size());
          for (const auto &[rb, weight] : kv->second) {
            w.bytes(reinterpret_cast<const uint8_t *>(rb.data()), rb.size());
            w.i64(weight);
          }
        }
        (void)merged_extra;
      };
      auto write_pweights =
          [&w](const std::unordered_map<std::string, int64_t> &wm,
               const flatpacked::FlatPackedWeights &flat) {
            if (flat.empty()) {
              w.u64(wm.size());
              for (const auto &[rb, weight] : wm) {
                w.bytes(reinterpret_cast<const uint8_t *>(rb.data()),
                        rb.size());
                w.i64(weight);
              }
              return;
            }
            uint64_t live = 0;
            for (const auto &we : flat.dir) {
              const std::string rb(
                  reinterpret_cast<const char *>(flat.arena.data() +
                                                 we.row_off),
                  we.row_len);
              auto ov = wm.find(rb);
              const int64_t total =
                  we.weight + (ov == wm.end() ? 0 : ov->second);
              if (total != 0) {
                live++;
              }
            }
            for (const auto &[rb, dw] : wm) {
              if (flat.find(rb) == 0 && dw != 0) {
                // overlay-only row (flat.find returns 0 for absent —
                // a flat row with true weight 0 cannot exist)
                live++;
              }
            }
            w.u64(live);
            for (const auto &we : flat.dir) {
              const std::string rb(
                  reinterpret_cast<const char *>(flat.arena.data() +
                                                 we.row_off),
                  we.row_len);
              auto ov = wm.find(rb);
              const int64_t total =
                  we.weight + (ov == wm.end() ? 0 : ov->second);
              if (total == 0) {
                continue;
              }
              w.bytes(reinterpret_cast<const uint8_t *>(rb.data()),
                      rb.size());
              w.i64(total);
            }
            for (const auto &[rb, dw] : wm) {
              if (flat.find(rb) == 0 && dw != 0) {
                w.bytes(reinterpret_cast<const uint8_t *>(rb.data()),
                        rb.size());
                w.i64(dw);
              }
            }
          };
      auto write_boxed = [&w](const RowWeights &rw) {
        w.u64(rw.size());
        for (const auto &[row, weight] : rw) {
          w.row(row.columns);
          w.i64(weight);
        }
      };
      write_pindex(packed_left_, flat_left_);
      write_pindex(packed_right_, flat_right_);
      write_pweights(packed_wleft_, flat_wleft_);
      write_pweights(packed_wright_, flat_wright_);
      write_boxed(left_pad_);
      write_boxed(right_pad_);
      out = w.take();
      return;
    }
    auto write_weights = [&w](const RowWeights &rw) {
      w.u64(rw.size());
      for (const auto &[row, weight] : rw) {
        w.row(row.columns);
        w.i64(weight);
      }
    };
    auto write_index = [&](const Index &idx) {
      w.u64(idx.size());
      for (const auto &[key, bucket] : idx) {
        w.row(key.columns);
        write_weights(bucket);
      }
    };
    write_index(left_index_);
    write_index(right_index_);
    write_weights(left_weights_);
    write_weights(right_weights_);
    write_weights(left_pad_);
    write_weights(right_pad_);
    out = w.take();
  }

  bool restore_state(const uint8_t *data, size_t len) override {
    try {
      BlobReader r(data, len);
      if (packed_ok_) {
        if (r.u64() != kPackedStateMagic) {
          return false; // boxed-era blob: rebuild by replay
        }
        // Tier 2: decode into the contiguous flat layer (append + one
        // sort), NOT the hash maps — the 36M-insert map build was ~all of
        // the first-edit-after-reopen cost. The maps stay empty and serve
        // as the mutation overlay from here on.
        auto read_flat_index = [&r](PackedIndex &overlay,
                                    flatpacked::FlatPackedIndex &flat) {
          overlay.clear();
          flat.clear();
          const uint64_t n = r.u64();
          flat.dir.reserve(n);
          for (uint64_t i = 0; i < n; i++) {
            std::string kb = r.byte_string();
            flatpacked::DirEnt de;
            de.key_off = flat.append_bytes(kb);
            de.key_len = static_cast<uint32_t>(kb.size());
            de.bucket_off = flat.buckets.size();
            const uint64_t m = r.u64();
            de.bucket_n = static_cast<uint32_t>(m);
            for (uint64_t j = 0; j < m; j++) {
              std::string rb = r.byte_string();
              flatpacked::BucketEnt be;
              be.row_off = flat.append_bytes(rb);
              be.row_len = static_cast<uint32_t>(rb.size());
              be.weight = r.i64();
              flat.buckets.push_back(be);
            }
            flat.dir.push_back(de);
          }
          flat.finish_build();
        };
        auto read_flat_weights = [&r](std::unordered_map<std::string, int64_t>
                                          &overlay,
                                      flatpacked::FlatPackedWeights &flat) {
          overlay.clear();
          flat.clear();
          const uint64_t n = r.u64();
          flat.dir.reserve(n);
          for (uint64_t i = 0; i < n; i++) {
            std::string rb = r.byte_string();
            flatpacked::WeightEnt we;
            we.row_off = flat.arena.size();
            flat.arena.insert(flat.arena.end(), rb.begin(), rb.end());
            we.row_len = static_cast<uint32_t>(rb.size());
            we.weight = r.i64();
            flat.dir.push_back(we);
          }
          flat.finish_build();
        };
        auto read_boxed = [&r](RowWeights &rw) {
          rw.clear();
          const uint64_t n = r.u64();
          for (uint64_t i = 0; i < n; i++) {
            DuckDBRow row = r.hashed_row();
            const int64_t weight = r.i64();
            rw.emplace(std::move(row), weight);
          }
        };
        read_flat_index(packed_left_, flat_left_);
        read_flat_index(packed_right_, flat_right_);
        read_flat_weights(packed_wleft_, flat_wleft_);
        read_flat_weights(packed_wright_, flat_wright_);
        read_boxed(left_pad_);
        read_boxed(right_pad_);
        return r.done();
      }
      auto read_weights = [&r](RowWeights &rw) {
        rw.clear();
        const uint64_t n = r.u64();
        for (uint64_t i = 0; i < n; i++) {
          DuckDBRow row = r.hashed_row();
          const int64_t weight = r.i64();
          rw.emplace(std::move(row), weight);
        }
      };
      auto read_index = [&](Index &idx) {
        idx.clear();
        const uint64_t n = r.u64();
        for (uint64_t i = 0; i < n; i++) {
          DuckDBRow key = r.hashed_row();
          RowWeights bucket;
          read_weights(bucket);
          idx.emplace(std::move(key), std::move(bucket));
        }
      };
      read_index(left_index_);
      read_index(right_index_);
      read_weights(left_weights_);
      read_weights(right_weights_);
      read_weights(left_pad_);
      read_weights(right_pad_);
      return r.done();
    } catch (...) {
      return false;
    }
  }

private:
  // ---- Phase 2d: packed join-index storage --------------------------------
  // Boxed DuckDBRow index entries cost ~450-600B resident; packed byte rows
  // cost tens. Active (packed_ok_) for INNER/LEFT-pad joins whose side
  // types are all codec-supported; buckets decode into the probe scratch on
  // access — the same materialize-into-scratch pattern the spilled and
  // projected shared paths already use. MARK and right-padding joins stay
  // boxed (their reconcile paths read row objects pervasively and are not
  // emitted by the compiled plans this exists for).
  using PackedBucket = std::vector<std::pair<std::string, int64_t>>;
  using PackedIndex = std::unordered_map<std::string, PackedBucket>;
  PackedIndex packed_left_, packed_right_;
  std::unordered_map<std::string, int64_t> packed_wleft_, packed_wright_;
  bool packed_ok_ = false;
  // Tier-2 restore layer: checkpoint blobs decode into these contiguous,
  // key-sorted structures (append + one sort — no 36M-insert hash-map
  // build). Immutable; the packed_* maps above act as a DELTA overlay on
  // top (weights sum across layers). serialize_state folds both layers
  // back into one blob stream.
  flatpacked::FlatPackedIndex flat_left_, flat_right_;
  flatpacked::FlatPackedWeights flat_wleft_, flat_wright_;

  flatpacked::FlatPackedIndex &flat_index(bool left) {
    return left ? flat_left_ : flat_right_;
  }
  flatpacked::FlatPackedWeights &flat_weights(bool left) {
    return left ? flat_wleft_ : flat_wright_;
  }

  PackedIndex &packed_index(bool left) {
    return left ? packed_left_ : packed_right_;
  }

  std::unordered_map<std::string, int64_t> &packed_weights(bool left) {
    return left ? packed_wleft_ : packed_wright_;
  }

  void integrate_packed(const DeltaKeys &dk, bool left) {
    const bool track_weights = left && pads_left_; // marks/right-pads boxed
    auto &wmap = packed_weights(left);
    auto &idx = packed_index(left);
    std::string kb, rb;
    // Small deltas (the steady edit path: 1-few rows per commit) keep the
    // original per-row merge — the grouped path below costs a map build +
    // an extra string copy per row, measured at ~+19ms on a wfp steady
    // edit when applied unconditionally.
    if (dk.rows.size() <= 16) {
      for (size_t i = 0; i < dk.rows.size(); i++) {
        const DuckDBRow &row = *dk.rows[i];
        const int64_t w = dk.weights[i];
        if (!packed::encode_row(rb, row)) {
          throw std::runtime_error("packed join index: unencodable row");
        }
        if (track_weights) {
          int64_t &total = wmap[rb];
          total += w;
          if (total == 0) {
            wmap.erase(rb);
          }
        }
        if (!dk.valid[i]) {
          continue;
        }
        if (!packed::encode_row(kb, dk.keys[i])) {
          throw std::runtime_error("packed join index: unencodable key");
        }
        auto &bucket = idx[kb];
        bool merged = false;
        for (size_t b = 0; b < bucket.size(); b++) {
          if (bucket[b].first == rb) {
            bucket[b].second += w;
            if (bucket[b].second == 0) {
              bucket[b] = std::move(bucket.back());
              bucket.pop_back();
            }
            merged = true;
            break;
          }
        }
        if (!merged) {
          bucket.emplace_back(rb, w);
        }
        if (bucket.empty()) {
          idx.erase(kb);
        }
      }
      return;
    }
    // Bulk deltas (initial replay chunks): group per encoded key so each
    // bucket is merged once per CALL, not scanned once per row — the
    // per-row linear bucket probe made initial replay O(rows x bucket) on
    // fat buckets (low-cardinality keys, degenerate cross-join single
    // bucket), profiled at ~25% of a wfp cold attach.
    std::unordered_map<std::string,
                       std::vector<std::pair<std::string, int64_t>>>
        grouped;
    for (size_t i = 0; i < dk.rows.size(); i++) {
      const DuckDBRow &row = *dk.rows[i];
      const int64_t w = dk.weights[i];
      if (!packed::encode_row(rb, row)) {
        throw std::runtime_error("packed join index: unencodable row");
      }
      if (track_weights) {
        int64_t &total = wmap[rb];
        total += w;
        if (total == 0) {
          wmap.erase(rb);
        }
      }
      if (!dk.valid[i]) {
        continue;
      }
      if (!packed::encode_row(kb, dk.keys[i])) {
        throw std::runtime_error("packed join index: unencodable key");
      }
      // static: getenv scans environ (with a lock in some libcs) — this sat
      // inside the per-row integrate loop.
      static const bool packed_debug = std::getenv("DBSP_PACKED_DEBUG") != nullptr;
      if (packed_debug) {
        fprintf(stderr, "[packed] integrate side=%s klen=%zu k0=%02x%02x%02x w=%lld\n",
                left ? "L" : "R", kb.size(), (unsigned char)kb[0],
                (unsigned char)kb[4], kb.size() > 5 ? (unsigned char)kb[5] : 0,
                (long long)w);
      }
      grouped[kb].emplace_back(rb, w);
    }

    for (auto &[key, adds] : grouped) {
      auto &bucket = idx[key];
      if (bucket.size() + adds.size() <= 16) {
        // Small: the linear merge is cheaper than building a map.
        for (auto &[arb, aw] : adds) {
          bool merged = false;
          for (size_t b = 0; b < bucket.size(); b++) {
            if (bucket[b].first == arb) {
              bucket[b].second += aw;
              if (bucket[b].second == 0) {
                bucket[b] = std::move(bucket.back());
                bucket.pop_back();
              }
              merged = true;
              break;
            }
          }
          if (!merged) {
            bucket.emplace_back(std::move(arb), aw);
          }
        }
      } else {
        // One position map over the existing bucket; weight updates land
        // in place (indices stay valid — appends are deferred), fresh
        // rows collect separately, zero-weight entries compact at the end.
        std::unordered_map<std::string_view, size_t> pos;
        pos.reserve(bucket.size());
        for (size_t b = 0; b < bucket.size(); b++) {
          pos.emplace(bucket[b].first, b);
        }
        std::vector<std::pair<std::string, int64_t>> fresh;
        // Reserve BEFORE taking string_views into fresh entries: SSO
        // string bytes move on vector reallocation.
        fresh.reserve(adds.size());
        std::unordered_map<std::string_view, size_t> fresh_pos;
        for (auto &[arb, aw] : adds) {
          auto it = pos.find(arb);
          if (it != pos.end()) {
            bucket[it->second].second += aw;
            continue;
          }
          auto fit = fresh_pos.find(arb);
          if (fit != fresh_pos.end()) {
            fresh[fit->second].second += aw;
            continue;
          }
          fresh.emplace_back(std::move(arb), aw);
          fresh_pos.emplace(fresh.back().first, fresh.size() - 1);
        }
        size_t out_i = 0;
        for (size_t b = 0; b < bucket.size(); b++) {
          if (bucket[b].second != 0) {
            if (out_i != b) {
              bucket[out_i] = std::move(bucket[b]);
            }
            out_i++;
          }
        }
        bucket.resize(out_i);
        for (auto &[frb, fw] : fresh) {
          if (fw != 0) {
            bucket.emplace_back(std::move(frb), fw);
          }
        }
      }
      if (bucket.empty()) {
        idx.erase(key);
      }
    }
  }

  bool probe_packed(bool left, const DuckDBRow &key, RowWeights &out) {
    out.clear();
    std::string kb;
    if (!packed::encode_row(kb, key)) {
      return false;
    }
    const auto &idx = left ? packed_left_ : packed_right_;
    const auto &flat = left ? flat_left_ : flat_right_;
    auto it = idx.find(kb);
    const flatpacked::DirEnt *fe = flat.find(kb);
    if (it == idx.end() && fe == nullptr) {
      return false;
    }
    // Merge layers by row bytes: flat weight + overlay delta. The common
    // cases are flat-only (post-restore reads) and overlay-only (models
    // built this session), both of which skip the merge map.
    if (fe != nullptr && it == idx.end()) {
      // Mapped-aware accessors, not the owned vectors — see write_pindex.
      out.reserve(fe->bucket_n);
      for (uint32_t b = 0; b < fe->bucket_n; b++) {
        const auto &be = flat.bucket_at(fe->bucket_off + b);
        out.emplace(decode_probe_row(reinterpret_cast<const char *>(
                        flat.arena_data() + be.row_off)),
                    be.weight);
      }
      return !out.empty();
    }
    if (fe == nullptr) {
      if (it->second.empty()) {
        return false;
      }
      out.reserve(it->second.size());
      for (const auto &[bytes, w] : it->second) {
        out.emplace(decode_probe_row(bytes.data()), w);
      }
      return true;
    }
    std::unordered_map<std::string, int64_t> merged;
    for (uint32_t b = 0; b < fe->bucket_n; b++) {
      const auto &be = flat.bucket_at(fe->bucket_off + b);
      merged.emplace(
          std::string(
              reinterpret_cast<const char *>(flat.arena_data() + be.row_off),
              be.row_len),
          be.weight);
    }
    for (const auto &[bytes, w] : it->second) {
      merged[bytes] += w;
    }
    for (const auto &[bytes, w] : merged) {
      if (w == 0) {
        continue;
      }
      out.emplace(decode_probe_row(bytes.data()), w);
    }
    return !out.empty();
  }

  // The padded side's own-row weight total for one row, across the three
  // storage modes (shared arrangement / packed / boxed).
  int64_t own_row_weight(const DuckDBRow &row, bool left) {
    if (left && shared_left_) {
      auto it = shared_left_->weights.find(row);
      return it == shared_left_->weights.end() ? 0 : it->second;
    }
    if (!left && shared_right_) {
      auto it = shared_right_->weights.find(row);
      return it == shared_right_->weights.end() ? 0 : it->second;
    }
    if (packed_ok_) {
      std::string rb;
      if (!packed::encode_row(rb, row)) {
        return 0;
      }
      const auto &wmap = left ? packed_wleft_ : packed_wright_;
      auto it = wmap.find(rb);
      const int64_t overlay = it == wmap.end() ? 0 : it->second;
      const auto &fw = left ? flat_wleft_ : flat_wright_;
      return overlay + fw.find(rb);
    }
    const RowWeights &weights = left ? left_weights_ : right_weights_;
    auto it = weights.find(row);
    return it == weights.end() ? 0 : it->second;
  }

  duckdb::JoinType join_type_;
  // Buffered inner-join emits for the vectorized output-hash preseed
  std::shared_ptr<PlanKeepAlive> keep_alive_join_;
  std::vector<DuckDBRow> pending_rows_;
  std::vector<int64_t> pending_weights_;
  std::unique_ptr<BatchEvaluator> out_eval_;
  duckdb::vector<duckdb::LogicalType> left_types_, right_types_;
  bool pads_left_ = false, pads_right_ = false;
  bool marks_ = false;
  bool null_safe_keys_ = false;
  std::unique_ptr<BatchEvaluator> batch_left_keys_, batch_right_keys_;
  // I1: at most one side reads a shared, CDC-maintained arrangement
  // (updated BEFORE views step, so it is post-delta for the current step)
  std::shared_ptr<const SharedArrangement> shared_left_, shared_right_;
  // O4: consumer projections into the full-row arrangements (empty =
  // identity = zero-copy fast path)
  std::vector<duckdb::idx_t> shared_proj_left_, shared_proj_right_;
  RowWeights probe_scratch_left_, probe_scratch_right_;
  // N3: local indexes on disk for pure probe-target sides (spill mode)
  std::unique_ptr<SpilledBucketIndex> local_spill_left_, local_spill_right_;
  std::mutex local_spill_mutex_; // shard threads share the LRU cache
  Index left_index_, right_index_;
  RowWeights left_weights_, right_weights_;   // incl. NULL-key rows
  RowWeights left_pad_, right_pad_;           // currently emitted pad weight
  int64_t right_total_ = 0, right_null_ = 0;  // MARK: subquery-side stats
  std::unordered_map<DuckDBRow, std::pair<duckdb::Value, int64_t>,
                     DuckDBRowHash>
      mark_state_; // MARK: emitted (mark, weight) per left row
  DuckDBZSet output_;
  bool has_output_ = false;
};

} // namespace dbsp_native
