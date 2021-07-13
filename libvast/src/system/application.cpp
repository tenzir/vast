//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2018 The VAST Contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "vast/system/application.hpp"

#include "vast/command.hpp"
#include "vast/config.hpp"
#include "vast/detail/assert.hpp"
#include "vast/detail/process.hpp"
#include "vast/documentation.hpp"
#include "vast/format/arrow.hpp"
#include "vast/format/ascii.hpp"
#include "vast/format/csv.hpp"
#include "vast/format/json.hpp"
#include "vast/format/json/default_selector.hpp"
#include "vast/format/json/suricata_selector.hpp"
#include "vast/format/json/zeek_selector.hpp"
#include "vast/format/null.hpp"
#include "vast/format/syslog.hpp"
#include "vast/format/test.hpp"
#include "vast/format/zeek.hpp"
#include "vast/plugin.hpp"
#include "vast/system/configuration.hpp"
#include "vast/system/count_command.hpp"
#include "vast/system/explore_command.hpp"
#include "vast/system/get_command.hpp"
#include "vast/system/import_command.hpp"
#include "vast/system/infer_command.hpp"
#include "vast/system/pivot_command.hpp"
#include "vast/system/remote_command.hpp"
#include "vast/system/start_command.hpp"
#include "vast/system/stop_command.hpp"
#include "vast/system/version_command.hpp"
#include "vast/system/writer_command.hpp"

namespace vast::system {

namespace {

command::opts_builder add_index_opts(command::opts_builder ob) {
  return std::move(ob)
    .add<size_t>("max-partition-size", "maximum number of events in a "
                                       "partition")
    .add<size_t>("max-resident-partitions", "maximum number of in-memory "
                                            "partitions")
    .add<size_t>("max-taste-partitions", "maximum number of immediately "
                                         "scheduled partitions")
    .add<size_t>("max-queries,q", "maximum number of concurrent queries");
}

command::opts_builder add_archive_opts(command::opts_builder ob) {
  return std::move(ob)
    .add<size_t>("segments,s", "number of cached segments")
    .add<size_t>("max-segment-size,m", "maximum segment size in MB");
}

auto make_count_command() {
  return std::make_unique<command>(
    "count", "count hits for a query without exporting data",
    documentation::vast_count,
    opts("?vast.count")
      .add<bool>("disable-taxonomies", "don't substitute taxonomy identifiers")
      .add<bool>("estimate,e", "estimate an upper bound by "
                               "skipping candidate checks"));
}

auto make_dump_command() {
  auto dump = std::make_unique<command>(
    "dump", "print configuration objects as JSON", documentation::vast_dump,
    opts("?vast.dump").add<bool>("yaml", "format output as YAML"));
  dump->add_subcommand("concepts", "print all registered concept definitions",
                       documentation::vast_dump_concepts,
                       opts("?vast.dump.concepts"));
  dump->add_subcommand("models", "print all registered model definitions",
                       documentation::vast_dump_models,
                       opts("?vast.dump.models"));
  return dump;
}

auto make_explore_command() {
  return std::make_unique<command>(
    "explore", "explore context around query results",
    documentation::vast_explore,
    opts("?vast.explore")
      .add<std::string>("format", "output format (default: JSON)")
      .add<std::string>("after,A", "include all records up to this much"
                                   " time after each result")
      .add<std::string>("before,B", "include all records up to this much"
                                    " time before each result")
      .add<std::string>("by", "perform an equijoin on the given field")
      .add<count>("max-events,n", "maximum number of results")
      .add<count>("max-events-query", "maximum results for initial query")
      .add<count>("max-events-context", "maximum results per exploration"));
}

auto make_export_command() {
  auto export_ = std::make_unique<command>(
    "export", "exports query results to STDOUT or file",
    documentation::vast_export,
    opts("?vast.export")
      .add<bool>("continuous,c", "marks a query as continuous")
      .add<bool>("unified,u", "marks a query as unified")
      .add<bool>("disable-taxonomies", "don't substitute taxonomy identifiers")
      .add<std::string>("timeout", "timeout to stop the export after")
      // We don't expose the `preserve-ids` option to the user because it
      // doesnt' affect the formatted output.
      //.add<bool>("preserve-ids", "don't substitute taxonomy identifiers")
      .add<size_t>("max-events,n", "maximum number of results")
      .add<std::string>("read,r", "path for reading the query")
      .add<std::string>("write,w", "path to write events to")
      .add<bool>("uds,d", "treat -w as UNIX domain socket to connect to"));
  export_->add_subcommand("zeek", "exports query results in Zeek format",
                          documentation::vast_export_zeek,
                          opts("?vast.export.zeek")
                            .add<bool>("disable-timestamp-tags",
                                       "whether the output should contain "
                                       "#open/#close tags"));
  export_->add_subcommand("csv", "exports query results in CSV format",
                          documentation::vast_export_csv,
                          opts("?vast.export.csv"));
  export_->add_subcommand("ascii", "exports query results in ASCII format",
                          documentation::vast_export_ascii,
                          opts("?vast.export.ascii"));
  export_->add_subcommand("json", "exports query results in JSON format",
                          documentation::vast_export_json,
                          opts("?vast.export.json")
                            .add<bool>("flatten", "flatten nested objects into "
                                                  "the top-level")
                            .add<bool>("numeric-durations",
                                       "render durations as numbers as opposed "
                                       "to human-readable strings"));
  export_->add_subcommand("null",
                          "exports query without printing them (debug option)",
                          documentation::vast_export_null,
                          opts("?vast.export.null"));
  export_->add_subcommand("arrow", "exports query results in Arrow format",
                          documentation::vast_export_arrow,
                          opts("?vast.export.arrow"));

  for (const auto& plugin : plugins::get()) {
    if (const auto* writer = plugin.as<writer_plugin>()) {
      auto opts_category
        = fmt::format("?vast.export.{}", writer->writer_format());
      export_->add_subcommand(writer->writer_format(), writer->writer_help(),
                              writer->writer_documentation(),
                              writer->writer_options(opts(opts_category)));
    }
  }
  return export_;
}

auto make_get_command() {
  return std::make_unique<command>(
    "get", "extracts the events assiciated with ids", documentation::vast_get,
    opts("?vast.get")
      .add<std::string>("format", "output format (default: JSON)"));
}

auto make_infer_command() {
  return std::make_unique<command>(
    "infer", "infers the schema from data", documentation::vast_infer,
    opts("?vast.infer")
      .add<size_t>("buffer,b", "maximum number of bytes to buffer")
      .add<std::string>("read,r", "path to the input data"));
}

auto make_kill_command() {
  return std::make_unique<command>("kill", "terminates a component", "",
                                   opts("?vast.kill"), false);
}

auto make_peer_command() {
  return std::make_unique<command>("peer", "peers with another node", "",
                                   opts("?vast.peer"), false);
}

auto make_pivot_command() {
  auto pivot = std::make_unique<command>(
    "pivot", "extracts related events of a given type",
    documentation::vast_pivot,
    opts("?vast.pivot")
      .add<size_t>("flush-interval,f", "flush to disk after this many packets "
                                       "(only with the PCAP plugin)")
      .add<bool>("disable-taxonomies", "don't substitute taxonomy identifiers")
      .add<std::string>("format", "output format "
                                  "(default: JSON)"));
  return pivot;
}

auto make_send_command() {
  return std::make_unique<command>("send",
                                   "sends a message to a registered actor", "",
                                   opts("?vast.send"), false);
}

auto make_spawn_source_command() {
  auto spawn_source = std::make_unique<command>(
    "source", "creates a new source inside the node",
    documentation::vast_spawn_source,
    opts("?vast.spawn.source")
      .add<std::string>("batch-encoding", "encoding type of table slices "
                                          "(arrow or msgpack)")
      .add<size_t>("batch-size", "upper bound for the size of a table slice")
      .add<std::string>("batch-timeout", "timeout after which batched "
                                         "table slices are forwarded")
      .add<std::string>("listen,l", "the endpoint to listen on "
                                    "([host]:port/type)")
      .add<size_t>("max-events,n", "the maximum number of events to import")
      .add<std::string>("read,r", "path to input where to read events from")
      .add<std::string>("read-timeout", "timeout for waiting for incoming data")
      .add<std::string>("schema,S", "alternate schema as string")
      .add<std::string>("schema-file,s", "path to alternate schema")
      .add<std::string>("type,t", "filter event type based on prefix matching")
      .add<bool>("uds,d", "treat -r as listening UNIX domain socket"));
  spawn_source->add_subcommand("csv",
                               "creates a new CSV source inside the node",
                               documentation::vast_spawn_source_csv,
                               opts("?vast.spawn.source.csv"));
  spawn_source->add_subcommand("json",
                               "creates a new JSON source inside the node",
                               documentation::vast_spawn_source_json,
                               opts("?vast.spawn.source.json"));
  spawn_source->add_subcommand("suricata",
                               "creates a new Suricata source inside the node",
                               documentation::vast_spawn_source_suricata,
                               opts("?vast.spawn.source.suricata"));
  spawn_source->add_subcommand("syslog",
                               "creates a new Syslog source inside the node",
                               documentation::vast_spawn_source_syslog,
                               opts("?vast.spawn.source.syslog"));
  spawn_source->add_subcommand(
    "test", "creates a new test source inside the node",
    documentation::vast_spawn_source_test,
    opts("?vast.spawn.source.test").add<size_t>("seed", "the PRNG seed"));
  spawn_source->add_subcommand("zeek",
                               "creates a new Zeek source inside the node",
                               documentation::vast_spawn_source_zeek,
                               opts("?vast.spawn.source.zeek"));
  for (const auto& plugin : plugins::get()) {
    if (const auto* reader = plugin.as<reader_plugin>()) {
      auto opts_category
        = fmt::format("?vast.spawn.source.{}", reader->reader_format());
      spawn_source->add_subcommand(reader->reader_format(),
                                   reader->reader_help(),
                                   reader->reader_documentation(),
                                   reader->reader_options(opts(opts_category)));
    }
  }
  return spawn_source;
}

auto make_spawn_sink_command() {
  auto spawn_sink = std::make_unique<command>(
    "sink", "creates a new sink", "",
    opts("?vast.spawn.sink")
      .add<std::string>("write,w", "path to write events to")
      .add<bool>("uds,d", "treat -w as UNIX domain socket"),
    false);
  spawn_sink->add_subcommand("zeek", "creates a new Zeek sink", "",
                             opts("?vast.spawn.sink.zeek"));
  spawn_sink->add_subcommand("ascii", "creates a new ASCII sink", "",
                             opts("?vast.spawn.sink.ascii"));
  spawn_sink->add_subcommand("csv", "creates a new CSV sink", "",
                             opts("?vast.spawn.sink.csv"));
  spawn_sink->add_subcommand("json", "creates a new JSON sink", "",
                             opts("?vast.spawn.sink.json"));
  for (const auto& plugin : plugins::get()) {
    if (const auto* writer = plugin.as<writer_plugin>()) {
      auto opts_category
        = fmt::format("?vast.spawn.sink.{}", writer->writer_format());
      spawn_sink->add_subcommand(writer->writer_format(), writer->writer_help(),
                                 writer->writer_documentation(),
                                 writer->writer_options(opts(opts_category)));
    }
  }
  return spawn_sink;
}

auto make_spawn_command() {
  auto spawn
    = std::make_unique<command>("spawn", "creates a new component",
                                documentation::vast_spawn, opts("?vast.spawn"));
  spawn->add_subcommand("accountant", "spawns the accountant", "",
                        opts("?vast.spawn.accountant"), false);
  spawn->add_subcommand("archive", "creates a new archive", "",
                        add_archive_opts(opts("?vast.spawn.archive")), false);
  spawn->add_subcommand(
    "explorer", "creates a new explorer", "",
    opts("?vast.spawn.explorer")
      .add<vast::duration>("after,A", "timebox after each result")
      .add<vast::duration>("before,B", "timebox before each result"),
    false);
  spawn->add_subcommand(
    "exporter", "creates a new exporter", "",
    opts("?vast.spawn.exporter")
      .add<bool>("continuous,c", "marks a query as continuous")
      .add<bool>("unified,u", "marks a query as unified")
      .add<uint64_t>("events,e", "maximum number of results"),
    false);
  spawn->add_subcommand("importer", "creates a new importer", "",
                        opts("?vast.spawn.importer"), false);
  spawn->add_subcommand("index", "creates a new index", "",
                        add_index_opts(opts("?vast.spawn.index")), false);
  spawn->add_subcommand(make_spawn_source_command());
  spawn->add_subcommand(make_spawn_sink_command());
  return spawn;
}

auto make_status_command() {
  return std::make_unique<command>(
    "status", "shows properties of a server process",
    documentation::vast_status,
    opts("?vast.status")
      .add<bool>("detailed", "add more information to the output")
      .add<bool>("debug", "include extra debug information"));
}

auto make_start_command() {
  return std::make_unique<command>(
    "start", "starts a node", documentation::vast_start,
    opts("?vast.start")
      .add<bool>("print-endpoint", "print the client endpoint on stdout")
      .add<std::vector<std::string>>("commands", "an ordered list of commands "
                                                 "to run inside the node after "
                                                 "starting")
      .add<size_t>("disk-budget-check-interval", "time between two disk size "
                                                 "scans")
      .add<std::string>("disk-budget-check-binary",
                        "binary to run to determine current disk usage")
      .add<std::string>("disk-budget-high", "high-water mark for disk budget")
      .add<std::string>("disk-budget-low", "low-water mark for disk budget")
      .add<size_t>("disk-budget-step-size", "number of partitions to erase "
                                            "before re-checking size"));
}

auto make_stop_command() {
  return std::make_unique<command>(
    "stop", "stops a node", documentation::vast_stop, opts("?vast.stop"));
}

auto make_version_command() {
  return std::make_unique<command>("version", "prints the software version",
                                   documentation::vast_version,
                                   opts("?vast.version"));
}

auto make_command_factory() {
  // When updating this list, remember to update its counterpart in node.cpp as
  // well iff necessary
  // clang-format off
  auto result = command::factory{
    {"count", count_command},
    {"dump", remote_command},
    {"dump concepts", remote_command},
    {"dump models", remote_command},
    {"explore", explore_command},
    {"export ascii", make_writer_command("ascii")},
    {"export csv", make_writer_command("csv")},
    {"export json", make_writer_command("json")},
    {"export null", make_writer_command("null")},
    {"export arrow", make_writer_command("arrow")},
    {"export zeek", make_writer_command("zeek")},
    {"get", get_command},
    {"infer", infer_command},
    {"import csv", import_command},
    {"import json", import_command},
    {"import suricata", import_command},
    {"import syslog", import_command},
    {"import test", import_command},
    {"import zeek", import_command},
    {"import zeek-json", import_command},
    {"kill", remote_command},
    {"peer", remote_command},
    {"pivot", pivot_command},
    {"send", remote_command},
    {"spawn accountant", remote_command},
    {"spawn archive", remote_command},
    {"spawn eraser", remote_command},
    {"spawn exporter", remote_command},
    {"spawn explorer", remote_command},
    {"spawn importer", remote_command},
    {"spawn type-registry", remote_command},
    {"spawn index", remote_command},
    {"spawn meta-index", remote_command},
    {"spawn sink ascii", remote_command},
    {"spawn sink csv", remote_command},
    {"spawn sink json", remote_command},
    {"spawn sink zeek", remote_command},
    {"spawn source csv", remote_command},
    {"spawn source json", remote_command},
    {"spawn source suricata", remote_command},
    {"spawn source syslog", remote_command},
    {"spawn source test", remote_command},
    {"spawn source zeek", remote_command},
    {"spawn source zeek-json", remote_command},
    {"start", start_command},
    {"status", remote_command},
    {"stop", stop_command},
    {"version", version_command},
  };
  // clang-format on
  for (auto& plugin : plugins::get()) {
    if (auto* reader = plugin.as<reader_plugin>()) {
      result.emplace(fmt::format("import {}", reader->reader_format()),
                     import_command);
      result.emplace(fmt::format("spawn source {}", reader->reader_format()),
                     remote_command);
    }
    if (auto* writer = plugin.as<writer_plugin>()) {
      result.emplace(fmt::format("export {}", writer->writer_format()),
                     make_writer_command(writer->writer_format()));
      result.emplace(fmt::format("spawn sink {}", writer->writer_format()),
                     remote_command);
    }
  }
  return result;
} // namespace

auto make_root_command(std::string_view path) {
  // We're only interested in the application name, not in its path. For
  // example, argv[0] might contain "./build/release/bin/vast" and we are only
  // interested in "vast".
  path.remove_prefix(std::min(path.find_last_of('/') + 1, path.size()));
  // For documentation, we use the complete man-page formatted as Markdown
  const auto binary = detail::objectpath();
  auto schema_desc
    = "list of directories to look for schema files ([/etc/vast/schema"s;
  if (binary) {
    const auto relative_schema_dir
      = binary->parent_path().parent_path() / "share" / "vast" / "schema";
    schema_desc += ", " + relative_schema_dir.string();
  }
  schema_desc += "])";
  auto ob
    = opts("?vast")
        .add<std::string>("config", "path to a configuration file")
        .add<bool>("bare-mode",
                   "disable user and system configuration, schema and plugin "
                   "directories lookup and static and dynamic plugin "
                   "autoloading (this may only be used on the command line)")
        .add<caf::atom_value>("verbosity", "output verbosity level on the "
                                           "console")
        .add<std::vector<std::string>>("schema-dirs", schema_desc.c_str())
        .add<std::string>("db-directory,d", "directory for persistent state")
        .add<std::string>("log-file", "log filename")
        .add<std::string>("client-log-file", "client log file (default: "
                                             "disabled)")
        .add<std::string>("endpoint,e", "node endpoint")
        .add<std::string>("node-id,i", "the unique ID of this node")
        .add<bool>("node,N", "spawn a node instead of connecting to one")
        .add<bool>("enable-metrics", "keep track of performance metrics")
        .add<std::vector<std::string>>("plugin-dirs", "additional directories "
                                                      "to load plugins from")
        .add<std::vector<std::string>>(
          "plugins", "plugins to load at startup; the special values 'bundled' "
                     "and 'all' enable autoloading of bundled and all plugins "
                     "respectively.")
        .add<std::string>("aging-frequency", "interval between two aging "
                                             "cycles")
        .add<std::string>("aging-query", "query for aging out obsolete data")
        .add<std::string>("shutdown-grace-period",
                          "time to wait until component shutdown "
                          "finishes cleanly before inducing a hard kill")
        .add<std::string>("store-backend", "store plugin to use for imported "
                                           "data");
  ob = add_index_opts(std::move(ob));
  ob = add_archive_opts(std::move(ob));
  auto root
    = std::make_unique<command>(path, "", documentation::vast, std::move(ob));
  root->add_subcommand(make_count_command());
  root->add_subcommand(make_dump_command());
  root->add_subcommand(make_export_command());
  root->add_subcommand(make_explore_command());
  root->add_subcommand(make_get_command());
  root->add_subcommand(make_infer_command());
  root->add_subcommand(make_import_command());
  root->add_subcommand(make_kill_command());
  root->add_subcommand(make_peer_command());
  root->add_subcommand(make_pivot_command());
  root->add_subcommand(make_send_command());
  root->add_subcommand(make_spawn_command());
  root->add_subcommand(make_start_command());
  root->add_subcommand(make_status_command());
  root->add_subcommand(make_stop_command());
  root->add_subcommand(make_version_command());
  return root;
}

} // namespace

std::unique_ptr<command> make_import_command() {
  auto import_ = std::make_unique<command>(
    "import", "imports data from STDIN or file", documentation::vast_import,
    opts("?vast.import")
      .add<std::string>("batch-encoding", "encoding type of table slices "
                                          "(arrow or msgpack)")
      .add<size_t>("batch-size", "upper bound for the size of a table slice")
      .add<std::string>("batch-timeout", "timeout after which batched "
                                         "table slices are forwarded")
      .add<bool>("blocking,b", "block until the IMPORTER forwarded all data")
      .add<std::string>("listen,l", "the endpoint to listen on "
                                    "([host]:port/type)")
      .add<size_t>("max-events,n", "the maximum number of events to import")
      .add<std::string>("read,r", "path to input where to read events from")
      .add<std::string>("read-timeout", "timeout for waiting for incoming data")
      .add<std::string>("schema,S", "alternate schema as string")
      .add<std::string>("schema-file,s", "path to alternate schema")
      .add<std::string>("type,t", "filter event type based on prefix matching")
      .add<bool>("uds,d", "treat -r as listening UNIX domain socket"));
  import_->add_subcommand("zeek", "imports Zeek TSV logs from STDIN or file",
                          documentation::vast_import_zeek,
                          opts("?vast.import.zeek"));
  import_->add_subcommand("zeek-json",
                          "imports Zeek JSON logs from STDIN or file",
                          documentation::vast_import_zeek,
                          opts("?vast.import.zeek-json"));
  import_->add_subcommand("csv", "imports CSV logs from STDIN or file",
                          documentation::vast_import_csv,
                          opts("?vast.import.csv"));
  import_->add_subcommand("json", "imports JSON with schema",
                          documentation::vast_import_json,
                          opts("?vast.import.json"));
  import_->add_subcommand("suricata", "imports suricata eve json",
                          documentation::vast_import_suricata,
                          opts("?vast.import.suricata"));
  import_->add_subcommand("syslog", "imports syslog messages",
                          documentation::vast_import_syslog,
                          opts("?vast.import.syslog"));
  import_->add_subcommand(
    "test", "imports random data for testing or benchmarking",
    documentation::vast_import_test,
    opts("?vast.import.test").add<size_t>("seed", "the PRNG seed"));
  for (const auto& plugin : plugins::get()) {
    if (const auto* reader = plugin.as<reader_plugin>()) {
      auto opts_category
        = fmt::format("?vast.import.{}", reader->reader_format());
      import_->add_subcommand(reader->reader_format(), reader->reader_help(),
                              reader->reader_documentation(),
                              reader->reader_options(opts(opts_category)));
    }
  }
  return import_;
}

std::pair<std::unique_ptr<command>, command::factory>
make_application(std::string_view path) {
  auto root = make_root_command(path);
  auto root_factory = make_command_factory();
  // Add additional commands from plugins.
  for (auto& plugin : plugins::get()) {
    if (auto* cp = plugin.as<command_plugin>()) {
      auto&& [cmd, cmd_factory] = cp->make_command();
      if (!cmd || cmd_factory.empty())
        continue;
      root->add_subcommand(std::move(cmd));
      root_factory.insert(std::make_move_iterator(cmd_factory.begin()),
                          std::make_move_iterator(cmd_factory.end()));
    }
  }
  return {std::move(root), std::move(root_factory)};
}

void render_error(const command& root, const caf::error& err,
                  std::ostream& os) {
  if (!err)
    // The user most likely killed the process via CTRL+C, print nothing.
    return;
  os << render(err) << '\n';
  if (err.category() == caf::atom("vast")) {
    auto x = static_cast<vast::ec>(err.code());
    switch (x) {
      default:
        break;
      case ec::invalid_subcommand:
      case ec::missing_subcommand:
      case ec::unrecognized_option: {
        auto ctx = err.context();
        if (ctx.match_element<std::string>(1)) {
          auto name = ctx.get_as<std::string>(1);
          if (auto cmd = resolve(root, name))
            helptext(*cmd, os);
        } else {
          VAST_ASSERT(!"User visible error contexts must consist of strings!");
        }
        break;
      }
    }
  }
}

command::opts_builder opts(std::string_view category) {
  return command::opts(category);
}

} // namespace vast::system
