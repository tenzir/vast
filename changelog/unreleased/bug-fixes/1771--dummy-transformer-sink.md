Import processes now respond quicker. Requests to shut down when the
server process exits are no longer delayed for busy imports, and metrics
and telemetry reports are now written in a timely manner.

Particularly busy imports caused the shutdown of the server process to hang, if
the import processes were still running, or had not yet flushed all data. The
server now shuts down correctly in these cases.
