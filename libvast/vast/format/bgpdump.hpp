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

#include "vast/concept/parseable/core.hpp"
#include "vast/concept/parseable/string.hpp"
#include "vast/concept/parseable/numeric.hpp"
#include "vast/concept/parseable/vast/data.hpp"
#include "vast/event.hpp"
#include "vast/format/parser_reader.hpp"
#include "vast/schema.hpp"

namespace vast::format::bgpdump {

/// A parser that reading ASCII output from the BGPDump utility.
struct bgpdump_parser : parser<bgpdump_parser> {
  using attribute = event;

  bgpdump_parser();

  template <class Iterator>
  bool parse(Iterator& f, Iterator& l, event& e) const {
    using parsers::addr;
    using parsers::any;
    using parsers::net;
    using parsers::u64;
    using namespace std::chrono;
    static auto str = +(any - '|');
    static auto time = u64->*[](count x) { return vast::time{seconds(x)}; };
    static auto head
      = "BGP4MP|" >> time >> '|' >> str >> '|' >> addr >> '|' >> u64 >> '|';
    vast::time ts;
    std::string update;
    vast::address source_ip;
    count source_as;
    if (!head(f, l, ts, update, source_ip, source_as))
      return false;
    vector v;
    v.emplace_back(ts);
    v.emplace_back(std::move(source_ip));
    v.emplace_back(source_as);
    if (update == "A" || update == "B") {
      // Announcement or routing table entry
      static auto num = u64->*[](count x) { return data{x}; };
      static auto tail =
        net >> '|' >> (num % ' ') >> -(" {" >> u64 >> '}') >> '|'
            >> str >> '|' >> addr >> '|' >> u64 >> '|' >> u64 >> '|'
            >> -str >> '|' >> -str >> '|' >> -str;
      subnet sn;
      std::vector<data> as_path;
      optional<count> origin_as;
      std::string origin;
      vast::address nexthop;
      count local_pref;
      count med;
      optional<std::string> community;
      optional<std::string> atomic_aggregate;
      optional<std::string> aggregator;
      auto t = std::tie(sn, as_path, origin_as, origin, nexthop, local_pref,
                        med, community, atomic_aggregate, aggregator);
      if (!tail(f, l, t))
        return {};
      v.emplace_back(std::move(sn));
      v.emplace_back(vector(std::move(as_path)));
      v.emplace_back(std::move(origin_as));
      v.emplace_back(std::move(origin));
      v.emplace_back(nexthop);
      v.emplace_back(local_pref);
      v.emplace_back(med);
      v.emplace_back(std::move(community));
      v.emplace_back(std::move(atomic_aggregate));
      v.emplace_back(std::move(aggregator));
      e = event{{std::move(v), update == "A" ? announce_type : route_type}};
      e.timestamp(ts);
    } else if (update == "W") {
      subnet sn;
      if (!net(f, l, sn))
        return {};
      v.emplace_back(sn);
      e = event{{std::move(v), withdraw_type}};
      e.timestamp(ts);
    } else if (update == "STATE") {
      static auto tail = -str >> '|' >> -str;
      optional<std::string> old_state;
      optional<std::string> new_state;
      if (!tail(f, l, old_state, new_state))
        return false;
      v.emplace_back(std::move(old_state));
      v.emplace_back(std::move(new_state));
      e = event{{std::move(v), state_change_type}};
      e.timestamp(ts);
    } else {
      return false;
    }
    return true;
  }

  type announce_type;
  type route_type;
  type withdraw_type;
  type state_change_type;
};

class reader : public parser_reader<bgpdump_parser> {
public:
  using parser_reader<bgpdump_parser>::parser_reader;

  caf::error schema(vast::schema sch) override;

  vast::schema schema() const override;

  const char* name() const override;
};

} // namespace vast::format::bgpdump
