-- Example trusted-server policy, not an EDF weapon simulation.
-- A rising right trigger applies fixed custom damage to the nearest live ant
-- in a horizontal cone around the accepted client aim yaw. A rendered local
-- host can include its host-sampled EDF mission ants; a standalone server uses
-- its own server-owned ant actors. The client never chooses target or damage.
-- Ordinary mission ants are deliberately excluded from the enemy attack below.
-- On a standalone headless server, each server-owned ant chases and bites the
-- nearest living player using server-visible transforms. A guest-backed host
-- leaves native EDF AI in control. This is a simple mod policy, not EDF AI.

local RANGE_CM = 1800
local MAX_VERTICAL_OFFSET_CM = 300
local AIM_CONE_COS_SQUARED = 0.8830222216 -- cos(20 degrees)^2
local DAMAGE_POINTS = 25
local ACTION_COOLDOWN_TICKS = 12
local MAX_ACTIONS_PER_TICK = 8
local ACTION_TTL_TICKS = 120
local ANT_SPEED_CM_PER_TICK = 4
local ANT_STOP_DISTANCE_CM = 120
local ANT_ATTACK_RANGE_CM = 180
local ANT_ATTACK_VERTICAL_OFFSET_CM = 220
local ANT_ATTACK_DAMAGE = 12
local ANT_ATTACK_COOLDOWN_TICKS = 45

local script_tick = 0
local spawn_complete = false
local next_spawn_attempt = 0
local previous_trigger = {}
local next_action_tick = {}
local next_ant_attack_tick = {}
local pending_actions = {}
local pending_by_player = {}

local function remove_player_pending(player_id)
	local remaining = {}
	for _, action in ipairs(pending_actions) do
		if action.player_id ~= player_id then
			table.insert(remaining, action)
		end
	end
	pending_actions = remaining
	pending_by_player[player_id] = nil
end

local function move_headless_ant(actor, players)
	local target = nil
	local nearest_distance_squared = math.huge
	for _, player in ipairs(players) do
		if player.health_valid and player.alive and player.health > 0 then
			local dx = player.x_cm - actor.x_cm
			local dy = player.y_cm - actor.y_cm
			local dz = player.z_cm - actor.z_cm
			local distance_squared = dx * dx + dy * dy + dz * dz
			if distance_squared < nearest_distance_squared then
				target = player
				nearest_distance_squared = distance_squared
			end
		end
	end
	if not target then
		return
	end

	local dx = target.x_cm - actor.x_cm
	local dz = target.z_cm - actor.z_cm
	local distance = math.sqrt(dx * dx + dz * dz)
	local dy = target.y_cm - actor.y_cm
	if distance <= ANT_ATTACK_RANGE_CM
		and math.abs(dy) <= ANT_ATTACK_VERTICAL_OFFSET_CM then
		if script_tick >= (next_ant_attack_tick[target.id] or 0)
			and edf.damage_player(target.id, ANT_ATTACK_DAMAGE) then
			next_ant_attack_tick[target.id] = script_tick + ANT_ATTACK_COOLDOWN_TICKS
		end
	end
	if distance <= ANT_STOP_DISTANCE_CM then
		return
	end
	local travel = math.min(ANT_SPEED_CM_PER_TICK, distance - ANT_STOP_DISTANCE_CM)
	local next_x = math.floor(actor.x_cm + dx / distance * travel + 0.5)
	local next_z = math.floor(actor.z_cm + dz / distance * travel + 0.5)
	local yaw_mrad = math.floor(math.atan2(dx, dz) * 1000 + 0.5)
	-- Returns false for actors driven by the real guest simulation; their native
	-- AI owns movement. Headless server actors accept this bounded transform.
	edf.set_actor_transform(actor.id, next_x, actor.y_cm, next_z, yaw_mrad)
end

edf.on("player_leave", function(player_id)
	previous_trigger[player_id] = nil
	next_action_tick[player_id] = nil
	next_ant_attack_tick[player_id] = nil
	remove_player_pending(player_id)
end)

edf.on("tick", function(_tick)
	script_tick += 1
	local players = edf.players()
	local actors = edf.actors()
	local present = {}
	local active_players = {}
	local live_actor_ids = {}
	local range_squared = RANGE_CM * RANGE_CM
	local has_server_ant = false
	local has_live_ant = false

	for _, actor in ipairs(actors) do
		if actor.archetype == "ant" and actor.health > 0 then
			has_live_ant = true
			live_actor_ids[actor.id] = true
			if actor.server_owned then
				has_server_ant = true
				move_headless_ant(actor, players)
			end
		end
	end
	if has_server_ant then
		spawn_complete = true
	elseif has_live_ant then
		-- A guest-backed host already has native mission enemies. Do not add the
		-- demonstration ant when those are available.
		spawn_complete = true
	elseif not spawn_complete and script_tick >= next_spawn_attempt then
		local ant_id = edf.spawn_actor("ant", 1200, 0, 0, 0)
		if ant_id then
			spawn_complete = true
			edf.broadcast("Server-owned ant spawn accepted as " .. ant_id)
		else
			next_spawn_attempt = script_tick + 120
		end
	end

	for _, player in ipairs(players) do
		present[player.id] = true
		active_players[player.id] = player.active
	end
	for player_id in pairs(next_ant_attack_tick) do
		if not present[player_id] then
			next_ant_attack_tick[player_id] = nil
		end
	end

	for id in pairs(previous_trigger) do
		if not present[id] then
			previous_trigger[id] = nil
			next_action_tick[id] = nil
			remove_player_pending(id)
		end
	end

	local live_pending = {}
	for _, action in ipairs(pending_actions) do
		if active_players[action.player_id]
			and script_tick < action.expires_at
			and live_actor_ids[action.actor_id]
		then
			table.insert(live_pending, action)
		end
	end
	pending_actions = live_pending
	pending_by_player = {}
	for _, action in ipairs(pending_actions) do
		pending_by_player[action.player_id] = action
	end

	for _, player in ipairs(players) do
		local id = player.id
		local trigger_down = player.active and player.right_trigger > 0
		local trigger_pressed = trigger_down and not previous_trigger[id]
		previous_trigger[id] = trigger_down

		if trigger_pressed and not pending_by_player[id]
			and script_tick >= (next_action_tick[id] or 0) then
			local nearest = nil
			local nearest_distance_squared = range_squared + 1
			local yaw = player.aim_yaw_mrad / 1000
			local forward_x = math.sin(yaw)
			local forward_z = math.cos(yaw)
			for _, actor in ipairs(actors) do
				if actor.archetype == "ant" and actor.health > 0 then
					local dx = actor.x_cm - player.x_cm
					local dy = actor.y_cm - player.y_cm
					local dz = actor.z_cm - player.z_cm
					local distance_squared = dx * dx + dy * dy + dz * dz
					local horizontal_distance_squared = dx * dx + dz * dz
					local aim_dot = dx * forward_x + dz * forward_z
					local inside_aim_cone = aim_dot > 0
						and aim_dot * aim_dot >= horizontal_distance_squared * AIM_CONE_COS_SQUARED
					if distance_squared <= range_squared
						and math.abs(dy) <= MAX_VERTICAL_OFFSET_CM
						and inside_aim_cone
						and distance_squared < nearest_distance_squared then
						nearest = actor
						nearest_distance_squared = distance_squared
					end
				end
			end

			if nearest then
				local action = {
					player_id = id,
					actor_id = nearest.id,
					expires_at = script_tick + ACTION_TTL_TICKS,
				}
				table.insert(pending_actions, action)
				pending_by_player[id] = action
				next_action_tick[id] = script_tick + ACTION_COOLDOWN_TICKS
			end
		end
	end

	-- Visit at most the queue length from the start of this tick so a failed
	-- request can move to the tail without being retried again immediately.
	local queued_at_start = #pending_actions
	local visited = 0
	local attempts = 0
	local retries = {}
	while visited < queued_at_start and attempts < MAX_ACTIONS_PER_TICK do
		local action = table.remove(pending_actions, 1)
		visited += 1
		attempts += 1
		if not edf.damage_actor(action.actor_id, DAMAGE_POINTS) then
			table.insert(retries, action)
		end
	end
	for _, action in ipairs(retries) do
		table.insert(pending_actions, action)
	end

	pending_by_player = {}
	for _, action in ipairs(pending_actions) do
		pending_by_player[action.player_id] = action
	end
end)
