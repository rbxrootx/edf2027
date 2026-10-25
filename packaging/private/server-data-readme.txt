Put trusted server-side .luau scripts in this folder's scripts subdirectory.
The private test installer includes server_owned_ant_combat.luau there and
loads it on server start. The server creates identity.edfkey here on first
start; preserve it across restarts so clients keep recognizing this server.
