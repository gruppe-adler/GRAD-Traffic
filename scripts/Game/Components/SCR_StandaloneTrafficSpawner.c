// ============================================================================
// GRAD-Traffic Standalone Spawner
// ============================================================================
// A fully self-contained traffic spawner that works on ANY map without
// requiring GameMode configuration, mission headers, or entity catalogs.
//
// USAGE:
// 1. Place this component on ANY entity in your mission (even an empty one)
// 2. Configure the attributes in the World Editor or leave defaults
// 3. Traffic will spawn automatically when the mission starts
//
// The spawner will auto-detect:
// - Road networks from SCR_AIWorld
// - Player positions for spawn/despawn logic
// - Available civilian factions
// ============================================================================

[ComponentEditorProps(category: "Traffic System", description: "Standalone traffic spawner - works on any map without configuration")]
class SCR_StandaloneTrafficSpawnerClass : ScriptComponentClass {}

class SCR_StandaloneTrafficSpawner : ScriptComponent
{
	// ========================================================================
	// Configuration - All with sensible defaults
	// ========================================================================

	[Attribute("1", UIWidgets.CheckBox, desc: "Enable traffic spawning")]
	protected bool m_bEnabled;

	[Attribute("10", UIWidgets.Slider, desc: "Maximum active vehicles", params: "1 50 1")]
	protected int m_iMaxVehicles;

	[Attribute("2000", UIWidgets.Slider, desc: "Despawn distance from players (meters)", params: "500 5000 100")]
	protected float m_fDespawnDistance;

	[Attribute("400", UIWidgets.Slider, desc: "Safe zone radius - won't spawn/despawn near players", params: "100 1000 50")]
	protected float m_fPlayerSafeRadius;

	[Attribute("CIV", UIWidgets.EditBox, desc: "Faction key for drivers and vehicles")]
	protected string m_sFactionKey;

	[Attribute("1", UIWidgets.CheckBox, desc: "Use behavior tree for advanced AI (stuck recovery, varied panic)")]
	protected bool m_bUseBehaviorTree;

	[Attribute("0", UIWidgets.CheckBox, desc: "Show debug markers and lines")]
	protected bool m_bShowDebug;

	// --- Vehicle Prefabs (with defaults) ---
	[Attribute(desc: "Vehicle prefabs to spawn. If empty, will auto-detect from faction catalog or use fallbacks.")]
	protected ref array<ResourceName> m_aVehiclePrefabs;

	// --- Core Prefabs (with working defaults) ---
	[Attribute("{22E43956740A6794}Prefabs/Characters/Factions/CIV/GenericCivilians/Character_CIV_Randomized.et", UIWidgets.ResourceNamePicker, desc: "Driver character prefab", params: "et")]
	protected ResourceName m_DriverPrefab;

	[Attribute("{750A8D1695BD6998}Prefabs/AI/Waypoints/AIWaypoint_Move.et", UIWidgets.ResourceNamePicker, desc: "Waypoint prefab for navigation", params: "et")]
	protected ResourceName m_WaypointPrefab;

	[Attribute("{5C5DDBF12CA4FC46}Prefabs/Groups/INDFOR/Group_FIA_Team_Sentries.et", UIWidgets.ResourceNamePicker, desc: "AI Group prefab", params: "et")]
	protected ResourceName m_GroupPrefab;

	// ========================================================================
	// Internal State
	// ========================================================================

	protected ref array<Vehicle> m_aActiveVehicles = {};
	protected ref map<Vehicle, vector> m_mVehicleDestinations = new map<Vehicle, vector>();
	protected ref array<Vehicle> m_aAbandonedVehicles = {};
	protected ref array<ref Shape> m_aDebugShapes = {};
	protected bool m_bInitialized = false;

	// ========================================================================
	// Common Civilian Vehicle Prefabs (Fallbacks)
	// ========================================================================

	// These are standard Reforger civilian vehicle prefabs that exist in vanilla
	static const ref array<string> DEFAULT_CIVILIAN_VEHICLES = {
		"{2A8A8B72369B5765}Prefabs/Vehicles/Wheeled/S1203/S1203_transport_CIV.et",
		"{E7C4D8176E09E19B}Prefabs/Vehicles/Wheeled/UAZ469/UAZ469_CIV.et",
		"{CF76689A2E364B92}Prefabs/Vehicles/Wheeled/M998/M1025_unarmed_CIVWL.et"
	};

	// ========================================================================
	// Initialization
	// ========================================================================

	override void OnPostInit(IEntity owner)
	{
		if (!Replication.IsServer()) return;
		if (!m_bEnabled) return;

		// Delay initialization to allow world to fully load
		GetGame().GetCallqueue().CallLater(Initialize, 3000, false);
	}

	protected void Initialize()
	{
		Print("[TRAFFIC-STANDALONE] Initializing standalone traffic spawner...", LogLevel.NORMAL);

		// Validate road network exists
		SCR_AIWorld aiWorld = SCR_AIWorld.Cast(GetGame().GetAIWorld());
		if (!aiWorld)
		{
			Print("[TRAFFIC-STANDALONE] ERROR: No SCR_AIWorld found - traffic requires AI world with road network!", LogLevel.ERROR);
			return;
		}

		RoadNetworkManager roadMgr = aiWorld.GetRoadNetworkManager();
		if (!roadMgr)
		{
			Print("[TRAFFIC-STANDALONE] ERROR: No RoadNetworkManager found - this map may not have road data!", LogLevel.ERROR);
			return;
		}

		// Initialize vehicle prefabs
		if (!InitializeVehiclePrefabs())
		{
			Print("[TRAFFIC-STANDALONE] ERROR: No valid vehicle prefabs found!", LogLevel.ERROR);
			return;
		}

		// Subscribe to behavior tree events
		if (m_bUseBehaviorTree)
			SCR_TrafficEvents.OnVehicleAbandoned.Insert(OnVehicleAbandoned);

		m_bInitialized = true;

		Print(string.Format("[TRAFFIC-STANDALONE] Initialized successfully! Max vehicles: %1, Faction: %2, Vehicles available: %3",
			m_iMaxVehicles, m_sFactionKey, m_aVehiclePrefabs.Count()), LogLevel.NORMAL);

		// Start the main loop
		GetGame().GetCallqueue().CallLater(UpdateLoop, 1000, true);
	}

	protected bool InitializeVehiclePrefabs()
	{
		// If prefabs are already configured, validate them
		if (m_aVehiclePrefabs && m_aVehiclePrefabs.Count() > 0)
		{
			Print(string.Format("[TRAFFIC-STANDALONE] Using %1 pre-configured vehicle prefabs", m_aVehiclePrefabs.Count()), LogLevel.NORMAL);
			return true;
		}

		// Initialize array if null
		if (!m_aVehiclePrefabs)
			m_aVehiclePrefabs = {};

		// Try to get vehicles from entity catalog first
		if (TryLoadFromCatalog())
		{
			Print(string.Format("[TRAFFIC-STANDALONE] Loaded %1 vehicles from entity catalog", m_aVehiclePrefabs.Count()), LogLevel.NORMAL);
			return m_aVehiclePrefabs.Count() > 0;
		}

		// Fallback: Try default civilian vehicle paths
		Print("[TRAFFIC-STANDALONE] No catalog available, trying default vehicle prefabs...", LogLevel.WARNING);

		foreach (string prefabPath : DEFAULT_CIVILIAN_VEHICLES)
		{
			Resource res = Resource.Load(prefabPath);
			if (res && res.IsValid())
			{
				m_aVehiclePrefabs.Insert(prefabPath);
				Print(string.Format("[TRAFFIC-STANDALONE] Found default vehicle: %1", prefabPath), LogLevel.NORMAL);
			}
		}

		if (m_aVehiclePrefabs.Count() == 0)
		{
			Print("[TRAFFIC-STANDALONE] WARNING: No default vehicles found, attempting generic search...", LogLevel.WARNING);
			return TryFindAnyVehicle();
		}

		return m_aVehiclePrefabs.Count() > 0;
	}

	protected bool TryLoadFromCatalog()
	{
		// Try to find catalog manager on any game mode
		BaseGameMode gameMode = GetGame().GetGameMode();
		if (!gameMode) return false;

		SCR_EntityCatalogManagerComponent catalogManager = SCR_EntityCatalogManagerComponent.Cast(gameMode.FindComponent(SCR_EntityCatalogManagerComponent));
		if (!catalogManager) return false;

		SCR_EntityCatalog catalog = catalogManager.GetEntityCatalogOfType(EEntityCatalogType.VEHICLE);
		if (!catalog) return false;

		array<SCR_EntityCatalogEntry> entries;
		catalog.GetEntityList(entries);
		if (!entries) return false;

		foreach (SCR_EntityCatalogEntry entry : entries)
		{
			ResourceName prefab = entry.GetPrefab();
			if (prefab == "") continue;

			// Check if vehicle belongs to target faction
			if (IsPrefabFromFaction(prefab, m_sFactionKey))
			{
				m_aVehiclePrefabs.Insert(prefab);
			}
		}

		return m_aVehiclePrefabs.Count() > 0;
	}

	protected bool IsPrefabFromFaction(ResourceName prefab, string targetFaction)
	{
		Resource resource = Resource.Load(prefab);
		if (!resource || !resource.IsValid()) return false;

		IEntitySource source = resource.GetResource().ToEntitySource();

		for (int i = 0; i < source.GetComponentCount(); i++)
		{
			IEntityComponentSource compSource = source.GetComponent(i);
			if (compSource.GetClassName().Contains("FactionAffiliationComponent"))
			{
				string prefabFaction;
				compSource.Get("m_sFactionKey", prefabFaction);
				return prefabFaction == targetFaction;
			}
		}

		// If no faction component, check if prefab name contains CIV
		string prefabStr = prefab;
		return prefabStr.Contains("CIV") || prefabStr.Contains("_civ") || prefabStr.Contains("Civilian");
	}

	protected bool TryFindAnyVehicle()
	{
		// Last resort: search for any drivable vehicle in common paths
		array<string> searchPaths = {
			"Prefabs/Vehicles/Wheeled/",
			"Prefabs/Vehicles/Car/",
			"Prefabs/Vehicles/Truck/"
		};

		// This is a fallback - in real use, configure prefabs directly
		Print("[TRAFFIC-STANDALONE] No vehicles found - please configure m_aVehiclePrefabs manually!", LogLevel.ERROR);
		return false;
	}

	// ========================================================================
	// Main Update Loop
	// ========================================================================

	protected void UpdateLoop()
	{
		if (!m_bInitialized) return;

		// Cleanup destroyed/despawned vehicles
		CleanupVehicles();

		// Spawn new vehicles if needed
		if (m_aActiveVehicles.Count() < m_iMaxVehicles)
			SpawnVehicle();

		// Update debug visualization
		if (m_bShowDebug)
			UpdateDebugVisualization();
	}

	// ========================================================================
	// Vehicle Spawning
	// ========================================================================

	protected void SpawnVehicle()
	{
		if (m_aVehiclePrefabs.IsEmpty())
		{
			Print("[TRAFFIC-STANDALONE] No vehicle prefabs available!", LogLevel.ERROR);
			return;
		}

		// Find valid road positions
		vector spawnPos, destPos;
		if (!FindValidRoadPoints(spawnPos, destPos))
		{
			if (m_bShowDebug)
				Print("[TRAFFIC-STANDALONE] Failed to find valid road points, retrying next loop...", LogLevel.DEBUG);
			return;
		}

		// Check player proximity for spawn position
		if (IsPlayerNearby(spawnPos, m_fPlayerSafeRadius))
			return;

		EntitySpawnParams params = new EntitySpawnParams();
		params.TransformMode = ETransformMode.WORLD;
		params.Transform[3] = spawnPos;

		// 1. Spawn AI Group
		IEntity groupEnt = GetGame().SpawnEntityPrefab(Resource.Load(m_GroupPrefab), GetGame().GetWorld(), params);
		SCR_AIGroup group = SCR_AIGroup.Cast(groupEnt);
		if (!group)
		{
			Print("[TRAFFIC-STANDALONE] Failed to spawn AI group!", LogLevel.ERROR);
			return;
		}

		// Set faction
		FactionManager factionMgr = GetGame().GetFactionManager();
		if (factionMgr)
		{
			Faction faction = factionMgr.GetFactionByKey(m_sFactionKey);
			if (faction)
				group.SetFaction(faction);
		}

		// 2. Spawn Vehicle
		ResourceName vehiclePrefab = m_aVehiclePrefabs.GetRandomElement();
		IEntity vehEnt = GetGame().SpawnEntityPrefab(Resource.Load(vehiclePrefab), GetGame().GetWorld(), params);
		Vehicle vehicle = Vehicle.Cast(vehEnt);
		if (!vehicle)
		{
			Print("[TRAFFIC-STANDALONE] Failed to spawn vehicle!", LogLevel.ERROR);
			SCR_EntityHelper.DeleteEntityAndChildren(group);
			return;
		}

		m_aActiveVehicles.Insert(vehicle);
		m_mVehicleDestinations.Insert(vehicle, destPos);

		// 3. Spawn Driver
		IEntity driverEnt = GetGame().SpawnEntityPrefab(Resource.Load(m_DriverPrefab), GetGame().GetWorld(), params);
		if (!driverEnt)
		{
			Print("[TRAFFIC-STANDALONE] Failed to spawn driver!", LogLevel.ERROR);
			SCR_EntityHelper.DeleteEntityAndChildren(group);
			SCR_EntityHelper.DeleteEntityAndChildren(vehicle);
			m_aActiveVehicles.RemoveItem(vehicle);
			return;
		}

		// 4. Link driver to group
		AIControlComponent aiControl = AIControlComponent.Cast(driverEnt.FindComponent(AIControlComponent));
		if (aiControl)
		{
			AIAgent agent = aiControl.GetControlAIAgent();
			if (agent)
			{
				agent.PreventMaxLOD();
				group.AddAgent(agent);
			}
		}

		// Set group to passive
		SCR_AIGroupUtilityComponent utility = SCR_AIGroupUtilityComponent.Cast(group.FindComponent(SCR_AIGroupUtilityComponent));
		if (utility)
			utility.SetCombatMode(EAIGroupCombatMode.HOLD_FIRE);

		// 5. Seat driver in vehicle
		if (!SeatDriverInVehicle(vehicle, driverEnt))
		{
			Print("[TRAFFIC-STANDALONE] Failed to seat driver in vehicle!", LogLevel.ERROR);
		}

		// Wake up AI
		if (aiControl)
		{
			AIAgent agent = aiControl.GetControlAIAgent();
			if (agent)
			{
				agent.DeactivateAI();
				agent.ActivateAI();
			}
		}

		// 6. Start vehicle
		StartVehicle(vehicle);

		// 7. Assign waypoint (delayed to let AI initialize)
		GetGame().GetCallqueue().CallLater(AssignWaypoint, 2000, false, group, destPos);

		if (m_bShowDebug)
			Print(string.Format("[TRAFFIC-STANDALONE] Spawned vehicle at %1, heading to %2", spawnPos, destPos), LogLevel.NORMAL);
	}

	protected bool FindValidRoadPoints(out vector spawn, out vector dest)
	{
		SCR_AIWorld aiWorld = SCR_AIWorld.Cast(GetGame().GetAIWorld());
		if (!aiWorld) return false;

		RoadNetworkManager roadMgr = aiWorld.GetRoadNetworkManager();
		if (!roadMgr) return false;

		for (int attempt = 0; attempt < 15; attempt++)
		{
			// Find random spawn point on road
			vector randomPos = GetRandomWorldPosition();
			BaseRoad spawnRoad;
			float roadDist;

			if (roadMgr.GetClosestRoad(randomPos, spawnRoad, roadDist) == -1)
				continue;

			array<vector> roadPoints = {};
			spawnRoad.GetPoints(roadPoints);
			if (roadPoints.IsEmpty()) continue;

			spawn = roadPoints[0];

			// Find destination at least 2km away
			vector destRandom = GetRandomWorldPosition();
			if (vector.Distance(spawn, destRandom) < 2000)
				continue;

			// Verify reachability
			vector validDest;
			if (roadMgr.GetReachableWaypointInRoad(spawn, destRandom, 500.0, validDest))
			{
				dest = validDest;
				return true;
			}
		}

		return false;
	}

	protected vector GetRandomWorldPosition()
	{
		vector worldMin, worldMax;
		GetGame().GetWorldEntity().GetWorldBounds(worldMin, worldMax);

		return Vector(
			Math.RandomFloat(worldMin[0], worldMax[0]),
			0,
			Math.RandomFloat(worldMin[2], worldMax[2])
		);
	}

	protected bool SeatDriverInVehicle(Vehicle vehicle, IEntity driver)
	{
		BaseCompartmentManagerComponent compMgr = BaseCompartmentManagerComponent.Cast(vehicle.FindComponent(BaseCompartmentManagerComponent));
		CompartmentAccessComponent access = CompartmentAccessComponent.Cast(driver.FindComponent(CompartmentAccessComponent));

		if (!compMgr || !access) return false;

		array<BaseCompartmentSlot> compartments = {};
		compMgr.GetCompartments(compartments);

		foreach (BaseCompartmentSlot slot : compartments)
		{
			if (slot.GetType() == ECompartmentType.PILOT)
				return access.GetInVehicle(vehicle, slot, true, -1, ECloseDoorAfterActions.INVALID, false);
		}

		return false;
	}

	protected void StartVehicle(Vehicle vehicle)
	{
		CarControllerComponent carController = CarControllerComponent.Cast(vehicle.FindComponent(CarControllerComponent));
		if (carController)
		{
			carController.StartEngine();
			carController.SetPersistentHandBrake(false);
		}
	}

	protected void AssignWaypoint(SCR_AIGroup group, vector destPos)
	{
		if (!group) return;
		if (group.GetAgentsCount() == 0) return;

		SCR_AIWorld aiWorld = SCR_AIWorld.Cast(GetGame().GetAIWorld());
		if (!aiWorld) return;

		RoadNetworkManager roadMgr = aiWorld.GetRoadNetworkManager();

		vector reachablePos;
		if (!roadMgr.GetReachableWaypointInRoad(group.GetOrigin(), destPos, 20.0, reachablePos))
			reachablePos = destPos;

		EntitySpawnParams params = new EntitySpawnParams();
		params.Transform[3] = reachablePos;

		IEntity wpEnt = GetGame().SpawnEntityPrefab(Resource.Load(m_WaypointPrefab), GetGame().GetWorld(), params);
		AIWaypoint wp = AIWaypoint.Cast(wpEnt);

		if (wp)
		{
			wp.SetCompletionRadius(10.0);
			group.AddWaypoint(wp);
		}
	}

	// ========================================================================
	// Cleanup
	// ========================================================================

	protected void CleanupVehicles()
	{
		array<int> toRemove = {};

		for (int i = 0; i < m_aActiveVehicles.Count(); i++)
		{
			Vehicle veh = m_aActiveVehicles[i];

			// Null check (already deleted)
			if (!veh)
			{
				toRemove.Insert(i);
				continue;
			}

			// Destroyed check
			DamageManagerComponent damage = DamageManagerComponent.Cast(veh.FindComponent(DamageManagerComponent));
			if (damage && damage.GetState() == EDamageState.DESTROYED)
			{
				m_mVehicleDestinations.Remove(veh);
				m_aAbandonedVehicles.RemoveItem(veh);
				SCR_EntityHelper.DeleteEntityAndChildren(veh);
				toRemove.Insert(i);
				continue;
			}

			// Abandoned (stuck recovery failed)
			if (m_aAbandonedVehicles.Contains(veh))
			{
				m_mVehicleDestinations.Remove(veh);
				m_aAbandonedVehicles.RemoveItem(veh);
				SCR_EntityHelper.DeleteEntityAndChildren(veh);
				toRemove.Insert(i);
				continue;
			}

			// Distance check - despawn if far from all players
			if (!IsPlayerNearby(veh.GetOrigin(), m_fPlayerSafeRadius))
			{
				if (!IsWithinRange(veh.GetOrigin()))
				{
					m_mVehicleDestinations.Remove(veh);
					SCR_EntityHelper.DeleteEntityAndChildren(veh);
					toRemove.Insert(i);
				}
			}
		}

		// Remove from array in reverse order
		for (int i = toRemove.Count() - 1; i >= 0; i--)
			m_aActiveVehicles.Remove(toRemove[i]);
	}

	protected bool IsPlayerNearby(vector pos, float radius)
	{
		array<int> playerIds = {};
		GetGame().GetPlayerManager().GetPlayers(playerIds);

		foreach (int playerId : playerIds)
		{
			IEntity player = GetGame().GetPlayerManager().GetPlayerControlledEntity(playerId);
			if (!player) continue;

			if (vector.Distance(pos, player.GetOrigin()) < radius)
				return true;
		}

		return false;
	}

	protected bool IsWithinRange(vector pos)
	{
		array<int> playerIds = {};
		GetGame().GetPlayerManager().GetPlayers(playerIds);

		foreach (int playerId : playerIds)
		{
			IEntity player = GetGame().GetPlayerManager().GetPlayerControlledEntity(playerId);
			if (!player) continue;

			if (vector.Distance(pos, player.GetOrigin()) < m_fDespawnDistance)
				return true;
		}

		return false;
	}

	protected void OnVehicleAbandoned(Vehicle vehicle)
	{
		if (!vehicle) return;
		if (!m_aActiveVehicles.Contains(vehicle)) return;
		if (m_aAbandonedVehicles.Contains(vehicle)) return;

		m_aAbandonedVehicles.Insert(vehicle);
	}

	// ========================================================================
	// Debug Visualization
	// ========================================================================

	protected void UpdateDebugVisualization()
	{
		m_aDebugShapes.Clear();

		foreach (Vehicle veh, vector dest : m_mVehicleDestinations)
		{
			if (!veh) continue;

			vector points[2];
			points[0] = veh.GetOrigin();
			points[1] = dest;

			m_aDebugShapes.Insert(Shape.CreateLines(Color.CYAN, ShapeFlags.NOZBUFFER | ShapeFlags.TRANSP, points, 2));
		}
	}

	// ========================================================================
	// Public API
	// ========================================================================

	//! Get count of active vehicles
	int GetActiveVehicleCount()
	{
		return m_aActiveVehicles.Count();
	}

	//! Get all active vehicles
	void GetActiveVehicles(out array<Vehicle> vehicles)
	{
		vehicles = m_aActiveVehicles;
	}

	//! Enable or disable spawning at runtime
	void SetEnabled(bool enabled)
	{
		m_bEnabled = enabled;
		if (!enabled)
		{
			// Stop the loop
			GetGame().GetCallqueue().Remove(UpdateLoop);
		}
		else if (m_bInitialized)
		{
			// Restart the loop
			GetGame().GetCallqueue().CallLater(UpdateLoop, 1000, true);
		}
	}

	//! Set maximum vehicles at runtime
	void SetMaxVehicles(int max)
	{
		m_iMaxVehicles = Math.Max(1, max);
	}

	// ========================================================================
	// Cleanup on Delete
	// ========================================================================

	override void OnDelete(IEntity owner)
	{
		GetGame().GetCallqueue().Remove(UpdateLoop);
		GetGame().GetCallqueue().Remove(Initialize);

		if (m_bUseBehaviorTree)
			SCR_TrafficEvents.OnVehicleAbandoned.Remove(OnVehicleAbandoned);
	}
}
