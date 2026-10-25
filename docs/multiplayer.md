# Multiplayer runtime

The source tree builds a standalone, headless ENet server and an optional
server directory. The server executable does not load the Xbox executable, the
ReXGlue runtime, or game assets. Its protocol uses bounded, versioned packets.
Standalone servers default to relaying each client's locally simulated soldier
transform; this is client-authoritative movement. Hosts can enable
`--server-authoritative-movement` to make the server ignore client coordinates
and simulate horizontal movement from accepted input on a fixed 60 Hz tick.
In client-transform mode, the server rejects pose deltas above its configurable
speed ceiling (default 60 m/s, set with `--max-client-speed`) using server receive
time and the last accepted pose. This is a teleport outlier guard; the host still
does not validate collision or terrain movement.
Because mission spawn points are not mapped into server coordinates yet, the
first active client transform establishes that player's starting pose and is not
speed-checked. Later client transforms are checked against that accepted pose.
Guest-backed host transforms come from the host guest and cannot be overwritten
by a remote client's transform packet. The rendered F5 local host currently
uses client-transform mode so each EDF guest keeps its real mission spawn,
collision, and movement physics. The host publishes its own copied soldier
pose from the game thread; remote clients send their locally simulated poses,
which the server bounds and relays. The server worker never reads guest memory.
When a remote peer enters a mission hosted through F5, the host creates and
validates EDF's native player 1 object, then routes that peer's authenticated
input into the game's normal XInput path. The host's own controls stay on player
0; guest creation and input sampling stay on EDF's simulation thread, and stale
input is neutralized after 250 ms while the seat remains attached through a
brief packet stall; disconnect immediately releases it. EDF's native
simulation has only two player seats, so one remote peer can currently use this
host-side gameplay path; the host samples that soldier's live pose as a trusted
transform, so the remote client's position packet cannot override the F5 host's
seat. Additional connected peers remain networked replicas. The host-side seat
uses EDF's own collision, weapons, and mission simulation, but this does not make
standalone dedicated servers server-authoritative.
The mission-world check is separate from the primary soldier's alive check, so
an owned player 1 seat can remain active while player 0 is down. Pose sampling
validates the requested seat independently, allowing a living remote seat to
keep publishing its transform during the host's respawn gap. A downed remote
pilot stays attached to that seat, but the host injects neutral controls while
the death transition completes because the native revive mapping is not
verified. It adopts an EDF replacement after observing the old seat disappear;
if EDF leaves the index-1 seat empty, the bridge waits at least five seconds
and spawns a fresh player beside the host's primary soldier once that soldier
is alive. That fallback respawn is bridge-managed rather than EDF's native
revive flow.
Clients interpolate remote player replicas. The optional
`--server-authoritative-movement` mode remains available for servers configured
with a valid mission spawn. It simulates horizontal movement and owns player
vitals that trusted Luau scripts can damage, but it does not provide EDF terrain
collision, vertical movement, weapons, enemy AI, or mission logic.
For diagnostics, set `edf_mp_owner_correction_trace=true` in the game console
to log the local owner's position and wrapped-yaw correction error every ten
seconds while an authoritative pose is active. The trace is read-only; it does
not smooth or alter the local soldier's physics state. Disable it after
collecting a short walk/jump/fall sample to keep logs concise.
The headless guest-backed host sends an interest-limited actor snapshot for
server-owned ant spawns. The rendered F5 local host additionally publishes up
to 48 exact-key mission-ant pose and health records at 20 Hz, and sends host
removals reliably. Guest memory is sampled and mutated on the game thread;
server scripts and networking receive copied records and bounded damage queues.
Trusted host scripts can inspect these mission ants and choose custom damage.
Self-hosts can configure up to 512 player slots and the replication radius.
Unauthenticated ENet peers use a separate bounded handshake pool, capped at 32
or the configured player count, whichever is lower, so they cannot consume all
authenticated player slots while waiting on the protocol handshake.
The F5 browser discovers
LAN servers, joins a host name or IPv4 address directly, and can query a
self-hosted Internet directory. Hosts can configure up to 512 player slots.
**Cancel join** and closing the browser cancel only a still-pending connection;
if the server's welcome arrives at the same time, the established session is
kept alive when the browser closes. Cancel never waits for DNS or transport
shutdown on the UI or game thread. A new join is refused until a canceled
connection worker has finished stopping, so repeated retries cannot replace a
worker that is still resolving or connecting. Windows uses cancellable
`GetAddrInfoEx`; platforms whose resolver only offers blocking `getaddrinfo`
may keep that background worker alive until the operating system lookup
returns, while leaving the UI responsive.
The 32-entry wire snapshot keeps the observer and nearest 27 active peers, plus
four rotating name-only roster entries. Clients cache discovered names for up
to eight seconds so the browser can assemble the full connected roster without
replicating out-of-range players.

When connected during an EDF mission, the client samples its local soldier pose
and input on the guest engine thread. Client transforms are relayed at input rate
and published to peers at 20 Hz. Other connected players appear as interpolated
`FriendSoldier01` replicas. Server presence follows fresh authenticated input,
server-owned death/reset state, and trusted host samples for native guest seats
it owns. Other F5 network peers use bounded client transforms and fresh accepted
input. A client-reported menu/loading hint neutralizes controls but does not hide
a living connected player. In client-transform mode the server cannot
independently verify mission or menu transitions; F5 uses trusted host poses and
vitals only for its mapped native EDF seats.

The client forwards both controller and native keyboard/mouse controls. Their
button and trigger states are combined, and keyboard movement or vehicle aim
overrides the matching controller axis when active, following the game's input
merge behavior.

## Build

The dedicated server can be built without the game SDK, native graphics targets,
or renderer dependencies:

```sh
cmake -S . -B out/build/server -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DEDF2027_HEADLESS_SERVER_ONLY=ON
cmake --build out/build/server --target edf2027_server edf2027_master
```

The same headless path is available on Windows and Linux with a supported CMake
toolchain. The client browser is included in `edf2027.exe` when the regular
ReXGlue game build is enabled.

### Quick two-client LAN session

Build and start the standalone server in one terminal (no game SDK or XEX is
needed by the server):

```powershell
cmake -S . -B out/build/multiplayer-server -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ `
  -DEDF2027_HEADLESS_SERVER_ONLY=ON
cmake --build out/build/multiplayer-server --target edf2027_server
.\out\build\multiplayer-server\src\multiplayer\edf2027_server.exe --name "EDF Host" --mission "Mission 1"
```

`--server-authoritative-movement` is an opt-in input-only mode. Use it only when
`--spawn-x`, `--spawn-y`, and `--spawn-z` match the selected mission's spawn;
otherwise the server's default `(0, 0, 0)` reset can move players outside the
level. The server browser labels each server's movement authority. This mode
owns replicated X/Z coordinates but still leaves EDF collision, vertical
movement, native weapon hit detection, and mission logic client-side. In this
mode `--player-health` sets server-owned max health (default 100, range 1–65535);
trusted Luau scripts can subtract health with `edf.damage_player`. No EDF weapon
or enemy hit path calls this hook yet. The F5 local-host option uses
client-transform mode until mission spawn mapping is available.

Run two matching native game clients. On the host PC, press **F5**, enter a
player name, and choose **Host local and join**. On another PC, enter a
different player name and the host's LAN IPv4 address followed by `:27717`,
choose **Join address**, then close the browser and start the same mission on
both clients. Its roster includes
connected inactive/out-of-range peers by name only; only the nearest 27 active
peers become remote soldier replicas in each snapshot.
Mission simulation and combat remain client-side.
Allow UDP 27717 through the host firewall; UDP 27718 is optional LAN discovery
for **Refresh LAN**. Direct address joining does not depend on discovery or the
self-hosted directory.

### Self-host with Docker

The repository includes a game-asset-free server image and a Compose setup that
starts both the game server and a self-hosted directory. The
`server-data/scripts` directory is included for the read-only container mount.
Add trusted Luau scripts there, set `EDF_ADVERTISE_IP` to the IPv4 address that
players can reach, then run:

```sh
export EDF_ADVERTISE_IP=203.0.113.10
docker compose up --build
```

Compose publishes UDP ports 27717, 27718, and 27719, keeps server scripts
mounted read-only, and stores the signing identity in a persistent Docker
volume. Enter `<reachable IPv4>:27719` in the F5 browser's
**Directory host:port** field, then choose **Query directory**. To enable the included custom ant combat example, copy
`examples/server_owned_ant_combat.luau` into `server-data/scripts` on the host.
On the same LAN, use **Refresh LAN** if the host's Docker
network forwards broadcast discovery;
otherwise enter the host's LAN address and `27717` directly in the F5 browser.
To make direct Internet joining reachable, forward UDP 27717 on the host router
and firewall, then join by its public address. The first connection saves the
server's signed key fingerprint on the client. Keep the Docker `server-state`
volume when updating the server image so that fingerprint remains stable.

The directory normally advertises the server's observed source address. Because
the Compose directory shares a bridge with the server, that address is the
container's private IP; set `EDF_ADVERTISE_IP` to a reachable LAN or public IPv4
before starting the stack. Forward UDP 27717 and 27719 through the host router
and firewall for Internet play. The same override can be supplied directly as
`--advertise-ip <reachable IPv4>` when running the server outside Compose.

To stop the server, run `docker compose down`. The binaries are also available
inside the image as `/usr/local/bin/edf2027_server` and
`/usr/local/bin/edf2027_master`; the default image entrypoint runs the game
server.

## Probe the ReX guest without its renderer

The separate `edf2027_headless_probe` target is an opt-in diagnostic for the
server simulation work. It launches the generated EDF guest through ReXGlue's
direct runtime API with GPU setup skipped, then reports completed calls and
engine steps from the guest's `sub_821A4BA0` dispatcher. With `--input-script`,
it feeds the game's controller wrapper and samples player and hostile faction
objects on the engine thread. Scripted mode succeeds only after it observes a
living player, live faction-1 actors, and the same actor moving across at least
eight ready engine samples. World sampling runs every eight dispatches. This is
evidence of active gameplay for the supplied route; the probe does not identify
the mission or map. Its status output lists observed faction-list objects named
`clPlayerObject` and the value read from each object's `+492` player-index field.
An observed index does not establish a working local seat or a mapping to the
same-numbered XInput user. Duplicate player indices are marked ambiguous and are
not eligible for network input. The `xinput_wrapper_calls` summary counts calls
to the guest's XInput wrapper, including paths that inject state without polling
native XInput. `xinput_native_forwards` counts calls forwarded to the native
`XamInputGetState` import; it does not record whether a controller was attached
or the import succeeded. Both summaries show each user as
`total/calls-while-last-sampled-ready`; readiness comes from the latest world
sample, so calls near a transition can be classified using the previous sample.
When mission sampling is enabled,
readiness is sampled every eight dispatcher calls; without it, the
last-sampled-ready counts stay zero. These diagnostics do not prove player
construction or input-to-player mapping.

The plain diagnostic probe accepts `--input-user INDEX` (`0` through `3`,
default `0`). For a nonzero index, it feeds the same scripted input to user 0
and the selected XInput user, so the primary frontend can start the mission while
the probe observes the selected seat. The route
`tools/native-probe-secondary-player-input.txt` holds a movement input after the
Mission 1 load. With `--input-script` and a nonzero input user, success additionally
requires at least one wrapper call for that user and a unique living
`clPlayerObject` whose `+492` index equals the selected user to move at least
1 meter from its initial position across at least eight consecutive ready world
samples. The probe matches those samples by guest object address, but does not
observe removal or reallocation between samples, so this is not a lifetime-stable
identity. Missing, ambiguous, dead, or not-ready samples reset the motion baseline.
This is an observation for that supplied route and generated guest/build only; it
does not establish behavior in all missions, prove that input caused the observed
motion, or prove production multiplayer seat ownership. The authoritative host
does not accept `--input-user`; its network input remains on user 0.

Both headless guest targets accept `--user-root DIRECTORY` to isolate the XAM
profile and save files from the extracted game files. By default they create a
`user-data` folder beside the game-data directory. A seeded save made with
`tools/make-edf-save.py` can be placed in this directory when a reproducible
mission-select route is needed.

For a bounded player-construction trace, enable the development cvar
`edf_native_load_trace` while loading a mission. The script-native dispatcher
resets its sample counters at `MissionSequence::Begin` and logs at most its
first two calls per selector, recording the selector
(`r7 & 0x7f`), caller, raw `r3` through `r6` values, and returned `r3`. Those
registers are deliberately not labeled as decoded script arguments. The generic
object-factory trace separately records up to 128 returned objects whose RTTI class
contains `Player`, `Soldier`, or `Camera`, plus up to 128 other factory results;
for candidate classes it decodes the guest wide-string path and records the
`clPlayerObject` value at `+492` when readable. A separate faction-registration
trace logs up to 128 candidate objects after registration, including that player
index, faction, `notify` flag, and health field. The trace also records at most
16 raw register samples each for generated input setup routines `sub_820A0220`
and `sub_820BDC68`; these values are not labeled as decoded arguments. Factory
and registration samples continue while the cvar remains enabled, so later
gameplay object creation may also appear after the mission-load sample counters
reset. These traces reveal construction and setup calls, but do not by themselves
prove seat registration or controller binding. Their addresses and observations
belong to this generated game build; do not assume they describe the prototype
XEX or its mission bytecode without establishing that the binaries match.

The standalone network server does not authoritatively simulate EDF mission or
weapon combat.

Build it from the regular Windows game configuration, with the ReXGlue 0.10.0
SDK installed:

```powershell
cmake -S . -B out/build/headless-probe -DEDF2027_BUILD_HEADLESS_GUEST_PROBE=ON
cmake --build out/build/headless-probe --target edf2027_headless_probe
.\out\build\headless-probe\edf2027_headless_probe.exe --game-root "D:\path\to\extracted-game" --xex "game:\\default.xex"
```

Use the actual XEX filename exposed in the game's `game:` VFS mount. The
network-only server build remains independent of ReXGlue and game assets.
To drive the benchmark's fresh-profile Mission 1 route and require live actor
progress, add `--input-script tools/native-benchmark-skipintro-input.txt
--duration 300`. The route uses simulation-clock time, so allow enough wall time
for the menu, intro skip, and map load to finish.

To check whether the same route/build exposes a moving player object with index 1,
invoke the plain diagnostic probe with:

```powershell
.\out\build\headless-probe\edf2027_headless_probe.exe `
  --game-root "D:\path\to\extracted-game" `
  --xex "game:\\default.xex" `
  --input-script tools/native-probe-secondary-player-input.txt `
  --input-user 1 --duration 300
```

Periodic and final status include the selected user's wrapper counts and the
selected-player presence, ambiguity, life, consecutive-ready-sample count, and
motion observation. For a nonzero selected user with a script, the probe clearly
reports failure if either the wrapper call or motion observation is absent.

## Run the guest-backed experimental host

The optional `edf2027_authoritative_host` target starts a headless ReX guest
alongside the ENet service. It requires extracted game data and a controller
route because there is no UI to select and start a mission. The first two active
network pilots can control guest users 0 and 1. Seat 0 requires a unique, living
`clPlayerObject` with player index 0 in the sampled guest world. At startup the
host requests a second `Player01.Sgo` object at index 1 through this generated
build's factory and registration calls. Seat 1 stays unavailable until the guest
sampler sees a unique, living index-1 player object. This is a build-specific
construction path, not a general EDF seat factory. Pilot input is buffered until
the probe sees that player and at least one live hostile actor; that readiness
heuristic does not identify the mission. Accepted input is copied through a
thread-safe per-seat mailbox; only the guest thread writes guest controller
state. Each seat has its own input, sampled transform, reset, and acknowledgement.
A gameplay-inactive pilot stays assigned with neutral controls through a menu,
loading screen, or respawn so server position resets can still reach that soldier.
A brief input timeout neutralizes controls but preserves the seat for recovery;
a disconnect releases only that pilot's seat. This host always
advertises server-authoritative movement and ignores client position packets.
The first two seated pilots use transforms sampled from their live EDF soldiers;
additional network players use the server's fixed-step planar fallback until
more guest seats are validated.

Build it from the regular Windows game configuration:

```powershell
cmake -S . -B out/build/edf-host -DEDF2027_BUILD_HEADLESS_GUEST_PROBE=ON
cmake --build out/build/edf-host --target edf2027_authoritative_host
```

For the headless mission route, use `tools/native-headless-mission-input.txt`.
It follows the established benchmark route: menu input begins at 20 seconds of
guest game time, the intro skip is at 85 seconds, and gameplay input begins at
100 seconds. Use `--duration 420` if you want the process to stop after the full
300-second gameplay input; without `--duration`, the host keeps running until
Ctrl+C:

```powershell
.\out\build\edf-host\edf2027_authoritative_host.exe `
  --game-root "D:\path\to\extracted-game" `
  --user-root ".\server-user-data" `
  --input-script tools/native-headless-mission-input.txt `
  --name "My EDF Host" --mission "Mission 1"
```

For a route diagnostic, add `--trace-input-consumers` to report which guest
XInput call sites received scripted Start/A presses. Add
`--trace-mission-actors` to emit bounded native enemy spawn/removal lifecycle
records from the guest. These flags only observe the route; lifecycle records
appear only if the guest reaches Mission 1.

This is an integration bridge, not yet a production authoritative mission
server. Guest-backed player positions and headings come from fresh samples of
their live guest soldiers; samples older than 500 ms suspend that pilot until
the guest is available again. The index-1 constructor uses generated addresses
from this game build. Unit tests cover seat validation, input isolation, reset
acknowledgements, and disconnect reuse. The current headless Mission 1 route
has not yet been demonstrated to enter gameplay, so creation and controller binding of the second
soldier have not been demonstrated at runtime. Treat seat 1 as experimental
until that route, or an equivalent rendered host path, reaches a live mission.
Additional network players use the fixed-step server fallback until guest seats
are validated for them.

Script and server respawn position resets are applied on the guest
thread and held pending until both the client and guest confirm the same reset
generation. The guest confirmation comes from a later simulation readback of the
soldier and camera pose. Guest controls stay neutral while either confirmation is
pending. If a different pilot takes the guest seat, the previous player's reset
falls back to the standalone client acknowledgment path. Mission objectives,
enemy AI, weapon behavior and combat effects are not replicated from the host
guest. With host mission-ant reconciliation enabled, clients apply server pose
and health only to ordinary ants matched by exact mission spawn keys; those
matched ants are locally invulnerable. The bundled Luau policy can select one
of those host-sampled ants from accepted client aim and queue custom server
damage. Ants without an exact match still simulate locally, and this policy
does not reproduce EDF weapon behavior. The
standalone `edf2027_server` remains the self-host option when the
ReX guest and game data are not available.

## Host a game server

Run the server on the host and point it at a data folder for its server-side scripts:

```sh
./edf2027_server --name "My EDF Server" --mission "Mission 1" \
  --data-dir ./server-data --port 27717 --discovery-port 27718 \
  --max-players 128 --interest-radius 500 \
  --spawn-x 0 --spawn-y 0 --spawn-z 0
```

On Windows, run `edf2027_server.exe` with the same options in PowerShell. Place
trusted Luau scripts in `server-data/scripts`; copy
`examples/server_owned_ant_combat.luau` there to enable the optional example.
When neither `--data-dir` nor `--scripts` is supplied, the server looks for
`server-data/scripts` beside its executable. In a source checkout, it falls back
to `server-data/scripts` in the current directory when that folder exists.

The default server listens on all IPv4 interfaces. Its default game port is
27717 and its default discovery port is 27718. Allow or forward those UDP ports
to make it reachable from outside the local network; if you change either port,
allow or forward its configured value instead.
`--interest-radius` is in meters; each client receives the nearest active players
within that radius, capped at 32 records per snapshot. Press **F5** in the game
to refresh the LAN list. To join over the Internet without a
directory, enter `host-or-ip:27717` in the address field, replacing `27717` with
the configured game port if it has changed. The **Refresh LAN** scan uses the
client's configured discovery port, which must match the self-hosted server's
`--discovery-port` value. Set the browser's `edf_discovery_port` to that same
value; it is persisted in the per-user configuration. This setting does not
affect **Query directory**, which continues to use the configured directory
host and port.
`--spawn-x`, `--spawn-y`, and `--spawn-z` set the host-controlled initial player
spawn in meters; they default to the world origin. Pick coordinates for the
mission you are hosting. Additional joins are spread in a small grid around that
point. The server does not yet inspect the selected mission to discover its
native spawn automatically.

## Host a server directory

The directory is also self-hosted. Run `edf2027_master.exe` on a public host,
then start each game server with its address:

```powershell
edf2027_master.exe --port 27719
edf2027_server.exe --name "My EDF Server" --master "directory.example:27719"
```

Open or forward the directory's UDP port. In the F5 browser, enter the same
`host:port` in **Directory host:port** and choose **Query directory**. Entries
update every 20 seconds and disappear when the game server disconnects or its
heartbeat has been absent for 90 seconds. The directory keeps its list in
memory.

## Chat

Connected players can send chat from the native F5 browser. The server assigns
the sender identity and relays the message to authenticated clients; clients
cannot choose another player's ID. Chat is UTF-8, limited to 255 bytes, and
rate-limited per player. The server also caps aggregate broadcasts at 20 per
second because each message is reliably sent to every authenticated player;
messages over that limit are dropped.

## Luau server hooks

Player names are limited to 16 UTF-8 bytes; chat and broadcast messages are
limited to 255 bytes. Place trusted `.luau` files under `server_data/scripts`.
For a dedicated installation, pass `--data-dir /path/to/server-data`; the server
loads scripts from its `scripts/` subfolder. `--scripts` selects a specific
script folder and takes precedence over `--data-dir`. Files load once at startup.
Available hooks are `tick`,
`player_join`, and `player_leave`. `edf.broadcast(text)` sends a server message
to connected clients and the host log, sharing the server-wide 20-per-second
chat limit. The server fires `player_leave` once for each authenticated player
when the connection closes or the server stops. Restarting a stopped server
starts a fresh Luau VM and reloads the scripts. `edf.rebase_player(id, x_cm, y_cm, z_cm,
yaw_mrad)` sets a server-approved position in centimeters (yaw is in
milliradians) and clears the player's active
state and input. The server marks that player's owner snapshot with a pending
position reset and generation ID. The game client applies the position to its
local soldier, then acknowledges that ID with its next input packet. The server
keeps a standalone-server reset pending until it receives the matching
acknowledgment; that acknowledgment is accepted even if the client reports
gameplay inactive. The experimental guest-backed host
also waits for a later guest simulation readback confirming the host soldier kept
the reset; controls stay neutral until both acknowledgments arrive. If a different
pilot takes the single guest seat, the previous pilot no longer waits for a guest
acknowledgment. A client cannot choose coordinates for a server-approved reset;
ordinary movement transforms are client-authoritative by default in protocol v20;
servers started with `--server-authoritative-movement` ignore them. View
direction is still sent as aim input. `edf.players()` returns a fresh array of
connected players in join order. Each record contains `id`, `name`, `active`,
`health_valid`, `alive`, `health`, `max_health`,
`x_cm`, `y_cm`, `z_cm`,
`yaw_mrad`, `buttons`, `left_trigger`, and `right_trigger`. IDs come from server
state; ordinary positions and controls originate from accepted client input,
while `active` is server-owned. A fresh authenticated input keeps a standalone
player present even when `gameplay_active` is false; that client hint only
neutralizes controls. Pending server position resets and server-owned death
keep a player inactive. F5 hosting derives activity from trusted transforms and
vitals for mapped native guest seats; other network peers use fresh accepted
input and bounded client poses. An unavailable mapped guest seat stays inactive. Client
transforms also pass the configured speed outlier guard, but are not
collision-validated. These fields do not prove a legitimate mission transition
or client action. Health is populated from a trusted native EDF host sample or
the dedicated server's configured health model. Headless
standalone servers leave `health_valid` false in client-transform mode. With
`--server-authoritative-movement`, the standalone server initializes health to
`--player-health` at authenticated join and trusted Luau scripts can reduce it
through `edf.damage_player`; other dedicated-server modes leave `health_valid`
false.
The guest-backed host keeps its owned seat binding when EDF changes the native
player index to `-1` on death. It publishes the latched dead state for at least
five seconds, including across a gap where both native player indices are
temporarily absent. It adopts a different index-1 object after observing that
seat absent following the death. If EDF creates the replacement in the same
frame as deassignment, adoption waits until the full presentation window has
elapsed. This assumes EDF's native index-1 slot is unique. If no replacement
appears, it creates a seat after the primary returns. The dead state stays
latched while the primary seat is unavailable, and resets at the next mission
sequence or owner disconnect. The bridge retains its seat authorization across
mission loads and rebinds the new native object. Removed guest pointers are
discarded immediately, so a recycled heap address cannot be removed as the old
seat. Native revive controls and the client's death animation remain
unverified; this is a host-vitals bridge, not a complete EDF life-cycle port.
The script view also includes `input_sequence`, signed `move_x`/`move_y` and
`aim_x`/`aim_y`, `aim_yaw_mrad`, `gameplay_active`, and `request_spawn`. Control
axes are zeroed when the client reports gameplay inactive or the server marks
the player inactive. These fields expose the latest accepted input to `tick`
hooks; they are server-local and add no fields to the network protocol.
`gameplay_active` is a client-reported menu/gameplay hint, not a presence or
damage exemption. In headless authoritative mode, the server initializes health
when the authenticated player joins, even if the client reports itself
inactive; damage policies should use connected players' server-owned health
and life state rather than the client hint or `active` alone.
`edf.damage_player(player_id, damage_points)` subtracts whole health points and
returns whether the server accepted the hit. It is enabled only for a headless
server started with `--server-authoritative-movement`; each player can receive
at most one scripted damage request per 60 Hz server tick. Lethal damage marks
the player dead and inactive. An approved respawn restores the configured max
health. This hook does not detect EDF weapons, projectiles, collisions, or enemy
attacks; a trusted server script must decide when damage applies. In this mode,
respawn requests are honored only when the server has valid health state showing
lethal damage (zero health), followed by the existing inactive interval and
cooldown. Requests are rejected when health is unknown. The client can select
only the configured spawn and cannot use a request to heal or teleport while
alive. For example, `examples/server_player_hazard.luau` applies a small
periodic hazard to every connected living player, starting after ten seconds,
regardless of the client's gameplay-activity hint.
The list is capped by the server's 512-player limit. It does not expose enemy
state or mission objectives.
`edf.actors()` returns a fresh detached array; edits to the returned Luau tables
do not affect server state. On the guest-backed host it combines the latest
sampled server-owned actors and ordinary live mission ants. In callback-free
headless mode it contains only the dedicated server's abstract server-owned
actors. Each record contains `id`,
`archetype` (currently `"ant"`), `x_cm`, `y_cm`, `z_cm`, `yaw_mrad`, and
`health`, plus `server_owned`: `true` for explicitly server-owned actors from
either the guest-backed server actor source or the callback-free headless
registry, and `false` for host-sampled ordinary mission ants. This field is
Luau-view provenance, not a new snapshot field. Each source is independently
capped at 48 records, so the combined Luau view is capped at 96. Mission-ant IDs
use a distinct high-bit namespace and are not reused during the host process.
Ordinary mission ants are deliberately excluded from `ActorSnapshot`; clients
continue simulating their own local mission ants independently. Server-owned
actors use that existing snapshot path. Clients smooth replica poses and
reconcile health from actor snapshots. A snapshot reporting zero health removes
the ant from the client scene and keeps it out while that ID remains in snapshots;
when the host sampler omits the dead ant, stale-ID cleanup removes it. Headless
abstract actors remain in the bounded registry at zero health so their death
remains observable in snapshots. Since trusted Luau can base decisions on the
host-sampled actor health and pose, it does not need to accept client damage
claims as evidence. Accepted client input is exposed as intent, not as a
verified hit; only trusted Luau chooses targets and damage for custom host-side
combat. Sampling and damaging a host mission ant alone does not bind its identity
to a client's local ant or replicate its health/death. The opt-in exact-spawn-key
reconciliation below adds pose and health synchronization for unique matches.
When the game client detects a load or death-to-live transition, it can request
a respawn at the host-configured spawn. The server requires at least two seconds
of inactive gameplay state and enforces a five-second cooldown. It never accepts
a replacement coordinate from the client.
Call it only after trusted server logic confirms a spawn or mission transition;
do not use unverified client coordinates as the new position. A wrong spawn
coordinate may put the client at an invalid place in the local mission. It returns
`true` when the player exists and the coordinates are valid, or `false` when the
player ID is not connected. Invalid coordinate arguments raise a Luau error.
The standalone dedicated server cannot inspect guest mission state. On the
guest-backed host, Luau can inspect only the bounded live ant sample described
below; it still cannot verify pilot death, mission transitions, or objectives.
The client reports transitions through unverified gameplay state and spawn
requests. A spawn request is accepted only when trusted health state records the
player dead at zero health; requests are rejected while health is alive or
unknown. This still does not prove a legitimate mission transition. Hosts must
supply verified coordinates and decide when a Luau rebase is appropriate; the
server does not load or verify mission state itself.

The guest-backed host exposes `edf.spawn_actor("ant", x_cm, y_cm, z_cm,
yaw_mrad)`. It queues a black Giant Ant on the authoritative guest and returns a
monotonic entity ID when queued, or `nil` if the host mission is not ready or it
has no free guest actor slot; the bridge caps queued, in-flight, and live ants at
48 and releases a slot when an ant is removed or observed dead. A readiness or
mission-generation transition may drop an accepted queued request before it
becomes an actor, and that canceled request releases its slot. The example
reports queue acceptance and does not retry an accepted ID. Scripts that need
confirmation should look for that ID in `edf.actors()`. If the network runtime restarts while its guest
remains alive, live guest ants continue occupying slots until the guest removes
them. Clients
within the server's interest radius create an ant replica and smooth its pose from
20 Hz snapshots. The client replica is assigned to invader faction 1, but is not
awakened; the server owns its movement. The replica is marked invulnerable
locally, and its health, dead flag, and pose are reasserted from server snapshots
so client damage cannot become authoritative. Native hit reactions and targeting
still need in-game verification. This guest-backed behavior does not replace
mission-script spawns.
When the guest actor-source, spawn-sink, damage-sink, and mission-actor-source
callbacks are all absent, the API creates an abstract static server-owned ant
with initial health 60 to match the disc's GiantAnt01 durability. The callback-free headless registry accepts at most 48
spawn requests per server instance; failed or removed actors do not restore that
counter. These records are published through the existing `ActorSnapshot`
path without changing its wire encoding. The headless server supplies no EDF AI,
weapon, or physics simulation and does not move the actors. Trusted scripts can
add their own owned-actor policy as described below.

In callback-free headless mode, trusted scripts can move a live server-owned
ant with `edf.set_actor_transform(actor_id, x_cm, y_cm, z_cm, yaw_mrad)`.
Coordinates are centimeters and yaw is milliradians. Each coordinate must be
within ±1,000,000 cm and yaw within ±3,142 mrad; invalid arguments raise a
Luau error. The call returns `true` after updating a live abstract ant and
`false` for an unknown, dead, or non-headless actor. The updated transform is
included in the next actor snapshot, so clients render a replica of the
server-controlled pose. This is a bounded script hook, not an EDF movement or
collision simulation. The guest-backed host returns `false` for native guest
actors and leaves those actors under the game's AI. The example
`server_owned_ant_combat.luau` demonstrates simple script-driven pursuit and
proximity attacks for headless ants. It uses accepted player positions as
targets, which remain client-authoritative inputs.

Trusted server Luau scripts on the guest-backed host can call
`edf.damage_actor(actor_id, damage_points)` for a currently live server-owned or
host-sampled mission ant. The server validates the ID against its latest
source-specific sample, limits each actor to one accepted request per server
tick, and queues only the ID and damage value for application on the guest
thread. It applies at most 16 requests per guest sample, leaving overflow in
the bounded queue. The call
returns `true` when queued, not when the game has applied it; it returns `false`
when the actor is not currently tracked, the guest is unavailable, the per-tick
limit is reached, or the 64-request queue is full. `damage_points` must be in
`(0, 65535]`. The source queues this trusted value for the current generated
guest damage call; a successful queue does not establish how much health the
game removes. This guest-backed server-script API does not implement automatic client weapon
hits. For mission ants, the guest thread revalidates class, faction, live health,
dead flag, vtable, and the current mission registry before applying damage. The
generated scenegraph-removal hook invalidates pointer identity before calling the
original removal routine; absent or dead samples are also removed, and mission
IDs are not reused during the host process. Explicit-spawn reservations remain
separate and are unaffected by mission-ant cleanup. The headless guest-backed
host requires `--mission-actor-reconciliation`. The rendered F5 local host
enables mission sync automatically, and every rendered game tracks local spawn
keys from mission start. Exact matches receive host pose and health; unmatched
keys stay local. Joining clients make matched local ants invulnerable, then
restore their previous invulnerability flags when the session ends. In
callback-free headless mode, `edf.damage_actor` instead subtracts positive damage
from a live server-owned abstract ant on the server, clamps remaining health at
zero, and rounds it to the nearest whole health point before storing it as
integer health; subsequent actor snapshots publish the resulting health and
death. The existing one accepted request per actor per server tick limit and
positive damage range `(0, 65535]` apply in both modes. Headless health
bookkeeping supplies no EDF weapon or physics simulation. A guest-backed request
that fails later actor validation or whose guest call throws is dropped without
retry. The guest hook
revalidates the selected server-owned or mission actor before applying the
trusted script's value on the guest simulation thread. This describes the
current source call path only;
it does not establish EDF weapon semantics or mission combat behavior, and no
prototype XEX ABI or address is used to infer generated-build behavior.

Separate prototype-artifact observation (not part of the generated-build API
contract): local Ghidra/XEXLoaderWV inspection of the supplied prototype XEX
observed function `0x820D59D0` reading the prototype damage record at `+0x14`
(faction) and `+0x18` (float amount). This artifact-specific observation does not
verify the generated-build address, ABI, call semantics, or full record size;
the guest-side 128-byte allocation is not proof of the record size.

### Optional custom combat policy for server-owned and mission ants

`edf.players()` contains server-retained accepted input, including the accepted
`right_trigger` value and client-supplied aim yaw, alongside server-owned player
position and the current gameplay-active hint. A false hint zeros the controls
before scripts see them. The server bounds accepted yaw changes to 720 degrees
per second by default, using server packet receive time and the shortest wrapped
angle across +/-180 degrees, crediting at most 100 ms of turn time per packet.
Self-hosted servers can set `--max-client-aim-rate`
from 90 to 3600 degrees per second. The first active aim establishes the starting
heading; inactive hints do not change it or accumulate extra turn allowance.
This limits abrupt aim snaps for server scripts but does not verify a player's
camera, pitch, or physical input. `edf.actors()` contains the latest host-sampled server-owned
ant records and ordinary mission ants on a guest-backed host, or abstract static
server-owned ants in callback-free headless mode. A trusted script can treat a
control as an action request, choose a target from those actor records, and choose
its own damage value before calling `edf.damage_actor`. The client supplies
neither target ID nor damage amount. This is a mod-defined policy, not a claim
that right trigger is a particular EDF weapon's fire control or that range,
target selection, or damage matches EDF.

`examples/server_owned_ant_combat.luau` is an opt-in example that targets live
server-owned ants and host-sampled mission ants. On a headless guest-backed
host, start with `--mission-actor-reconciliation`. The rendered local host and
clients track exact spawn keys from mission start. Connecting later can sync
already-live ants when the local spawn keys match; every client still needs to
run the same mission.
The example tries to spawn
one demo ant only when no live ant is available, and retries only after a nil
result, no more than once per 120 script ticks. Once a spawn is accepted, it
does not respawn the ant after death or removal. On a rising right-trigger input
it selects the nearest live ant within 1,800 cm that falls inside a horizontal
cone with a 20-degree half-angle around the accepted client aim yaw and within
300 cm vertically, then requests 25 damage points. A player can queue a new
action every 12 script ticks (about 0.2 seconds at 60 Hz), with at most one
pending action per player. Firing again requires a fresh trigger edge after the
cooldown; holding the trigger does not auto-repeat. New actions enter a FIFO
queue; a rejected retry moves to the tail, so admission is FIFO with retry
rotation. Another trigger edge is ignored while that player has a pending action
or is on cooldown. Pending actions are discarded if the player becomes
gameplay-inactive, disconnects, the target is no longer live, or the action ages
past its 120-tick TTL. Eight is an attempt budget per server tick,
not a drop limit. A request for which `edf.damage_actor` returns `false` remains
pending and is retried for up to 120 locally counted tick callbacks (about 2
seconds at 60 Hz), then is removed. It is also removed when the API accepts it
into the guest queue (or applies headless damage), the target is no longer live,
or the player disconnects. Queue acceptance does not mean guest damage has
completed. This horizontal aim-cone and vertical-bound rule is example custom
server policy, not reverse-engineered weapon data or EDF weapon simulation. It
has no pitch, line-of-sight, projectile, ammunition, animation, mission AI, or
collision checks. Install it as a trusted script under the server's scripts
directory. On a guest-backed host it applies custom server damage to the
selected server-owned or mission ant; this is not EDF weapon simulation. In
callback-free headless mode it can update abstract health, but that mode
supplies no EDF AI, weapon, or physics simulation.

In callback-free headless mode, live server-owned ants also pursue the nearest
player whose health is valid and above zero. They move at 4 cm per server tick,
stop 120 cm away, and request 12 damage when within 180 cm horizontally and
220 cm vertically. A successful attack starts a per-player 45-tick cooldown
(about 0.75 seconds at 60 Hz), so multiple ants do not stack damage beyond that
interval. This uses the server's accepted player transforms, which still come
from clients in client-transform mode, and has no EDF collision, pathfinding,
animation, mission-state, or native enemy-AI behavior. It requires
`--server-authoritative-movement`; otherwise the server has no trusted player
health to damage. The guest-backed host leaves its native EDF enemy AI in
control and does not apply this abstract ant attack.

```lua
edf.on("player_join", function(player_id, player_name)
    edf.broadcast(player_name .. " joined")
end)

edf.on("tick", function(tick)
    if tick % 3600 == 0 then
        local active = 0
        for _, player in ipairs(edf.players()) do
            if player.active then active += 1 end
        end
        edf.broadcast("One minute has passed; active players: " .. active)
    end
end)

-- Callback-free headless mode creates a static abstract ant; a guest-backed host
-- queues a guest ant. Coordinates are centimeters and yaw is milliradians.
local spawn_complete = false
local next_spawn_attempt = 0
edf.on("tick", function(tick)
    local has_server_ant = false
    for _, actor in ipairs(edf.actors()) do
        if actor.server_owned and actor.archetype == "ant" then
            has_server_ant = true
            break
        end
    end
    if has_server_ant then
        spawn_complete = true
    elseif not spawn_complete and tick >= next_spawn_attempt then
        local ant_id = edf.spawn_actor("ant", 1200, 0, 0, 0)
        if ant_id then
            spawn_complete = true
            edf.broadcast("Server-owned ant spawn accepted as " .. ant_id)
        else
            next_spawn_attempt = tick + 120
        end
    end
end)
```

The VM removes filesystem, process, debug and dynamic-loading APIs, caps total
Luau memory at 16 MiB, limits each event to 256 callbacks, and interrupts the
event dispatch after 200,000 instructions total. A callback that errors is
disabled for the rest of the server session. Only the server executes these
scripts; scripts never run in the client.

## Current limits and network safety

The transport and secure-session design decision is documented in
[`multiplayer-networking.md`](multiplayer-networking.md).

The current game client build is client-authoritative for player coordinates:
clients can report their own bounded transforms, so movement is not cheat-resistant
or server-authoritative. Inputs that omit a transform use the simple planar
420 cm/s fallback. The relay does not reproduce or validate EDF movement,
acceleration, terrain collision, jumping, or vehicles. The server also does not
simulate mission enemy AI, weapon behavior, client damage, mission objectives, or
shared random state. The guest-backed host can run AI for explicitly spawned
ants, and trusted scripts can implement a custom server-authoritative policy
that maps accepted client input to damage for tracked server-owned or host-sampled
mission ants. This is not EDF weapon simulation: no client hit/target/damage
claims are accepted, and shared mission combat is not yet achieved. The host may
damage its sampled mission ants, but each client still runs mission enemies and
combat independently and locally. A guest-backed host now sends trusted player
health and life state with protocol v20 snapshots; the owner applies those values
to its local soldier while host-authoritative movement is active. This is limited
to native player seats sampled by a running EDF host. Remote player replicas still
show smoothed positions and server facing; movement animation, weapon state, and
combat effects are not synchronized. Clients therefore do not yet share one
authoritative mission simulation. Actor snapshots are separate from player
snapshots, capped at 48 actors, and stay within the 1,200-byte packet limit.

To trace the host's sample stream as it reaches clients, start the guest-backed
host with `--mission-actor-diagnostics` and set `edf_mp_actor_trace=true` on
the client. The host sends a separate, read-only diagnostic snapshot at 20 Hz,
filtered to the client's interest radius. The
client logs the received host IDs, health, and pose once per second as
`side=client_remote`. Those records are kept out of the gameplay `ActorSnapshot`
path, so they do not create or remove guest objects. Both builds must use
matching protocol v20 and wire v11. The host also sends a separate bounded
identity diagnostic containing its actor IDs and mission spawn call/ant
ordinals. The client checks those ordinals against its current live local trace
on the game thread and logs `spawn_ordinal_candidate` matches. Host and client
trace generations are process-local counters; matching ordinals do not prove
that both processes loaded the same mission. Empty identity samples clear old
candidates. This stream remains read-only: it does not independently validate
the radius calculation, create or remove guest objects, or make AI/combat
server-authoritative.

## Experimental mission-ant state reconciliation

The host can send an atomic, bounded mission-ant state sample at 20 Hz. The
rendered F5 local host enables sampling when using "Host local and join";
headless guest-backed hosts use `--mission-actor-reconciliation`. Every game
keeps a bounded, silent map of mission-ant spawn identities from mission start.
This lets a client that connects later compare already-live ants when it has
run the same mission. Applying host pose, health, or removals is an explicit
experimental opt-in through `edf_mp_mission_actor_reconcile=true`; it is off by
default, including while connected. Host and client trace generations are
process-local, and matching spawn ordinals alone cannot prove the games loaded
the same mission or identified the same ant. Enable it only for a controlled
same-mission test. Diagnostic logging remains separately opt-in.
The host snapshot carries actor ID, exact `(call_ordinal, ant_ordinal)` spawn
key, pose and health together, so the client never joins state from two
different sample ticks. It caps at 48 actors and fits in 1,171 bytes. When more
than 48 ants are live, the host rotates the sample so later spawn ordinals also
receive updates; an omitted live ant is not declared removed before the sample
has had time to cycle through the bounded actor set. The host
retains up to 4,096 removals per mission generation and sends reliable
pages of at most 255 records. Clients merge pages in order, so late joiners
receive the full bounded removal history across multiple snapshots. ENet
fragments each page within the bounded 4,096-byte application-packet limit.

On the game thread, a client applies pose and health only when exactly one
current faction-1 `clGiantAnt` has the same spawn key. It rechecks the object
heap, class and faction before writing, and never creates a second proxy for a
mission ant. Matched mission ants become locally invulnerable, and a local-only death is cleared while the
guest still retains the object, so the host's health remains authoritative
between snapshots. Unmatched or ambiguous ants are left alone. The host sample
corrects local AI movement drift at the next update, but client AI continues
between samples. The host also sends a bounded cumulative entity
removal snapshot reliably when its per-mission generation/revision changes and
when a client joins. The game thread applies a host removal only to one unique
local ant with the exact spawn key, using the game's scenegraph removal routine.
This is an entity removal, not a kill confirmation: the hook is a generic
scenegraph removal path, and available source evidence does not prove that every
enemy death reaches it. The client still runs AI and mission objectives locally;
only matched-ant pose/health and the scripted custom damage policy are shared. A client
that removed its local ant before receiving the host event cannot restore it
from a state sample. This is experimental pose/health/removal reconciliation,
not a shared authoritative mission or a claim that multiplayer is complete.

With tracing enabled, the host also emits bounded mission-actor lifecycle
events. `identity_assigned` and `removal_hook_invalidated` record changes to
the host's ID registry; `vtable_mismatch` records a changed vtable at a tracked
address. `sample_disappearance` means an actor was absent from the current live
sample, not that its death or destruction was confirmed. Damage events share a
`request_id`: `damage_queued` records acceptance into the guest queue, and
`damage_dropped` records a rejected or canceled request. A `damage_dispatch`
records that the guest damage routine returned; its raw return value does not
prove that health changed. These events are cvar-gated and capped at 512 actor
trace log records per trace generation.

With `edf_mp_actor_trace=true`, the client also emits read-only shadow-match
telemetry for each new diagnostic sample. It compares host ant poses to live
local faction-1 ants, excludes replicas already created by the port, and reports
nearest candidates within 5 m. A nearest-neighbor gap of 50 cm or less, or
multiple host records selecting the same local object, is reported as ambiguous.
The summary includes unmatched samples, mean candidate distance, and mappings
that remain stable for at least ten diagnostic samples. These are proximity
candidates, not identity proof; this path does not write guest memory or change
mission behavior and must not be used by itself to enable host takeover.

For the next identity investigation, set `edf_mp_actor_trace=true` in both a
guest-backed host and its client before loading the mission. The generated
mission spawn native tracks up to 4,096 faction-1 `clGiantAnt` candidates per
mission. This hook returns one object per factory call, so each accepted ant
uses a unique ant-spawn call ordinal and ant ordinal zero. It logs candidates
with finite health and position, including the call ordinal, ant ordinal,
class/vtable, and post-call pose. Generated
source shows that `r7 != 0` is the optional `Awaken()` gate; the trace reports
that gate and the object's post-call `+737` awaken latch. The latch is an
initialization guard, not proof of AI state. A `yaw_valid` field marks whether
the object's forward vector could define a heading. When a traced ant reaches
the hooked `sub_821C0ED8` removal path, the trace also logs its spawn ordinals,
removal order, faction, health, and position immediately before removal. Spawn
records are buffered until the enclosing mission dispatch returns; removal
records are emitted from the removal hook.
With tracing enabled on both sides, a host actor ID is correlated only when the
host registry assigns it to an ant with matching spawn-trace metadata. The
`identity_assigned` lifecycle record supplies the exact diagnostic spawn key
`(trace_generation, call_ordinal, ant_ordinal)`. Assignments without that
metadata are logged as `identity_assigned_untraced` and are not used as keys.
The client logs received IDs when the observed actor set changes or once per
second. The comparer joins received IDs through the host assignment record and
requires the exact key to occur in both local spawn traces; it does not use
position or nearest-neighbor matching:

```powershell
python tools/compare-mission-ant-traces.py --require-lifecycle --require-network-mapping host.log client.log
```

The report ignores process-local pointers, vtables, callers, and raw register
arguments. The default mode compares full `(trace_generation, call_ordinal,
ant_ordinal)` keys. `--ignore-generation` is only for single-generation captures:
each log must contain exactly one trace-generation value across spawn, removal,
host identity-assignment, and host identity-removal records. It compares
ordinal-only candidates and skips all network-ID joins, so it cannot validate
network ID mapping. It cannot be combined with `--require-network-mapping`.
This is bounded, read-only telemetry. It compares the order and sampled state of
observed removal calls, but it cannot prove that every actor removal uses this
path. A clean comparison is only evidence for a possible cross-process mapping,
not proof that IDs or death/lifecycle ordering are stable; mission enemies remain
independently simulated until repeated traces validate that mapping and server
snapshots can safely reconcile existing client actors.

The server browser compares a 12-character tag derived from the port's
source/build-input fingerprint and blocks obvious build mismatches. The full
SHA-256 fingerprint covers the port and vendored ENet/Luau source, codegen
manifests, CMake source-selection files, and generated game code. In a full game
build it is computed after guest codegen completes, before multiplayer targets
compile. A server-only build computes it from the checked-in generated game
source. The handshake checks this fingerprint before issuing a challenge. It
does not attest the compiler, runtime libraries, installed game assets, or
mission; players still need to start the same mission themselves.

The server bounds and sequences client-reported coordinates and applies a
receive-time speed outlier check after the first active client pose, but it does
not validate physics. The first pose is accepted as a starting point because the
server cannot yet map EDF mission spawns; client-authoritative movement remains
cheatable. The server relies on a client lifecycle signal to request the
configured spawn; it cannot validate mission transitions or discover
mission-specific spawn points.
Gameplay packets use X25519-derived directional keys and XChaCha20-Poly1305
authenticated encryption. A persistent Ed25519 server identity signs each
ephemeral handshake; clients pin the first verified key for each endpoint in
their per-user configuration directory. The F5 connection status shows the
server fingerprint. A changed key is rejected with the saved and presented
fingerprints; remove that endpoint's pin only when you expect the server key
to rotate. Protect and back up `identity.edfkey` with the server's data.

The directory protocol is still unauthenticated and server listings are hints;
the direct gameplay handshake authenticates the server. Player accounts and
anti-cheat are not implemented, and normal EDF mission simulation remains local
to each client. Host the directory you use and restrict UDP exposure to the
players you intend to admit.
