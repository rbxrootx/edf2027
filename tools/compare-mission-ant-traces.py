#!/usr/bin/env python3
"""Compare bounded host/client mission-ant identity traces.

  python tools/compare-mission-ant-traces.py host.log client.log

The exact diagnostic spawn key is (trace_generation, call_ordinal, ant_ordinal).
It names the observed mission-spawn event, not a proven cross-process object
identity. Process-local pointers, vtables, callers, and raw register arguments
are deliberately ignored. Removal events are compared in order. Host actor IDs
are joined through their identity_assigned lifecycle event to this key, then
checked against the host and client spawn traces. This is still candidate
cross-process correlation, not proof of actor identity or safe client AI
suppression.
"""
import argparse
import math
import re
import sys
from collections import Counter
from dataclasses import dataclass

NUMBER = r"(?:[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?|(?i:[+-]?(?:nan|inf)))"
TRACE = re.compile(
    r"Mission ant identity trace: side=(?P<side>\S+) "
    r"(?:event=\S+ )?(?:trace_generation|generation)=(?P<generation>\d+) "
    r"(?:mission_generation=\d+ actor_id=\d+ )?call_ordinal=(?P<call>\d+) "
    r"ant_ordinal=(?P<ant>\d+) caller=\S+ args=\[[^]]*\] "
    r"f1_bits=\S+ r7_awaken_gate=(?P<gate>true|false) "
    r"object=\S+ (?:vtable=\S+|expected_vtable=\S+ current_vtable=\S+) "
    r"class=(?P<class>\S+) "
    r"faction=(?P<faction>\d+) awaken_latch_after=(?P<latch>\d+) "
    rf"health=(?P<health>{NUMBER}) x=(?P<x>{NUMBER}) "
    rf"y=(?P<y>{NUMBER}) z=(?P<z>{NUMBER}) "
    r"yaw_valid=(?P<yaw_valid>true|false) "
    rf"yaw_rad=(?P<yaw>{NUMBER})"
)
LIFECYCLE = re.compile(
    r"Mission ant lifecycle trace: side=(?P<side>\S+) "
    r"(?:event=\S+ )?(?:trace_generation|generation)=(?P<generation>\d+) "
    r"(?:mission_generation=\d+ actor_id=\d+ )?removal_ordinal=(?P<removal>\d+) "
    r"call_ordinal=(?P<call>\d+) ant_ordinal=(?P<ant>\d+) caller=\S+ "
    r"object=\S+ (?:vtable=\S+|expected_vtable=\S+ current_vtable=\S+) "
    r"class=(?P<class>\S+) "
    r"faction=(?P<faction>\d+) "
    rf"health=(?P<health>{NUMBER}) x=(?P<x>{NUMBER}) "
    rf"y=(?P<y>{NUMBER}) z=(?P<z>{NUMBER})"
)
ACTOR_ID_MAP = re.compile(
    r"Mission actor ID map: side=(?P<side>\S+) id=(?P<id>\d+) "
    r"generation=(?P<generation>\d+) call_ordinal=(?P<call>\d+) "
    r"ant_ordinal=(?P<ant>\d+)"
)
ACTOR_ID_LIFECYCLE = re.compile(
    r"Mission actor ID lifecycle: side=(?P<side>\S+) event=remove "
    r"id=(?P<id>\d+) generation=(?P<generation>\d+) "
    r"removal_ordinal=(?P<removal>\d+) call_ordinal=(?P<call>\d+) "
    r"ant_ordinal=(?P<ant>\d+)"
)
MISSION_ACTOR_LIFECYCLE = re.compile(
    r"Mission actor lifecycle: side=(?P<side>\S+) event=(?P<event>\S+) "
    r"trace_generation=(?P<generation>\d+) mission_generation=\d+ "
    r"actor_id=(?P<id>\d+).*?call_ordinal=(?P<call>\d+) "
    r"ant_ordinal=(?P<ant>\d+)"
)
TRACE_GENERATION_PATTERNS = (
    re.compile(r"Mission ant identity trace: side=\S+ (?:event=\S+ )?(?:trace_generation|generation)=(\d+)"),
    re.compile(r"Mission ant lifecycle trace: side=\S+ (?:event=\S+ )?(?:trace_generation|generation)=(\d+)"),
    re.compile(r"Mission actor lifecycle: side=\S+ event=\S+ trace_generation=(\d+)"),
    re.compile(r"Mission actor ID map: .*?generation=(\d+)"),
    re.compile(r"Mission actor ID lifecycle: .*?generation=(\d+)"),
)
REMOTE_ACTOR = re.compile(
    r"Mission actor network trace: side=(?P<side>\S+) tick=(?P<tick>\d+) "
    r"id=(?P<id>\d+) health=(?P<health>\d+) "
    r"x_cm=(?P<x>-?\d+) y_cm=(?P<y>-?\d+) z_cm=(?P<z>-?\d+) "
    r"yaw_mrad=(?P<yaw>-?\d+)"
)


@dataclass(frozen=True)
class Spawn:
    side: str
    generation: int
    call: int
    ant: int
    class_name: str
    faction: int
    awaken_gate: bool
    awaken_latch: int
    health: float
    x: float
    y: float
    z: float
    yaw_valid: bool
    yaw: float

    def key(self, ignore_generation=False):
        ordinal = (self.call, self.ant)
        return ordinal if ignore_generation else (self.generation, *ordinal)


@dataclass(frozen=True)
class Removal:
    side: str
    generation: int
    removal_ordinal: int
    call: int
    ant: int
    class_name: str
    faction: int
    health: float
    x: float
    y: float
    z: float

    def key(self, ignore_generation=False):
        ordinal = (self.call, self.ant)
        return ordinal if ignore_generation else (self.generation, *ordinal)


@dataclass(frozen=True)
class ActorIdMap:
    side: str
    actor_id: int
    generation: int
    call: int
    ant: int

    def key(self, ignore_generation=False):
        ordinal = (self.call, self.ant)
        return ordinal if ignore_generation else (self.generation, *ordinal)


@dataclass(frozen=True)
class ActorIdRemoval:
    side: str
    actor_id: int
    generation: int
    removal_ordinal: int
    call: int
    ant: int

    def key(self, ignore_generation=False):
        ordinal = (self.call, self.ant)
        return ordinal if ignore_generation else (self.generation, *ordinal)


@dataclass(frozen=True)
class RemoteActor:
    side: str
    tick: int
    actor_id: int


def load(path):
    records, removals, actor_id_maps, actor_id_removals, remote_actors, malformed = [], [], [], [], [], 0
    trace_generations = set()
    with open(path, encoding="utf-8", errors="replace") as source:
        for line_number, line in enumerate(source, 1):
            for pattern in TRACE_GENERATION_PATTERNS:
                generation_match = pattern.search(line)
                if generation_match:
                    trace_generations.add(int(generation_match.group(1)))
                    break
            if "Mission actor lifecycle:" in line:
                match = MISSION_ACTOR_LIFECYCLE.search(line)
                if not match:
                    malformed += 1
                    continue
                row = match.groupdict()
                if row["event"] == "identity_assigned":
                    actor_id_maps.append((line_number, ActorIdMap(
                        side=row["side"], actor_id=int(row["id"]),
                        generation=int(row["generation"]), call=int(row["call"]),
                        ant=int(row["ant"]),
                    )))
                elif row["event"] == "removal_hook_invalidated":
                    actor_id_removals.append((line_number, ActorIdRemoval(
                        side=row["side"], actor_id=int(row["id"]),
                        generation=int(row["generation"]), removal_ordinal=0,
                        call=int(row["call"]), ant=int(row["ant"]),
                    )))
                continue
            if "Mission actor ID lifecycle:" in line:
                match = ACTOR_ID_LIFECYCLE.search(line)
                if not match:
                    malformed += 1
                    continue
                row = match.groupdict()
                actor_id_removals.append((line_number, ActorIdRemoval(
                    side=row["side"], actor_id=int(row["id"]),
                    generation=int(row["generation"]), removal_ordinal=int(row["removal"]),
                    call=int(row["call"]), ant=int(row["ant"]),
                )))
                continue
            if "Mission actor ID map:" in line:
                match = ACTOR_ID_MAP.search(line)
                if not match:
                    malformed += 1
                    continue
                row = match.groupdict()
                actor_id_maps.append((line_number, ActorIdMap(
                    side=row["side"], actor_id=int(row["id"]),
                    generation=int(row["generation"]), call=int(row["call"]), ant=int(row["ant"]),
                )))
                continue
            if "Mission actor network trace:" in line:
                if " id=" not in line:
                    continue  # Per-snapshot summary line.
                match = REMOTE_ACTOR.search(line)
                if not match:
                    malformed += 1
                    continue
                row = match.groupdict()
                remote_actors.append((line_number, RemoteActor(
                    side=row["side"], tick=int(row["tick"]), actor_id=int(row["id"]),
                )))
                continue
            if "Mission ant lifecycle trace:" in line:
                match = LIFECYCLE.search(line)
                if not match:
                    malformed += 1
                    continue
                row = match.groupdict()
                removals.append((line_number, Removal(
                    side=row["side"], generation=int(row["generation"]),
                    removal_ordinal=int(row["removal"]), call=int(row["call"]),
                    ant=int(row["ant"]), class_name=row["class"],
                    faction=int(row["faction"]), health=float(row["health"]),
                    x=float(row["x"]), y=float(row["y"]), z=float(row["z"]),
                )))
                continue
            if "Mission ant identity trace:" not in line:
                continue
            match = TRACE.search(line)
            if not match:
                malformed += 1
                continue
            row = match.groupdict()
            records.append((line_number, Spawn(
                side=row["side"], generation=int(row["generation"]),
                call=int(row["call"]), ant=int(row["ant"]),
                class_name=row["class"], faction=int(row["faction"]),
                awaken_gate=row["gate"] == "true", awaken_latch=int(row["latch"]),
                health=float(row["health"]), x=float(row["x"]),
                y=float(row["y"]), z=float(row["z"]),
                yaw_valid=row["yaw_valid"] == "true", yaw=float(row["yaw"]),
            )))
    return records, removals, actor_id_maps, actor_id_removals, remote_actors, malformed, trace_generations


def index(records, ignore_generation):
    grouped = {}
    for line_number, record in records:
        grouped.setdefault(record.key(ignore_generation), []).append((line_number, record))
    duplicates = {key: rows for key, rows in grouped.items() if len(rows) > 1}
    return {key: rows[0][1] for key, rows in grouped.items() if len(rows) == 1}, duplicates


def compare(host, client, args):
    differences = []
    if host.class_name != client.class_name:
        differences.append("class")
    if host.faction != client.faction:
        differences.append("faction")
    if host.awaken_gate != client.awaken_gate:
        differences.append("r7_awaken_gate")
    if host.awaken_latch != client.awaken_latch:
        differences.append("awaken_latch_after")
    if (not math.isfinite(host.health) or not math.isfinite(client.health) or
            abs(host.health - client.health) > args.health_tolerance):
        differences.append("health")
    host_position = (host.x, host.y, host.z)
    client_position = (client.x, client.y, client.z)
    position_delta = math.dist(host_position, client_position)
    if (not all(math.isfinite(value) for value in (*host_position, *client_position)) or
            not math.isfinite(position_delta) or position_delta > args.position_tolerance):
        differences.append("position")
    yaw_is_finite = math.isfinite(host.yaw) and math.isfinite(client.yaw)
    yaw_delta = (abs(math.remainder(host.yaw - client.yaw, 2.0 * math.pi))
                 if yaw_is_finite else math.inf)
    if not yaw_is_finite or host.yaw_valid != client.yaw_valid or (
        host.yaw_valid and client.yaw_valid and
        yaw_delta > args.yaw_tolerance
    ):
        differences.append("yaw")
    return differences, position_delta, yaw_delta


def compare_removal(host, client, args):
    differences = []
    if host.class_name != client.class_name:
        differences.append("class")
    if host.faction != client.faction:
        differences.append("faction")
    if (not math.isfinite(host.health) or not math.isfinite(client.health) or
            abs(host.health - client.health) > args.health_tolerance):
        differences.append("health")
    position_delta = math.dist((host.x, host.y, host.z), (client.x, client.y, client.z))
    if not math.isfinite(position_delta) or position_delta > args.position_tolerance:
        differences.append("position")
    return differences, position_delta


def format_key(key):
    return "/".join(str(part) for part in key)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("host_log")
    parser.add_argument("client_log")
    parser.add_argument("--ignore-generation", action="store_true",
                        help="compare ordinal-only candidates for single-generation captures; cannot validate network ID mapping")
    parser.add_argument("--position-tolerance", type=float, default=0.25, metavar="METERS")
    parser.add_argument("--health-tolerance", type=float, default=1.0)
    parser.add_argument("--yaw-tolerance", type=float, default=0.03, metavar="RADIANS")
    parser.add_argument("--show", type=int, default=10, help="maximum differing keys to print")
    parser.add_argument("--require-lifecycle", action="store_true",
                        help="fail when either log contains no ant removal events")
    parser.add_argument("--require-network-mapping", action="store_true",
                        help="require each logged client-snapshot ID to map to a unique exact host/client spawn key; not identity proof")
    args = parser.parse_args()
    if args.ignore_generation and args.require_network_mapping:
        parser.error("--ignore-generation cannot be combined with --require-network-mapping; ordinal-only mode cannot validate network ID mapping")
    tolerances = (args.position_tolerance, args.health_tolerance, args.yaw_tolerance)
    if not all(math.isfinite(value) and value >= 0 for value in tolerances) or args.show < 0:
        parser.error("tolerances must be finite and nonnegative; --show must be nonnegative")

    try:
        host_rows, host_removals, host_id_maps, host_id_removals, _, host_malformed, host_generations = load(args.host_log)
        client_rows, client_removals, _, _, remote_actors, client_malformed, client_generations = load(args.client_log)
    except OSError as error:
        print(error, file=sys.stderr)
        return 2
    if not host_rows or not client_rows:
        print(f"trace records found: host={len(host_rows)}, client={len(client_rows)}; "
              "capture both logs with edf_mp_actor_trace=true", file=sys.stderr)
        return 2
    if args.ignore_generation:
        for label, generations in (("host", host_generations), ("client", client_generations)):
            if len(generations) != 1:
                print(f"--ignore-generation requires exactly one trace generation in each log; "
                      f"{label} has {len(generations)} ({sorted(generations)})", file=sys.stderr)
                return 2

    host_sides = {row.side for _, row in host_rows + host_removals}
    client_sides = {row.side for _, row in client_rows + client_removals}
    sides_match = host_sides == {"authoritative_host"} and client_sides == {"client"}
    if not sides_match:
        print("expected side labels: host=authoritative_host, client=client; "
              f"got host={sorted(host_sides)}, client={sorted(client_sides)}", file=sys.stderr)

    host, host_duplicates = index(host_rows, args.ignore_generation)
    client, client_duplicates = index(client_rows, args.ignore_generation)
    common = sorted(set(host) & set(client))
    host_only = sorted(set(host) - set(client))
    client_only = sorted(set(client) - set(host))
    mismatches = []
    for key in common:
        fields, distance, yaw_delta = compare(host[key], client[key], args)
        if fields:
            mismatches.append((key, fields, distance, yaw_delta))

    host_removed, host_removal_duplicates = index(host_removals, args.ignore_generation)
    client_removed, client_removal_duplicates = index(client_removals, args.ignore_generation)
    removal_common = sorted(set(host_removed) & set(client_removed))
    removal_host_only = sorted(set(host_removed) - set(client_removed))
    removal_client_only = sorted(set(client_removed) - set(host_removed))
    removal_mismatches = []
    for key in removal_common:
        fields, distance = compare_removal(host_removed[key], client_removed[key], args)
        if fields:
            removal_mismatches.append((key, fields, distance))
    def removal_order(rows):
        ordered = sorted(rows, key=lambda item: (item[1].generation, item[1].removal_ordinal))
        return [row.key(args.ignore_generation) for _, row in ordered]

    host_removal_order = removal_order(host_removals)
    client_removal_order = removal_order(client_removals)
    removal_order_matches = host_removal_order == client_removal_order

    if not args.ignore_generation:
        host_actor_ids = {}
        host_spawn_key_ids = {}
        duplicate_actor_ids = set()
        for _, mapping in host_id_maps:
            if mapping.actor_id in host_actor_ids:
                duplicate_actor_ids.add(mapping.actor_id)
            else:
                host_actor_ids[mapping.actor_id] = mapping
            host_spawn_key_ids.setdefault(mapping.key(), set()).add(mapping.actor_id)
        duplicate_spawn_keys = {
            key for key, actor_ids in host_spawn_key_ids.items() if len(actor_ids) > 1
        }
        remote_actor_ids = {row.actor_id for _, row in remote_actors}
        overlapping_remote_ids = {
            actor_id: host_actor_ids[actor_id].key()
            for actor_id in remote_actor_ids
            if actor_id in host_actor_ids and
            actor_id not in duplicate_actor_ids and
            host_actor_ids[actor_id].key() not in duplicate_spawn_keys and
            host_actor_ids[actor_id].key() in host and
            host_actor_ids[actor_id].key() not in host_duplicates and
            host_actor_ids[actor_id].key() in client and
            host_actor_ids[actor_id].key() not in client_duplicates
        }
        unresolved_remote_ids = sorted(remote_actor_ids - set(overlapping_remote_ids))
        host_map_sides = {row.side for _, row in host_id_maps}
        remote_sides = {row.side for _, row in remote_actors}
        network_sides_match = (not host_map_sides or host_map_sides == {"authoritative_host"}) and (
            not remote_sides or remote_sides == {"client_remote"})
        client_removal_keys = set(client_removed)
        mapped_host_id_removals = sum(
            row.key() in client_removal_keys
            for _, row in host_id_removals
        )

    print(f"records: host={len(host_rows)} ({host_malformed} malformed), "
          f"client={len(client_rows)} ({client_malformed} malformed)")
    print(f"sides: host={dict(Counter(row.side for _, row in host_rows))}; "
          f"client={dict(Counter(row.side for _, row in client_rows))}; "
          f"removals host={dict(Counter(row.side for _, row in host_removals))}, "
          f"client={dict(Counter(row.side for _, row in client_removals))}")
    key_label = "ordinal-only candidates" if args.ignore_generation else "exact spawn keys"
    print(f"{key_label}: {len(common)} common, {len(host_only)} host-only, "
          f"{len(client_only)} client-only; ambiguous duplicate keys="
          f"{len(host_duplicates)}/{len(client_duplicates)} host/client")
    print(f"common keys with gameplay-field differences: {len(mismatches)}")
    for key, fields, distance, yaw_delta in mismatches[:args.show]:
        print(f"  {format_key(key)}: {', '.join(fields)} "
              f"(position_delta={distance:.3f}m, yaw_delta={yaw_delta:.3f}rad)")
    for label, keys in (("host-only", host_only), ("client-only", client_only)):
        for key in keys[:args.show]:
            print(f"  {label}: {format_key(key)}")
    removal_key_label = "ordinal-only removal candidates" if args.ignore_generation else "spawn keys"
    print(f"removal records: host={len(host_removals)}, client={len(client_removals)}; "
          f"{removal_key_label}={len(removal_common)} common, {len(removal_host_only)} host-only, "
          f"{len(removal_client_only)} client-only; ambiguous duplicates="
          f"{len(host_removal_duplicates)}/{len(client_removal_duplicates)} host/client")
    if host_removals or client_removals:
        print(f"removal order: {'matches' if removal_order_matches else 'differs'} "
              "for candidate ordinal keys")
        print(f"common removal keys with state differences: {len(removal_mismatches)}")
        for key, fields, distance in removal_mismatches[:args.show]:
            print(f"  {format_key(key)}: {', '.join(fields)} (position_delta={distance:.3f}m)")
        for label, keys in (("removal host-only", removal_host_only),
                            ("removal client-only", removal_client_only)):
            for key in keys[:args.show]:
                print(f"  {label}: {format_key(key)}")
    elif args.require_lifecycle:
        print("no ant removal events found; recapture through at least one removal")
    else:
        print("no ant removal events captured in both logs")
    if args.ignore_generation:
        print("Network ID mapping not evaluated: ordinal-only fallback cannot validate it.")
    else:
        print(f"network spawn-key overlap candidates: host ID maps={len(host_id_maps)}, "
              f"client remote rows={len(remote_actors)}, unique remote IDs={len(remote_actor_ids)}, "
              f"overlap candidates={len(overlapping_remote_ids)}, unresolved={len(unresolved_remote_ids)}, "
              f"duplicate host IDs={len(duplicate_actor_ids)}, duplicate spawn-key maps={len(duplicate_spawn_keys)}")
        for actor_id, key in sorted(overlapping_remote_ids.items())[:args.show]:
            print(f"  remote ID 0x{actor_id:08x} -> exact spawn-key candidate {format_key(key)}")
        for actor_id in unresolved_remote_ids[:args.show]:
            print(f"  unresolved remote ID 0x{actor_id:08x}: no unique host/client spawn-key overlap")
        if host_id_removals:
            print(f"host ID lifecycle: removals={len(host_id_removals)}, corresponding client-local "
                  f"removal ordinals={mapped_host_id_removals}")
        if not network_sides_match:
            print(f"network trace side labels are unexpected: host ID maps={sorted(host_map_sides)}, "
                  f"client remote={sorted(remote_sides)}")
        print("Spawn-key overlap is only a candidate, not proof of cross-process actor identity. Validate repeated runs, "
              "actor lifecycle/death ordering, and the client AI gate before enabling replication.")
    if args.require_lifecycle and (not host_removals or not client_removals):
        return 2
    if not args.ignore_generation and args.require_network_mapping and (
        not host_id_maps or not remote_actors or unresolved_remote_ids or duplicate_actor_ids or
        duplicate_spawn_keys or
        not network_sides_match
    ):
        return 2
    return 1 if (not sides_match or host_malformed or client_malformed or host_only or client_only or
                 host_duplicates or client_duplicates or mismatches or removal_host_only or
                 removal_client_only or host_removal_duplicates or client_removal_duplicates or
                  removal_mismatches or (not args.ignore_generation and duplicate_actor_ids) or
                  ((host_removals or client_removals) and not removal_order_matches) or
                  (not args.ignore_generation and not network_sides_match)) else 0


if __name__ == "__main__":
    sys.exit(main())
