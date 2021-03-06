//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2016 The VAST Contributors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "vast/detail/endian.hpp"

#include <cstddef>
#include <cstdint>

namespace vast {

/// The [CRC32](http://en.wikipedia.org/wiki/Cyclic_redundancy_check) algorithm.
class crc32 {
public:
  static constexpr detail::endianness endian = detail::host_endian;
  using result_type = uint32_t;

  crc32(uint32_t seed = 0);

  void operator()(const void* x, size_t n);

  operator result_type() const;

  template <class Inspector>
  friend auto inspect(Inspector& f, crc32& crc) {
    return f(crc.digest_);
  }

private:
  result_type digest_;
};

} // namespace vast
