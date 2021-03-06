Introduction
============

Walbouncer provides a proxy server for PostgreSQL replication connections. It
also has the capability to replace a subset of WAL records will no-ops.

Usecases where you would use walbouncer:
    
- For clusters with more than two slaves you can change a slave servers
  effective `primary_conninfo` without restarting PostgreSQL by proxying
  it through walbouncer and changing walbouncer config when master server
  location changes.
  
  In this setup you would usually place walbouncer on the same host as the
  slave.

- You can choose to replicate a subset of data to save space on geographically
  distributed databases. Use a separate tablespace per location and configure
  walbouncer to filter out irrelevant data from the WAL stream.
  
  To also save on bandwidth use vtun to compress the stream.

NB: filtering the WAL stream is by definition introducing data loss to your
system. Slaves with filtered data are not usable after promotion to master.
You may encounter errors about missing files that prevent you from using
the database.

Building and installing
=======================

To build walbouncer you need to have libyaml-dev and PostgreSQL 9.4 installed
on your system. The correct PostgreSQL version is located using the pg_config
binary. Ensure that pg_config for PostgreSQL 9.4 is in your path.

To build walbouncer change into the src/ directory and run:

    make

Walbouncer is built as a selfcontained binary. It can be installed into your
PostgreSQL binaries directory using:

    make install

You can run a selftest procedure by running `make test`. Ensure that you have
ports 5432..5434 free and don't have any necessary walbouncer running. The test
run will leave two PostgreSQL instances and walbouncer running for further
experimentation.

Using walbouncer
================

Running
-------

Walbouncer is started by either providing essential configuration options on
the command line:

    walbouncer --port=5433 --host=master.example.com

For advanced use cases you need to provide a config file in YAML format:

    walbouncer -c path/to/myconfig.yaml

No built-in daemonization or init scripts are currently included. Log output
from walbouncer goes to stderr. You can use nohup or daemonize to run it in
the background.

Configuration file
------------------

The configuration file is encoded in YAML format. The following directives are
used:

```yaml    
# The port that walbouncer will listen on.
listen_port: 5433

# Connection settings for the replication master server
master:
    host: localhost
    port: 5432

# A list of configurations, each one a one entry mapping with the key
# specifying a name for the configuration. First matching configuration
# is chosen. If none of the configurations match the client is denied access.
configurations:
    # Name of the configuration
    - examplereplica1:
        # Conditions under which this configuration matches. All of the entries
        # must match.
        match:
            # Check application_name of the client for an exact match.
            application_name: slave1
            # Matches the IP address the client is connecting from. Can be a
            # specific IP or a hostmask
            source_ip: 192.168.0.0/16
        # Filter clauses can be omitted if filtering is not necessary. A record
        # is replicated if all of the include directives match and none of the
        # exclude directives match.
        filter:
            # If specified only tablespaces named in this list and default
            # tablespaces (pg_default, pg_global) are replicated.
            include_tablespaces: [spc_slave1, spc_slave2]
            # If specified tablespaces named in this list are not replicated.
            exclude_tablespaces:
                - spc_slave3 # Can also use alternate list syntax
            # If specified only databases in this list and template databases
            # are replicated
            include_databases: [postgres]
            # If specified databases in this list are skipped.
            exclude_databases: [test]
    # Second configuration
    - examplereplica2:
        match:
            application_name: slave2
        filter:
            include_tablespaces: [spc_slave2]
```

Additional Information
======================

PDF on general concepts [here](http://static.cybertec.at/wp-content/uploads/walbouncer.pdf)

Blog posts on Walbouncer: [post1](http://www.cybertec.at/2014/10/walbouncer-filtering-transaction-log/), [post2](http://www.cybertec.at/2016/08/walbouncer-refreshed-a-proxy-for-selective-postgresql-physical-replication/)


Gotchas
=======

Adding/renaming databases
-------------------------

List of databases used for filtering (OID-s are fetched from master on client connect) is only read on Walbouncer startup so to make sure the new/renamed
database will be filtered out on the replica, you need to stop the replica and the Walbouncer, adjust the Walbouncer config, issue the CREATE/RENAME
command, start Walbouncer and the replica. See above PDF for more details.

Dropping databases
------------------

Have all slaves that want to filter out the dropped database actively streaming before you execute the drop. 
Otherwise the slaves will not know to skip the drop record and xlog replay will fail with an error.


Potential future features
=========================

- Also provide filtering for pg_basebackup.
- Provide quorum for synchronous replication. (k of n servers have the data)
- Create a protocol to use multicast to stream data.

Pull requests and any other input are very welcome!
