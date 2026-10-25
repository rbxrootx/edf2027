# Server Luau scripts

Place trusted `.luau` files in this directory. The server loads them in
filename order at startup. Scripts execute with server authority; only install
code you trust.

The optional example policy is at
`../../examples/server_owned_ant_combat.luau`. Copy it here to enable its
server-owned ant and custom trigger-based damage example. Restart the server
after adding or changing scripts.

Docker Compose mounts `server-data` read-only inside the container. Edit these
files on the host; the server only needs read access.
