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

// -- v1 includes --------------------------------------------------------------

#include "vast/fwd.hpp"
#include "vast/table_slice.hpp"
#include "vast/view.hpp"

// -- v0 includes --------------------------------------------------------------

#include "vast/fwd.hpp"

#include <caf/fwd.hpp>
#include <caf/intrusive_cow_ptr.hpp>

#include <vector>

#include <vast/byte.hpp>
#include <vast/chunk.hpp>
#include <vast/span.hpp>
#include <vast/table_slice.hpp>
#include <vast/view.hpp>

namespace vast {

// -- forward-declarations -----------------------------------------------------

namespace fbs::table_slice::msgpack {

struct v0;

} // namespace fbs::table_slice::msgpack

namespace v1 {

/// A table slice that stores elements encoded in
/// [MessagePack](https://msgpack.org) format. The implementation stores data
/// in row-major order.
class msgpack_table_slice final {
public:
  // -- constructors, destructors, and assignment operators --------------------

  msgpack_table_slice(const fbs::table_slice::msgpack::v0& slice) noexcept;
  ~msgpack_table_slice() noexcept;

  // -- table slice facade------------------------------------------------------

  table_slice::size_type rows() const noexcept;

  table_slice::size_type columns() const noexcept;

  const record_type& layout() const noexcept;

  data_view
  at(table_slice::size_type row, table_slice::size_type column) const noexcept;

  void append_column_to_index(id offset, table_slice::size_type column,
                              value_index& idx) const;

private:
  // -- implementation details -------------------------------------------------

  /// A reference to the underlying FlatBuffers table.
  const fbs::table_slice::msgpack::v0& slice_;

  /// The layout of the slice.
  record_type layout_ = {};
};

} // namespace v1

inline namespace v0 {

class msgpack_table_slice;
class msgpack_table_slice_builder;

/// @relates msgpack_table_slice
using msgpack_table_slice_ptr = caf::intrusive_cow_ptr<msgpack_table_slice>;

/// A table slice that stores elements encoded in
/// [MessagePack](https://msgpack.org) format. The implementation stores data
/// in row-major order.
class msgpack_table_slice final : public vast::table_slice {
public:
  // -- friends ----------------------------------------------------------------

  friend msgpack_table_slice_builder;

  // -- constants --------------------------------------------------------------

  static constexpr caf::atom_value class_id = caf::atom("msgpack");

  // -- member types -----------------------------------------------------------

  /// Base type.
  using super = vast::table_slice;

  /// Unsigned integer type.
  using size_type = super::size_type;

  // -- factories --------------------------------------------------------------

  static vast::table_slice_ptr make(vast::table_slice_header header);

  // -- properties -------------------------------------------------------------

  msgpack_table_slice* copy() const override;

  caf::error serialize(caf::serializer& sink) const override;

  caf::error deserialize(caf::deserializer& source) override;

  caf::error load(vast::chunk_ptr chunk) override;

  void
  append_column_to_index(size_type col, vast::value_index& idx) const override;

  caf::atom_value implementation_id() const noexcept override;

  vast::data_view at(size_type row, size_type col) const override;

private:
  using table_slice::table_slice;

  /// Offsets from the beginning of the buffer to each row.
  std::vector<size_t> offset_table_;

  /// The buffer that contains the MessagePack data.
  vast::span<const vast::byte> buffer_;

  /// The table slice after the header.
  vast::chunk_ptr chunk_;
};

} // namespace v0

} // namespace vast
