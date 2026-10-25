-- Example server-side damage policy for the headless
-- --server-authoritative-movement mode. This does not model EDF combat.
-- It intentionally targets every connected living player; client-reported
-- gameplay activity is not used as a damage exemption.
local HAZARD_INTERVAL_TICKS = 600 -- 10 seconds at 60 Hz
local HAZARD_DAMAGE = 5
local next_hazard_tick = HAZARD_INTERVAL_TICKS

edf.on("tick", function(tick)
	if tick < next_hazard_tick then
		return
	end
	next_hazard_tick = tick + HAZARD_INTERVAL_TICKS

	for _, player in ipairs(edf.players()) do
		if player.health_valid and player.alive then
			edf.damage_player(player.id, HAZARD_DAMAGE)
		end
	end
end)
