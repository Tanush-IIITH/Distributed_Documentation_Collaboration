[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/0ek2UV58)

## Build
- Install a POSIX build toolchain (tested with `gcc` and GNU Make on Linux).
- From the repository root run `make` to compile the name server, storage server, and client into `bin/`.
- Use `make clean` to remove build outputs, and `make rebuild` to force a fresh build.

## Run
1. **Start the name server** (choose an open port, e.g. `9000`):
	- `./bin/name_server 9000`
2. **Start at least one storage server** (run in its own terminal; `client_port` must be unique per instance):
	- `./bin/storage_server 127.0.0.1 9000 127.0.0.1 10000`
	- Arguments: `<ns_ip> <ns_port> <client_ip> <client_port>`
3. **Start a client** (one per user session):
	- `./bin/client alice 127.0.0.1 9000`
	- Arguments: `<username> <ns_ip> <ns_port>`

### Notes
- Default storage data lives under `ss_storage/`; ensure the directory is writable before launching storage servers.
- Logs (`name_server.log`, `storage_server.log`, `client.log`) accumulate in the repository root for troubleshooting.
- When running everything on one machine, keep each process in its own terminal so interactive prompts and logs remain readable.
