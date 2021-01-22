/******************************************************************************
 *                    _   _____   __________                                  *
 *                   | | / / _ | / __/_  __/     Visibility                   *
 *                   | |/ / __ |_\ \  / /          Across                     *
 *                   |___/_/ |_/___/ /_/       Space and Time                 *
 *                                                                            *
 * This file is part of VAST. It is subject to the license terms in the       *
 * LICENSE file found in the top-level directory of this distribution and at  *
 * http://vast.io/license. No part of VAST, including this file, may be       *
 * copied, modified, propagated, or distributed except according to the terms *
 * contained in the LICENSE file.                                             *
 ******************************************************************************/

#pragma once

#include "vast/fwd.hpp"

#include "vast/concept/hashable/hash_append.hpp"
#include "vast/concept/hashable/xxhash.hpp"
#include "vast/defaults.hpp"
#include "vast/detail/flat_map.hpp"
#include "vast/detail/line_range.hpp"
#include "vast/detail/string.hpp"
#include "vast/error.hpp"
#include "vast/format/bench.hpp"
#include "vast/format/multi_layout_reader.hpp"
#include "vast/format/ostream_writer.hpp"
#include "vast/logger.hpp"
#include "vast/schema.hpp"
#include "vast/view.hpp"

#include <caf/expected.hpp>
#include <caf/fwd.hpp>
#include <caf/settings.hpp>

#include <chrono>
#include <simdjson.h>

namespace vast::format::simdjson {

/// Adds a JSON object to a table slice builder according to a given layout.
/// @param builder The builder to add the JSON object to.
/// @param xs The JSON object to add to *builder.
/// @param layout The record type describing *xs*.
/// @returns An error iff the operation failed.
caf::error add(table_slice_builder& bptr, const ::simdjson::dom::object& xs,
               const record_type& layout);

/// A reader for JSON data. It operates with a *selector* to determine the
/// mapping of JSON object to the appropriate record type in the schema.
template <class Selector, class BenchmarkMixin>
class reader final : public multi_layout_reader {
public:
  using super = multi_layout_reader;

  /// Constructs a JSON reader.
  /// @param table_slice_type The ID for table slice type to build.
  /// @param options Additional options.
  /// @param in The stream of JSON objects.
  reader(const caf::settings& options, std::unique_ptr<std::istream> in
                                       = nullptr);

  void reset(std::unique_ptr<std::istream> in);

  caf::error schema(vast::schema sch) override;

  vast::schema schema() const override;

  const char* name() const override;

  vast::system::report status() const override;

protected:
  caf::error
  read_impl(size_t max_events, size_t max_slice_size, consumer& f) override;

private:
  using iterator_type = std::string_view::const_iterator;

  Selector selector_;
  std::unique_ptr<std::istream> input_;

  // https://simdjson.org/api/0.7.0/classsimdjson_1_1dom_1_1parser.html
  // Parser is designed to be reused.
  ::simdjson::dom::parser json_parser_;

  std::unique_ptr<detail::line_range> lines_;
  caf::optional<size_t> proto_field_;
  std::vector<size_t> port_fields_;
  mutable size_t num_invalid_lines_ = 0;
  mutable size_t num_unknown_layouts_ = 0;
  mutable size_t num_lines_ = 0;
  // For non benchmark setup size for value can be optimized out when C++20 is
  // available
  // https://en.cppreference.com/w/cpp/language/attributes/no_unique_address
  BenchmarkMixin benchmark_;
};

// -- implementation ----------------------------------------------------------

template <class Selector, class BenchmarkMixin>
reader<Selector, BenchmarkMixin>::reader(const caf::settings& options,
                                         std::unique_ptr<std::istream> in)
  : super(options) {
  if (in != nullptr)
    reset(std::move(in));
}

template <class Selector, class BenchmarkMixin>
void reader<Selector, BenchmarkMixin>::reset(std::unique_ptr<std::istream> in) {
  VAST_ASSERT(in != nullptr);
  input_ = std::move(in);
  lines_ = std::make_unique<detail::line_range>(*input_);
}

template <class Selector, class BenchmarkMixin>
caf::error reader<Selector, BenchmarkMixin>::schema(vast::schema s) {
  return selector_.schema(std::move(s));
}

template <class Selector, class BenchmarkMixin>
vast::schema reader<Selector, BenchmarkMixin>::schema() const {
  return selector_.schema();
}

template <class Selector, class BenchmarkMixin>
const char* reader<Selector, BenchmarkMixin>::name() const {
  return "json-reader";
}

template <class Selector, class BenchmarkMixin>
vast::system::report reader<Selector, BenchmarkMixin>::status() const {
  using namespace std::string_literals;
  uint64_t invalid_line = num_invalid_lines_;
  uint64_t unknown_layout = num_unknown_layouts_;
  if (num_invalid_lines_ > 0)
    VAST_WARNING(this, "failed to parse", num_invalid_lines_, "of", num_lines_,
                 "recent lines");
  if (num_unknown_layouts_ > 0)
    VAST_WARNING(this, "failed to find a matching type for",
                 num_unknown_layouts_, "of", num_lines_, "recent lines");
  num_invalid_lines_ = 0;
  num_unknown_layouts_ = 0;
  num_lines_ = 0;
  return {
    {name() + ".invalid-line"s, invalid_line},
    {name() + ".unknown-layout"s, unknown_layout},
  };
}

template <class Selector, class BenchmarkMixin>
caf::error reader<Selector, BenchmarkMixin>::read_impl(size_t max_events,
                                                       size_t max_slice_size,
                                                       consumer& cons) {
  VAST_TRACE(VAST_ARG(max_events), VAST_ARG(max_slice_size));
  VAST_ASSERT(max_events > 0);
  VAST_ASSERT(max_slice_size > 0);
  size_t produced = 0;
  table_slice_builder_ptr bptr = nullptr;
  while (produced < max_events) {
    if (lines_->done())
      return finish(cons, caf::make_error(ec::end_of_input, "input exhausted"));
    if (batch_events_ > 0 && batch_timeout_ > reader_clock::duration::zero()
        && last_batch_sent_ + batch_timeout_ < reader_clock::now()) {
      VAST_DEBUG(this, "reached batch timeout");
      return finish(cons, ec::timeout);
    }
    auto it_tracker = benchmark_.make_iteration_tracker();
    bool timed_out = lines_->next_timeout(read_timeout_);
    if (timed_out) {
      VAST_DEBUG(this, "stalled at line", lines_->line_number());
      return ec::stalled;
    }
    auto& line = lines_->get();
    ++num_lines_;
    if (line.empty()) {
      // Ignore empty lines.
      VAST_DEBUG(this, "ignores empty line at", lines_->line_number());
      continue;
    }
    it_tracker.next_step();
    auto parse_result = json_parser_.parse(line);
    if (parse_result.error() != ::simdjson::error_code::SUCCESS) {
      if (num_invalid_lines_ == 0)
        VAST_WARNING(this, "failed to parse line", lines_->line_number(), ":",
                     line);
      ++num_invalid_lines_;
      continue;
    }
    auto get_object_result = parse_result.get_object();
    if (get_object_result.error() != ::simdjson::error_code::SUCCESS)
      return caf::make_error(ec::type_clash, "not a json object");
    it_tracker.next_step();
    auto&& layout = selector_(get_object_result.value());
    if (!layout) {
      if (num_unknown_layouts_ == 0)
        VAST_WARNING(this, "failed to find a matching type at line",
                     lines_->line_number(), ":", line);
      ++num_unknown_layouts_;
      continue;
    }
    it_tracker.next_step();
    bptr = builder(*layout);
    if (bptr == nullptr)
      return caf::make_error(ec::parse_error, "unable to get a builder");
    if (auto err = add(*bptr, get_object_result.value(), *layout)) {
      err.context() += caf::make_message("line", lines_->line_number());
      return finish(cons, err);
    }
    it_tracker.next_step();
    produced++;
    batch_events_++;
    if (bptr->rows() == max_slice_size)
      if (auto err = finish(cons, bptr))
        return err;
  }
  return finish(cons);
}

} // namespace vast::format::simdjson
