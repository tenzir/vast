//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2016 The VAST Contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "vast/system/index.hpp"

#include "vast/fwd.hpp"

#include "vast/chunk.hpp"
#include "vast/concept/parseable/to.hpp"
#include "vast/concept/printable/to_string.hpp"
#include "vast/concept/printable/vast/bitmap.hpp"
#include "vast/concept/printable/vast/error.hpp"
#include "vast/concept/printable/vast/expression.hpp"
#include "vast/concept/printable/vast/table_slice.hpp"
#include "vast/concept/printable/vast/uuid.hpp"
#include "vast/data.hpp"
#include "vast/defaults.hpp"
#include "vast/detail/assert.hpp"
#include "vast/detail/cache.hpp"
#include "vast/detail/fill_status_map.hpp"
#include "vast/detail/narrow.hpp"
#include "vast/detail/notifying_stream_manager.hpp"
#include "vast/detail/settings.hpp"
#include "vast/error.hpp"
#include "vast/expression_visitors.hpp"
#include "vast/fbs/index.hpp"
#include "vast/fbs/partition.hpp"
#include "vast/fbs/utils.hpp"
#include "vast/fbs/uuid.hpp"
#include "vast/ids.hpp"
#include "vast/io/read.hpp"
#include "vast/io/save.hpp"
#include "vast/logger.hpp"
#include "vast/partition_synopsis.hpp"
#include "vast/system/evaluator.hpp"
#include "vast/system/local_segment_store.hpp"
#include "vast/system/meta_index.hpp"
#include "vast/system/partition.hpp"
#include "vast/system/query_supervisor.hpp"
#include "vast/system/shutdown.hpp"
#include "vast/system/status.hpp"
#include "vast/table_slice.hpp"
#include "vast/uuid.hpp"
#include "vast/value_index.hpp"

#include <caf/error.hpp>
#include <caf/response_promise.hpp>
#include <caf/typed_event_based_actor.hpp>
#include <caf/typed_response_promise.hpp>
#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <memory>
#include <unistd.h>

using namespace std::chrono;

// clang-format off
//
// # Import
//
// The index is implemented as a stream stage that hooks into the table slice
// stream coming from the importer, and forwards them to the current active
// partition
//
//              table slice              table slice                      table slice column
//   importer ----------------> index ---------------> active partition ------------------------> indexer
//                                                                      ------------------------> indexer
//                                                                                ...
//
// # Lookup
//
// At the same time, the index is also involved in the lookup path, where it
// receives an expression and loads the partitions that might contain relevant
// results into memory.
//
//    expression                                lookup()
//   ------------>  index                  --------------------> meta_index
//                                                                 |
//     query_id,                                                   |
//     scheduled,                                                  |
//     remaining                                    [uuid]         |
//   <-----------  (creates query state)  <------------------------/
//                            |
//                            |  query_id, n_taste
//                            |
//    query_id, n             v                   expression, client
//   ------------> (spawn n partitions) --------------------------------> partition
//                                                                            |
//                                                      ids                   |
//   <------------------------------------------------------------------------/
//                                                      ids                   |
//   <------------------------------------------------------------------------/
//                                                                            |
//
//                                                                          [...]
//
//                                                      atom::done            |
//   <------------------------------------------------------------------------/
//
//
// # Erase
//
// We currently have two distinct erasure code paths: One externally driven by
// the disk monitor, who looks at the file system and identifies those partitions
// that shall be removed. This is done by the `atom::erase` handler.
//
// The other is data-driven and comes from the `eraser`, who sends us a `vast::query`
// whose results shall be deleted from disk.
//
// clang-format on

namespace vast::system {

namespace {

caf::error extract_partition_synopsis(
  const std::filesystem::path& partition_path,
  const std::filesystem::path& partition_synopsis_path) {
  // Use blocking operations here since this is part of the startup.
  auto chunk = chunk::mmap(partition_path);
  if (!chunk)
    return std::move(chunk.error());
  const auto* partition = fbs::GetPartition(chunk->get()->data());
  if (partition->partition_type() != fbs::partition::Partition::v0)
    return caf::make_error(ec::format_error, "found unsupported version for "
                                             "partition "
                                               + partition_path.string());
  const auto* partition_v0 = partition->partition_as_v0();
  VAST_ASSERT(partition_v0);
  partition_synopsis ps;
  unpack(*partition_v0, ps);
  flatbuffers::FlatBufferBuilder builder;
  auto ps_offset = *pack(builder, ps);
  fbs::PartitionSynopsisBuilder ps_builder(builder);
  ps_builder.add_partition_synopsis_type(
    fbs::partition_synopsis::PartitionSynopsis::v0);
  ps_builder.add_partition_synopsis(ps_offset.Union());
  auto flatbuffer = ps_builder.Finish();
  fbs::FinishPartitionSynopsisBuffer(builder, flatbuffer);
  auto chunk_out = fbs::release(builder);
  return io::save(partition_synopsis_path,
                  span{chunk_out->data(), chunk_out->size()});
}

} // namespace

std::filesystem::path index_state::partition_path(const uuid& id) const {
  return dir / to_string(id);
}

std::filesystem::path
index_state::partition_synopsis_path(const uuid& id) const {
  return synopsisdir / (to_string(id) + ".mdx");
}

partition_actor partition_factory::operator()(const uuid& id) const {
  // Load partition from disk.
  VAST_ASSERT(std::find(state_.persisted_partitions.begin(),
                        state_.persisted_partitions.end(), id)
              != state_.persisted_partitions.end());
  const auto path = state_.partition_path(id);
  VAST_DEBUG("{} loads partition {} for path {}", state_.self, id, path);
  return state_.self->spawn(passive_partition, id,
                            static_cast<store_actor>(state_.global_store),
                            filesystem_, path);
}

filesystem_actor& partition_factory::filesystem() {
  return filesystem_;
}

partition_factory::partition_factory(index_state& state) : state_{state} {
  // nop
}

index_state::index_state(index_actor::pointer self)
  : self{self}, inmem_partitions{0, partition_factory{*this}} {
}

caf::error index_state::load_from_disk() {
  // We dont use the filesystem actor here because this function is only
  // called once during startup, when no other actors exist yet.
  std::error_code err{};
  const auto file_exists = std::filesystem::exists(dir, err);
  if (!file_exists) {
    VAST_VERBOSE("{} found no prior state, starting with a clean slate", self);
    return caf::none;
  }
  if (auto fname = index_filename(); std::filesystem::exists(fname, err)) {
    VAST_VERBOSE("{} loads state from {}", self, fname);
    auto buffer = io::read(fname);
    if (!buffer) {
      VAST_ERROR("{} failed to read index file: {}", self,
                 render(buffer.error()));
      return buffer.error();
    }
    // TODO: Create a `index_ondisk_state` struct and move this part of the
    // code into an `unpack()` function.
    const auto* index = fbs::GetIndex(buffer->data());
    if (index->index_type() != fbs::index::Index::v0)
      return caf::make_error(ec::format_error, "invalid index version");
    const auto* index_v0 = index->index_as_v0();
    const auto* partition_uuids = index_v0->partitions();
    VAST_ASSERT(partition_uuids);
    auto synopses = std::make_shared<std::map<uuid, partition_synopsis>>();
    for (const auto* uuid_fb : *partition_uuids) {
      VAST_ASSERT(uuid_fb);
      vast::uuid partition_uuid{};
      unpack(*uuid_fb, partition_uuid);
      auto part_path = partition_path(partition_uuid);
      auto synopsis_path = partition_synopsis_path(partition_uuid);
      if (!exists(part_path)) {
        VAST_WARN("{} found partition {}"
                  "in the index state but not on disk; this may have been "
                  "caused by an unclean shutdown",
                  self, partition_uuid);
        continue;
      }
      // Generate external partition synopsis file if it doesn't exist.
      if (!exists(synopsis_path)) {
        if (auto error = extract_partition_synopsis(part_path, synopsis_path))
          return error;
      }
    retry:
      auto chunk = chunk::mmap(synopsis_path);
      if (!chunk) {
        VAST_WARN("{} could not mmap partition at {}", self, part_path);
        continue;
      }
      const auto* ps_flatbuffer
        = fbs::GetPartitionSynopsis(chunk->get()->data());
      partition_synopsis ps;
      if (ps_flatbuffer->partition_synopsis_type()
          != fbs::partition_synopsis::PartitionSynopsis::v0)
        return caf::make_error(ec::format_error, "invalid partition synopsis "
                                                 "version");
      const auto& synopsis_v0 = *ps_flatbuffer->partition_synopsis_as_v0();
      // Re-write old partition synopses that were created before the offset and
      // id were saved.
      if (!synopsis_v0.id_range()) {
        VAST_VERBOSE("{} rewrites old meta-index data for partition {}", self,
                     partition_uuid);
        if (auto error = extract_partition_synopsis(part_path, synopsis_path))
          return error;
        // TODO: Avoid goto.
        goto retry;
      }
      if (auto error = unpack(synopsis_v0, ps))
        return error;
      meta_index_bytes += ps.memusage();
      persisted_partitions.insert(partition_uuid);
      synopses->emplace(partition_uuid, std::move(ps));
    }
    // We collect all synopses to send them in bulk, since the `await` interface
    // doesn't lend itself to a huge number of awaited messages: Only the tip of
    // the current awaited list is considered, leading to an O(n**2) worst-case
    // behavior if the responses arrive in the same order to how they were sent.
    VAST_DEBUG("{} requesting bulk merge of {} partitions", self,
               synopses->size());
    this->accept_queries = false;
    self
      ->request(meta_index, caf::infinite, atom::merge_v,
                std::exchange(synopses, {}))
      .then(
        [=](atom::ok) {
          VAST_VERBOSE("{} successfully loaded meta index from disk and will "
                       "start processing queries",
                       self);
          this->accept_queries = true;
        },
        [=](caf::error& err) {
          VAST_ERROR("{} could not load meta index state from disk, shutting "
                     "down with error {}",
                     self, err);
          self->send_exit(self, std::move(err));
        });
    const auto* stats = index_v0->stats();
    if (!stats)
      return caf::make_error(ec::format_error, "no stats in persisted index "
                                               "state");
    for (const auto* const stat : *stats) {
      this->stats.layouts[stat->name()->str()]
        = layout_statistics{stat->count()};
    }
  } else {
    VAST_INFO("{} found existing database dir {} without index "
              "statefile, "
              "will start with fresh state",
              self, dir);
  }
  return caf::none;
}

bool index_state::worker_available() const {
  return !idle_workers.empty();
}

std::optional<query_supervisor_actor> index_state::next_worker() {
  if (!worker_available()) {
    VAST_VERBOSE("{} waits for query supervisors to become available to "
                 "delegate work; consider increasing 'vast.max-queries'",
                 self);
    return std::nullopt;
  }
  auto result = std::move(idle_workers.back());
  idle_workers.pop_back();
  return result;
}

void index_state::add_flush_listener(flush_listener_actor listener) {
  VAST_DEBUG("{} adds a new 'flush' subscriber: {}", self, listener);
  flush_listeners.emplace_back(std::move(listener));
  detail::notify_listeners_if_clean(*this, *stage);
}

// The whole purpose of the `-b` flag is to somehow block until all imported
// data is available for querying, so we have to layer hack upon hack here to
// achieve this most of the time. This is only used for integration tests.
// Note that there's still a race condition here if the call to
// `notify_flush_listeners()` arrives when there's still data en route to
// the unpersisted partitions.
// TODO(ch19583): Rip out the whole 'notifying_stream_manager' and replace it
// with some kind of ping/pong protocol.
void index_state::notify_flush_listeners() {
  VAST_DEBUG("{} sends 'flush' messages to {} listeners", self,
             flush_listeners.size());
  for (auto& listener : flush_listeners) {
    if (active_partition.actor)
      self->send(active_partition.actor, atom::subscribe_v, atom::flush_v,
                 listener);
    else
      self->send(listener, atom::flush_v);
  }
  flush_listeners.clear();
}

void index_state::create_active_partition() {
  auto id = uuid::random();
  caf::settings index_opts;
  index_opts["cardinality"] = partition_capacity;
  // These options must be kept in sync with vast/address_synopsis.hpp and
  // vast/string_synopsis.hpp respectively.
  auto synopsis_options = caf::settings{};
  put(synopsis_options, "max-partition-size", partition_capacity);
  put(synopsis_options, "address-synopsis-fp-rate", meta_index_fp_rate);
  put(synopsis_options, "string-synopsis-fp-rate", meta_index_fp_rate);
  // If we're using the global store, the importer already sends the table
  // slices. (In the long run, this should probably be streamlined so that all
  // data moves through the index. However, that requires some refactoring of
  // the archive itself so it can handle multiple input streams.)
  std::string store_name = {};
  chunk_ptr store_header = chunk::empty();
  if (partition_local_stores) {
    store_name = store_plugin->name();
    auto builder_and_header = store_plugin->make_store_builder(filesystem, id);
    if (!builder_and_header) {
      VAST_ERROR("could not create new active partition: {}",
                 render(builder_and_header.error()));
      self->quit(builder_and_header.error());
      return;
    }
    VAST_ASSERT(builder_and_header); // FIXME
    auto& [builder, header] = *builder_and_header;
    store_header = header;
    active_partition.store = builder;
    active_partition.store_slot
      = stage->add_outbound_path(active_partition.store);
  } else {
    store_name = "legacy_archive";
    active_partition.store = global_store;
  }
  active_partition.actor
    = self->spawn(::vast::system::active_partition, id, filesystem, index_opts,
                  synopsis_options,
                  static_cast<store_actor>(active_partition.store), store_name,
                  store_header);
  active_partition.stream_slot
    = stage->add_outbound_path(active_partition.actor);
  active_partition.capacity = partition_capacity;
  active_partition.id = id;
  VAST_DEBUG("{} created new partition {}", self, id);
}

void index_state::decomission_active_partition() {
  auto id = active_partition.id;
  auto actor = std::exchange(active_partition.actor, {});
  unpersisted[id] = actor;
  // Send buffered batches and remove active partition from the stream.
  stage->out().fan_out_flush();
  stage->out().close(active_partition.stream_slot);
  if (partition_local_stores)
    stage->out().close(active_partition.store_slot);
  stage->out().force_emit_batches();
  // Persist active partition asynchronously.
  auto part_dir = partition_path(id);
  auto synopsis_dir = partition_synopsis_path(id);
  VAST_DEBUG("{} persists active partition to {}", self, part_dir);
  self->request(actor, caf::infinite, atom::persist_v, part_dir, synopsis_dir)
    .then(
      [=](std::shared_ptr<partition_synopsis>& ps) {
        VAST_DEBUG("{} successfully persisted partition {}", self, id);
        // Semantically ps is a unique_ptr, and the partition releases its
        // copy before sending. We use shared_ptr for the transport because
        // CAF message types must be copy-constructible.
        meta_index_bytes += ps->memusage();
        // TODO: We should skip this continuation if we're currently shutting
        // down.
        self
          ->request(meta_index, caf::infinite, atom::merge_v, id, std::move(ps))
          .then(
            [=](atom::ok) {
              VAST_DEBUG("{} received ok for request to persist partition {}",
                         self, id);
              unpersisted.erase(id);
              persisted_partitions.insert(id);
            },
            [=](const caf::error& err) {
              VAST_DEBUG("{} received error for request to persist partition "
                         "{}: {}",
                         self, id, err);
            });
      },
      [=](caf::error& err) {
        VAST_ERROR("{} failed to persist partition {} with error: {}", self, id,
                   err);
        self->quit(std::move(err));
      });
}

caf::typed_response_promise<caf::settings>
index_state::status(status_verbosity v) const {
  struct extra_state {
    size_t memory_usage = 0;
    void deliver(caf::typed_response_promise<caf::settings>&& promise,
                 caf::settings&& content) {
      put(content, "index.memory-usage", memory_usage);
      promise.deliver(std::move(content));
    }
  };
  auto rs = make_status_request_state<extra_state>(self);
  if (v >= status_verbosity::detailed) {
    auto& stats_object = put_dictionary(rs->content, "statistics");
    auto& layout_object = put_dictionary(stats_object, "layouts");
    for (const auto& [name, layout_stats] : stats.layouts) {
      auto xs = caf::dictionary<caf::config_value>{};
      xs.emplace("count", layout_stats.count);
      // We cannot use put_dictionary(layout_object, name) here, because this
      // function splits the key at '.', which occurs in every layout name.
      // Hence the fallback to low-level primitives.
      layout_object.insert_or_assign(name, std::move(xs));
    }
    put(rs->content, "meta-index-bytes", meta_index_bytes);
    put(rs->content, "num-active-partitions",
        active_partition.actor == nullptr ? 0 : 1);
    put(rs->content, "num-cached-partitions", inmem_partitions.size());
    put(rs->content, "num-unpersisted-partitions", unpersisted.size());
    const auto timeout = defaults::system::initial_request_timeout / 5 * 4;
    auto& partitions = put_dictionary(rs->content, "partitions");
    auto partition_status = [&](const uuid& id, const partition_actor& pa,
                                caf::config_value::list& xs) {
      collect_status(
        rs, timeout, v, pa,
        [=, &xs](const caf::settings& part_status) {
          auto& ps = xs.emplace_back().as_dictionary();
          put(ps, "id", to_string(id));
          if (auto s = caf::get_if<caf::config_value::integer>(&part_status,
                                                               "memory-usage"))
            rs->memory_usage += *s;
          if (v >= status_verbosity::debug)
            detail::merge_settings(part_status, ps, policy::merge_lists::no);
        },
        [=, &xs](const caf::error& err) {
          VAST_WARN("{} failed to retrieve status from {} : {}", self, id,
                    render(err));
          auto& ps = xs.emplace_back().as_dictionary();
          put(ps, "id", to_string(id));
          put(ps, "error", render(err));
        });
    };
    // Resident partitions.
    auto& active = caf::put_list(partitions, "active");
    active.reserve(1);
    if (active_partition.actor != nullptr)
      partition_status(active_partition.id, active_partition.actor, active);
    auto& cached = put_list(partitions, "cached");
    cached.reserve(inmem_partitions.size());
    for (const auto& [id, actor] : inmem_partitions)
      partition_status(id, actor, cached);
    auto& unpersisted = put_list(partitions, "unpersisted");
    unpersisted.reserve(this->unpersisted.size());
    for (const auto& [id, actor] : this->unpersisted)
      partition_status(id, actor, unpersisted);
    // General state such as open streams.
  }
  if (v >= status_verbosity::debug)
    detail::fill_status_map(rs->content, self);
  return rs->promise;
}

std::vector<std::pair<uuid, partition_actor>>
index_state::collect_query_actors(query_state& lookup,
                                  uint32_t num_partitions) {
  VAST_TRACE_SCOPE("{} {}", VAST_ARG(lookup), VAST_ARG(num_partitions));
  std::vector<std::pair<uuid, partition_actor>> result;
  if (num_partitions == 0 || lookup.partitions.empty())
    return result;
  // Prefer partitions that are already available in RAM.
  auto partition_is_loaded = [&](const uuid& candidate) {
    return (active_partition.actor != nullptr
            && active_partition.id == candidate)
           || (unpersisted.count(candidate) != 0u)
           || inmem_partitions.contains(candidate);
  };
  std::partition(lookup.partitions.begin(), lookup.partitions.end(),
                 partition_is_loaded);
  // Helper function to spin up EVALUATOR actors for a single partition.
  auto spin_up = [&](const uuid& partition_id) -> partition_actor {
    // We need to first check whether the ID is the active partition or one
    // of our unpersisted ones. Only then can we dispatch to our LRU cache.
    partition_actor part;
    if (active_partition.actor != nullptr
        && active_partition.id == partition_id)
      part = active_partition.actor;
    else if (auto it = unpersisted.find(partition_id); it != unpersisted.end())
      part = it->second;
    else if (auto it = persisted_partitions.find(partition_id);
             it != persisted_partitions.end())
      part = inmem_partitions.get_or_load(partition_id);
    if (!part)
      VAST_ERROR("{} could not load partition {} that was part of a "
                 "query",
                 self, partition_id);
    return part;
  };
  // Loop over the candidate set until we either successfully scheduled
  // num_partitions partitions or run out of candidates.
  auto it = lookup.partitions.begin();
  auto last = lookup.partitions.end();
  while (it != last && result.size() < num_partitions) {
    auto partition_id = *it++;
    if (auto partition_actor = spin_up(partition_id))
      result.emplace_back(partition_id, partition_actor);
  }
  lookup.partitions.erase(lookup.partitions.begin(), it);
  VAST_DEBUG("{} launched {} partition actors to evaluate query", self,
             result.size());
  return result;
}

std::filesystem::path
index_state::index_filename(const std::filesystem::path& basename) const {
  return basename / dir / "index.bin";
}

caf::expected<flatbuffers::Offset<fbs::Index>>
pack(flatbuffers::FlatBufferBuilder& builder, const index_state& state) {
  VAST_DEBUG("index persists {} uuids of definitely persisted and {}"
             "uuids of maybe persisted partitions",
             state.persisted_partitions.size(), state.unpersisted.size());
  std::vector<flatbuffers::Offset<fbs::uuid::v0>> partition_offsets;
  for (auto uuid : state.persisted_partitions) {
    if (auto uuid_fb = pack(builder, uuid))
      partition_offsets.push_back(*uuid_fb);
    else
      return uuid_fb.error();
  }
  // We don't know if these will make it to disk before the index and the rest
  // of the system is shut down (in case of a hard/dirty shutdown), so we just
  // store everything and throw out the missing partitions when loading the
  // index.
  for (const auto& kv : state.unpersisted) {
    if (auto uuid_fb = pack(builder, kv.first))
      partition_offsets.push_back(*uuid_fb);
    else
      return uuid_fb.error();
  }
  auto partitions = builder.CreateVector(partition_offsets);
  std::vector<flatbuffers::Offset<fbs::layout_statistics::v0>> stats_offsets;
  for (const auto& [name, layout_stats] : state.stats.layouts) {
    auto name_fb = builder.CreateString(name);
    fbs::layout_statistics::v0Builder stats_builder(builder);
    stats_builder.add_name(name_fb);
    stats_builder.add_count(layout_stats.count);
    auto offset = stats_builder.Finish();
    stats_offsets.push_back(offset);
  }
  auto stats = builder.CreateVector(stats_offsets);
  fbs::index::v0Builder v0_builder(builder);
  v0_builder.add_partitions(partitions);
  v0_builder.add_stats(stats);
  auto index_v0 = v0_builder.Finish();
  fbs::IndexBuilder index_builder(builder);
  index_builder.add_index_type(vast::fbs::index::Index::v0);
  index_builder.add_index(index_v0.Union());
  auto index = index_builder.Finish();
  fbs::FinishIndexBuffer(builder, index);
  return index;
}

/// Persists the state to disk.
void index_state::flush_to_disk() {
  auto builder = flatbuffers::FlatBufferBuilder{};
  auto index = pack(builder, *this);
  if (!index) {
    VAST_WARN("{} failed to pack index: {}", self, index.error());
    return;
  }
  auto chunk = fbs::release(builder);
  self
    ->request(filesystem, caf::infinite, atom::write_v, index_filename(), chunk)
    .then(
      [=](atom::ok) {
        VAST_DEBUG("{} successfully persisted index state", self);
      },
      [=](const caf::error& err) {
        VAST_WARN("{} failed to persist index state: {}", self, render(err));
      });
}

index_actor::behavior_type
index(index_actor::stateful_pointer<index_state> self,
      filesystem_actor filesystem, archive_actor archive,
      const std::filesystem::path& dir, std::string store_backend,
      size_t partition_capacity, size_t max_inmem_partitions,
      size_t taste_partitions, size_t num_workers,
      const std::filesystem::path& meta_index_dir, double meta_index_fp_rate) {
  VAST_TRACE_SCOPE("{} {} {} {} {} {} {}", VAST_ARG(filesystem), VAST_ARG(dir),
                   VAST_ARG(partition_capacity), VAST_ARG(max_inmem_partitions),
                   VAST_ARG(taste_partitions), VAST_ARG(num_workers),
                   VAST_ARG(meta_index_dir), VAST_ARG(meta_index_fp_rate));
  VAST_VERBOSE("{} initializes index in {} with a maximum partition "
               "size of {} events and {} resident partitions",
               self, dir, partition_capacity, max_inmem_partitions);
  // The global archive gets hard-coded special treatment for backwards
  // compatibility.
  self->state.partition_local_stores = store_backend != "archive";
  if (self->state.partition_local_stores)
    VAST_VERBOSE("{} uses partition-local stores instead of the archive", self);
  if (dir != meta_index_dir)
    VAST_VERBOSE("{} uses {} for meta index data", self, meta_index_dir);
  // Set members.
  self->state.self = self;
  self->state.global_store = archive;
  self->state.accept_queries = true;
  if (self->state.partition_local_stores) {
    self->state.store_plugin = plugins::find<store_plugin>(store_backend);
    if (!self->state.store_plugin) {
      auto error = caf::make_error(ec::invalid_configuration,
                                   fmt::format("could not find "
                                               "store plugin '{}'",
                                               store_backend));
      VAST_ERROR("{}", render(error));
      self->quit(error);
      return index_actor::behavior_type::make_empty_behavior();
    }
  }
  self->state.filesystem = std::move(filesystem);
  self->state.meta_index = self->spawn<caf::lazy_init>(meta_index);
  self->state.dir = dir;
  self->state.synopsisdir = meta_index_dir;
  self->state.partition_capacity = partition_capacity;
  self->state.taste_partitions = taste_partitions;
  self->state.inmem_partitions.factory().filesystem() = self->state.filesystem;
  self->state.inmem_partitions.resize(max_inmem_partitions);
  self->state.meta_index_fp_rate = meta_index_fp_rate;
  self->state.meta_index_bytes = 0;
  // Read persistent state.
  if (auto err = self->state.load_from_disk()) {
    VAST_ERROR("{} failed to load index state from disk: {}", self,
               render(err));
    self->quit(err);
    return index_actor::behavior_type::make_empty_behavior();
  }
  // Setup stream manager.
  self->state.stage = detail::attach_notifying_stream_stage(
    self,
    /* continuous = */ true,
    [](caf::unit_t&) {
      // nop
    },
    [self](caf::unit_t&, caf::downstream<table_slice>& out, table_slice x) {
      VAST_ASSERT(x.encoding() != table_slice_encoding::none);
      auto&& layout = x.layout();
      self->state.stats.layouts[layout.name()].count += x.rows();
      auto& active = self->state.active_partition;
      if (!active.actor) {
        self->state.create_active_partition();
      } else if (x.rows() > active.capacity) {
        VAST_DEBUG("{} exceeds active capacity by {} rows", self,
                   x.rows() - active.capacity);
        self->state.decomission_active_partition();
        self->state.flush_to_disk();
        self->state.create_active_partition();
      }
      out.push(x);
      if (active.capacity == self->state.partition_capacity
          && x.rows() > active.capacity) {
        VAST_WARN("{} got table slice with {} rows that exceeds the "
                  "default partition capacity of {} rows",
                  self, x.rows(), self->state.partition_capacity);
        active.capacity = 0;
      } else {
        VAST_ASSERT(active.capacity >= x.rows());
        active.capacity -= x.rows();
      }
    },
    [self](caf::unit_t&, const caf::error& err) {
      // During "normal" shutdown, the node will send an exit message to
      // the importer which then cuts the stream to the index, and the
      // index exits afterwards.
      // We get an 'unreachable' error when the stream becomes unreachable
      // during actor destruction; in this case we can't use `self->state`
      // anymore since it will already be destroyed.
      VAST_DEBUG("index finalized streaming with error {}", render(err));
      if (err && err != caf::exit_reason::unreachable) {
        if (err != caf::exit_reason::user_shutdown)
          VAST_ERROR("{} got a stream error: {}", self, render(err));
        else
          VAST_DEBUG("{} got a user shutdown error: {}", self, render(err));
        // We can shutdown now because we only get a single stream from the
        // importer.
        self->send_exit(self, err);
      }
    });
  self->set_exit_handler([self](const caf::exit_msg& msg) {
    VAST_DEBUG("{} received EXIT from {} with reason: {}", self, msg.source,
               msg.reason);
    // Flush buffered batches and end stream.
    self->state.stage->shutdown(); // closes inbound paths
    self->state.stage->out().fan_out_flush();
    self->state.stage->out().close(); // closes outbound paths
    self->state.stage->out().force_emit_batches();
    // Bring down active partition.
    if (self->state.active_partition.actor)
      self->state.decomission_active_partition();
    // Collect partitions for termination.
    // TODO: We must actor_cast to caf::actor here because 'shutdown' operates
    // on 'std::vector<caf::actor>' only. That should probably be generalized in
    // the future.
    std::vector<caf::actor> partitions;
    partitions.reserve(self->state.inmem_partitions.size() + 1);
    for ([[maybe_unused]] auto& [_, part] : self->state.unpersisted)
      partitions.push_back(caf::actor_cast<caf::actor>(part));
    for ([[maybe_unused]] auto& [_, part] : self->state.inmem_partitions)
      partitions.push_back(caf::actor_cast<caf::actor>(part));
    self->state.flush_to_disk();
    // Receiving an EXIT message does not need to coincide with the state being
    // destructed, so we explicitly clear the tables to release the references.
    self->state.unpersisted.clear();
    self->state.inmem_partitions.clear();
    // Terminate partition actors.
    VAST_DEBUG("{} brings down {} partitions", self, partitions.size());
    shutdown<policy::parallel>(self, std::move(partitions));
  });
  // Launch workers for resolving queries.
  for (size_t i = 0; i < num_workers; ++i)
    self->spawn(query_supervisor,
                caf::actor_cast<query_supervisor_master_actor>(self));
  return {
    [self](atom::done, uuid partition_id) {
      VAST_DEBUG("{} queried partition {} successfully", self, partition_id);
    },
    [self](
      caf::stream<table_slice> in) -> caf::inbound_stream_slot<table_slice> {
      VAST_DEBUG("{} got a new stream source", self);
      return self->state.stage->add_inbound_path(in);
    },
    [self](accountant_actor accountant) {
      self->state.accountant = std::move(accountant);
    },
    [self](atom::subscribe, atom::flush, flush_listener_actor listener) {
      VAST_DEBUG("{} adds flush listener", self);
      self->state.add_flush_listener(std::move(listener));
    },
    [self](vast::query query) -> caf::result<void> {
      if (!self->state.accept_queries) {
        VAST_VERBOSE("{} delays query {} because it is still starting up", self,
                     query);
        return caf::skip;
      }
      // TODO: This check is not required technically, but we use the query
      // supervisor availability to rate-limit meta-index lookups. Do we
      // really need this?
      if (!self->state.worker_available())
        return caf::skip;
      // Query handling
      auto mid = self->current_message_id();
      auto sender = self->current_sender();
      auto client = caf::actor_cast<caf::actor>(sender);
      // TODO: This is used in order to "respond" to the message and to still
      // continue with the function afterwards. At some point this should be
      // changed to a proper solution for that problem, e.g., streaming.
      auto respond = [=](auto&&... xs) {
        unsafe_response(self, sender, {}, mid.response_id(),
                        std::forward<decltype(xs)>(xs)...);
      };
      // Convenience function for dropping out without producing hits.
      // Makes sure that clients always receive a 'done' message.
      auto no_result = [=] {
        respond(uuid::nil(), uint32_t{0}, uint32_t{0});
        caf::anon_send(client, atom::done_v);
      };
      // Sanity check.
      if (!sender) {
        VAST_WARN("{} ignores an anonymous query", self);
        respond(caf::sec::invalid_argument);
        return {};
      }
      std::vector<uuid> candidates;
      if (self->state.active_partition.actor)
        candidates.push_back(self->state.active_partition.id);
      for (const auto& [id, _] : self->state.unpersisted)
        candidates.push_back(id);
      auto rp = self->make_response_promise<void>();
      // Get all potentially matching partitions.
      self->request(self->state.meta_index, caf::infinite, query.expr)
        .then(
          [=, candidates = std::move(candidates)](
            std::vector<uuid> midx_candidates) mutable {
            VAST_DEBUG("{} got initial candidates {} and from meta-index {}",
                       self, candidates, midx_candidates);
            candidates.insert(candidates.end(), midx_candidates.begin(),
                              midx_candidates.end());
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()),
                             candidates.end());
            if (candidates.empty()) {
              VAST_DEBUG("{} returns without result: no partitions qualify",
                         self);
              no_result();
              // TODO: When updating to CAF 0.18, remove the use of the
              // untyped response promise and call deliver without arguments.
              auto& untyped_rp = static_cast<caf::response_promise&>(rp);
              untyped_rp.deliver(caf::unit);
              return;
            }
            // Allows the client to query further results after initial taste.
            auto query_id = uuid::random();
            // Ensure the query id is unique.
            while (self->state.pending.find(query_id)
                     != self->state.pending.end()
                   || query_id == uuid::nil())
              query_id = uuid::random();
            auto total = candidates.size();
            auto scheduled = detail::narrow<uint32_t>(
              std::min(candidates.size(), self->state.taste_partitions));
            auto lookup = query_state{query_id, query, std::move(candidates)};
            auto result
              = self->state.pending.emplace(query_id, std::move(lookup));
            VAST_ASSERT(result.second);
            respond(query_id, detail::narrow<uint32_t>(total), scheduled);
            rp.delegate(caf::actor_cast<caf::actor>(self), query_id, scheduled);
          },
          [=](caf::error err) mutable {
            VAST_ERROR("{} failed to receive candidates from meta-index: {}",
                       self, render(err));
            rp.deliver(std::move(err));
          });
      return rp;
    },
    [self](const uuid& query_id, uint32_t num_partitions) -> caf::result<void> {
      auto sender = self->current_sender();
      auto client = caf::actor_cast<receiver_actor<atom::done>>(sender);
      // Sanity checks.
      if (!sender) {
        VAST_ERROR("{} ignores an anonymous query", self);
        return {};
      }
      // A zero as second argument means the client drops further results.
      if (num_partitions == 0) {
        VAST_DEBUG("{} drops remaining results for query id {}", self,
                   query_id);
        self->state.pending.erase(query_id);
        return {};
      }
      auto iter = self->state.pending.find(query_id);
      if (iter == self->state.pending.end()) {
        VAST_WARN("{} drops query for unknown query id {}", self, query_id);
        self->send(client, atom::done_v);
        return {};
      }
      auto& query_state = iter->second;
      auto worker = self->state.next_worker();
      if (!worker)
        return caf::skip;
      // Get partition actors, spawning new ones if needed.
      auto actors
        = self->state.collect_query_actors(query_state, num_partitions);
      // Delegate to query supervisor (uses up this worker) and report
      // query ID + some stats to the client.
      VAST_DEBUG("{} scheduled {} more partition(s) for query id {}"
                 "with {} partitions remaining",
                 self, actors.size(), query_id, query_state.partitions.size());
      self->send(*worker, query_state.query, std::move(actors), client);
      // Cleanup if we exhausted all candidates.
      if (query_state.partitions.empty())
        self->state.pending.erase(iter);
      return {};
    },
    [self](atom::erase, uuid partition_id) -> caf::result<atom::done> {
      VAST_VERBOSE("{} erases partition {}", self, partition_id);
      auto rp = self->make_response_promise<atom::done>();
      auto path = self->state.partition_path(partition_id);
      auto synopsis_path = self->state.partition_synopsis_path(partition_id);
      bool adjust_stats = true;
      if (self->state.persisted_partitions.count(partition_id) == 0u) {
        std::error_code err{};
        const auto file_exists = std::filesystem::exists(path, err);
        if (!file_exists) {
          rp.deliver(caf::make_error(
            ec::logic_error, fmt::format("unknown partition for path {}: {}",
                                         path, err.message())));
          return rp;
        }
        // As a special case, if the partition exists on disk we just continue
        // normally here, since this indicates a previous erasure did not go
        // through cleanly.
        adjust_stats = false;
      }
      self
        ->request(self->state.meta_index, caf::infinite, atom::erase_v,
                  partition_id)
        .then(
          [self, partition_id, path, synopsis_path, rp,
           adjust_stats](atom::ok) mutable {
            auto partition_actor
              = self->state.inmem_partitions.eject(partition_id);
            self->state.persisted_partitions.erase(partition_id);
            self
              ->request(self->state.filesystem, caf::infinite, atom::mmap_v,
                        path)
              .then(
                [=](const chunk_ptr& chunk) mutable {
                  // Adjust layout stats by subtracting the events of the
                  // removed partition.
                  const auto* partition = fbs::GetPartition(chunk->data());
                  if (partition->partition_type()
                      != fbs::partition::Partition::v0) {
                    rp.deliver(caf::make_error(ec::format_error, "unexpected "
                                                                 "format "
                                                                 "version"));
                    return;
                  }
                  vast::ids all_ids;
                  const auto* partition_v0 = partition->partition_as_v0();
                  for (const auto* partition_stats :
                       *partition_v0->type_ids()) {
                    const auto* name = partition_stats->name();
                    vast::ids ids;
                    if (auto error
                        = fbs::deserialize_bytes(partition_stats->ids(), ids)) {
                      rp.deliver(caf::make_error(ec::format_error,
                                                 "could not deserialize "
                                                 "ids: "
                                                   + render(error)));
                      return;
                    }
                    all_ids |= ids;
                    if (adjust_stats)
                      self->state.stats.layouts[name->str()].count -= rank(ids);
                  }
                  // Note that mmap's will increase the reference count of a
                  // file, so unlinking should not affect indexers that are
                  // currently loaded and answering a query.
                  std::error_code err{};
                  std::filesystem::remove_all(synopsis_path, err);
                  if (err)
                    VAST_WARN("{} could not unlink partition synopsis at", self,
                              synopsis_path);
                  // TODO: We could send `all_ids` as the second argument here,
                  // which doesn't really make sense from an interface
                  // perspective but would save the partition from recomputing
                  // the same bitmap.
                  rp.delegate(partition_actor, atom::erase_v);
                },
                [=](caf::error& err) mutable {
                  rp.deliver(std::move(err));
                });
          },
          [partition_id](const caf::error& err) {
            VAST_WARN("index encountered an error trying to erase "
                      "partition {} from the meta index: {}",
                      partition_id, err);
          });
      return rp;
    },
    // -- query_supervisor_master_actor ----------------------------------------
    [self](atom::worker, query_supervisor_actor worker) {
      if (!self->state.worker_available())
        VAST_DEBUG("{} delegates work to query supervisors", self);
      self->state.idle_workers.emplace_back(std::move(worker));
    },
    // -- status_client_actor --------------------------------------------------
    [self](atom::status, status_verbosity v) { //
      return self->state.status(v);
    },
  };
}

} // namespace vast::system
