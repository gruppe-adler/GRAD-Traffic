// --- Nested Group: Spawn Settings ---
[BaseContainerProps()]
class GRAD_TRAFFIC_TrafficSpawnSettings
{
    [Attribute("1", desc: "Global toggle for the traffic system.")]
    bool m_bEnableTraffic;

    [Attribute("CIV", desc: "Faction key (e.g. CIV, US).")]
    string m_sTargetFaction;

    [Attribute("1", desc: "Pull vehicles from the Faction Catalog?")]
    bool m_bUseCatalog;
}

// --- Nested Group: Performance & Limits ---
[BaseContainerProps()]
class GRAD_TRAFFIC_TrafficLimitSettings
{
    [Attribute("10", desc: "Max vehicles on map.")]
    int m_iMaxTrafficCount;

    [Attribute("2000", desc: "Outer despawn range.")]
    float m_fTrafficSpawnRange;

    [Attribute("400", desc: "Safe zone radius around players.")]
    float m_fPlayerSafeRadius;
}

// --- Mod the base mission header (ACE Anvil style) ---
modded class SCR_MissionHeader
{
    [Attribute(desc: "Traffic spawn settings")]
    ref GRAD_TRAFFIC_TrafficSpawnSettings m_TrafficSpawnSettings;

    [Attribute(desc: "Traffic limit settings")]
    ref GRAD_TRAFFIC_TrafficLimitSettings m_TrafficLimitSettings;

    [Attribute("0", desc: "Display lines and markers for debugging?")]
    bool m_bShowDebugMarkers;
}

class SCR_TrafficEvents
{
    // Global hook: (Position, "gunfight" or "killed")
    static ref ScriptInvoker<vector, string> OnCivilianEvent = new ScriptInvoker<vector, string>();

    // Backwards-compatible hooks for traffic vehicle lifecycle events
    static ref ScriptInvoker<IEntity> OnTrafficVehicleSpawned = new ScriptInvoker<IEntity>();
    static ref ScriptInvoker<IEntity> OnTrafficVehicleDespawned = new ScriptInvoker<IEntity>();
}

class SCR_AmbientTrafficManager
{
    // --- Configuration ---
    protected int m_iMaxVehicles = 10;
    protected float m_fDespawnDistance = 2000;
    protected float m_fPlayerSafeRadius = 400.0;

    // PREFABS - Default values that work out of the box
    protected ref array<ResourceName> m_aVehicleOptions = {
        "{D2BCF98E80CF634C}Prefabs/Vehicles/Wheeled/S1203/S1203_cargo_beige.et",
        "{6AF3A89263D26CD8}Prefabs/Vehicles/Wheeled/S1203/S1203_cargo_blue.et",
        "{2A66E3B8B0C61D87}Prefabs/Vehicles/Wheeled/S1203/S1203_cargo_brown.et",
        "{6E485048122CEEEE}Prefabs/Vehicles/Wheeled/S1203/S1203_cargo_red.et",
        "{E5F73B9D4CEB94E4}Prefabs/Vehicles/Wheeled/S1203/S1203_cargo_white.et",
        "{E3AD3E9E60F2E061}Prefabs/Vehicles/Wheeled/S1203/S1203_cargo_yellow.et"
    };

    protected ResourceName m_DriverPrefab = "{22E43956740A6794}Prefabs/Characters/Factions/CIV/GenericCivilians/Character_CIV_Randomized.et";
    protected ResourceName m_WaypointPrefab = "{750A8D1695BD6998}Prefabs/AI/Waypoints/AIWaypoint_Move.et";
    protected ResourceName m_GroupPrefab = "{000CD338713F2B5A}Prefabs/Groups/Group_Base.et";

    // Tracking
    protected ref array<Vehicle> m_aActiveVehicles = {};
    protected ref map<Vehicle, vector> m_mVehicleDestinations = new map<Vehicle, vector>();
    protected ref array<ref Shape> m_aDebugShapes = {};

    // Line of Sight tracking for despawn prevention
    protected ref map<Vehicle, float> m_mLastLOSCheck = new map<Vehicle, float>();
    const float LOS_CHECK_INTERVAL = 3.0;
    const float TRAFFIC_VISIBILITY_CHECK_HEIGHT = 1.5;
    const float MIN_VEHICLE_SPACING = 200.0;

    // ------------------------------------------------------------------------------------------------
    // 1. Initialization — called directly by SCR_PlayerController after game is ready
    // ------------------------------------------------------------------------------------------------
    void Initialize()
    {
        // Default settings
        string factionToUse = "CIV";
        bool shouldEnable = true;
        bool useCatalog = false;

        // Try to load settings from mission header
        SCR_MissionHeader header = SCR_MissionHeader.Cast(GetGame().GetMissionHeader());
        if (header && header.m_TrafficSpawnSettings && header.m_TrafficLimitSettings)
        {
            Print("[TRAFFIC] Loading settings from mission header", LogLevel.NORMAL);

            shouldEnable        = header.m_TrafficSpawnSettings.m_bEnableTraffic;
            m_iMaxVehicles      = header.m_TrafficLimitSettings.m_iMaxTrafficCount;
            m_fDespawnDistance  = header.m_TrafficLimitSettings.m_fTrafficSpawnRange;
            m_fPlayerSafeRadius = header.m_TrafficLimitSettings.m_fPlayerSafeRadius;
            factionToUse        = header.m_TrafficSpawnSettings.m_sTargetFaction;
            useCatalog          = header.m_TrafficSpawnSettings.m_bUseCatalog;

            if (!shouldEnable)
            {
                Print("[TRAFFIC] System disabled via Mission Header.", LogLevel.NORMAL);
                return;
            }
        }
        else
        {
            Print("[TRAFFIC] No mission header traffic settings found - using built-in defaults", LogLevel.NORMAL);
        }

        if (useCatalog)
        {
            m_aVehicleOptions.Clear();
            GetVehiclesFromCatalog(factionToUse, m_aVehicleOptions);

            if (m_aVehicleOptions.IsEmpty())
            {
                Print("[TRAFFIC] Catalog returned no vehicles, reverting to hardcoded default", LogLevel.WARNING);
                m_aVehicleOptions.Insert("{D2BCF98E80CF634C}Prefabs/Vehicles/Wheeled/S1203/S1203_cargo_beige.et");
            }
        }

        Print(string.Format("[TRAFFIC] Initialized! %1 vehicle types | Faction: %2 | Max vehicles: %3",
            m_aVehicleOptions.Count(), factionToUse, m_iMaxVehicles), LogLevel.NORMAL);

        GetGame().GetCallqueue().CallLater(UpdateTrafficLoop, 1000, true);
    }

    protected void GetVehiclesFromCatalog(string targetFactionKey, out array<ResourceName> outPrefabs)
    {
        Print(string.Format("[TRAFFIC] Loading vehicles from faction catalog for '%1'...", targetFactionKey), LogLevel.NORMAL);

        FactionManager factionMgr = GetGame().GetFactionManager();
        if (!factionMgr) { Print("[TRAFFIC] CATALOG FAIL: No FactionManager.", LogLevel.WARNING); return; }

        SCR_Faction faction = SCR_Faction.Cast(factionMgr.GetFactionByKey(targetFactionKey));
        if (!faction) { Print(string.Format("[TRAFFIC] CATALOG FAIL: Faction '%1' not found.", targetFactionKey), LogLevel.WARNING); return; }

        SCR_EntityCatalog catalog = faction.GetFactionEntityCatalogOfType(EEntityCatalogType.VEHICLE);
        if (!catalog) { Print(string.Format("[TRAFFIC] CATALOG FAIL: No VEHICLE catalog for '%1'.", targetFactionKey), LogLevel.WARNING); return; }

        array<SCR_EntityCatalogEntry> entries = {};
        catalog.GetEntityList(entries);

        foreach (SCR_EntityCatalogEntry entry : entries)
        {
            if (!entry.IsEnabled()) continue;

            ResourceName prefab = entry.GetPrefab();
            if (prefab.IsEmpty()) continue;

            // Skip air assets — traffic is land-only
            string prefabStr = prefab;
            prefabStr.ToLower();
            if (prefabStr.Contains("helicopter") || prefabStr.Contains("plane") || prefabStr.Contains("/air/")) continue;

            Resource res = Resource.Load(prefab);
            if (!res || !res.IsValid()) continue;

            outPrefabs.Insert(prefab);
        }

        Print(string.Format("[TRAFFIC] Catalog loaded %1 prefabs from '%2'.", outPrefabs.Count(), targetFactionKey), LogLevel.NORMAL);
    }

    // ------------------------------------------------------------------------------------------------
    // 2. The Main Loop
    // ------------------------------------------------------------------------------------------------

    protected void UpdateTrafficLoop()
    {
        CleanupTraffic();

        if (m_aActiveVehicles.Count() < m_iMaxVehicles)
            SpawnSingleTrafficUnit();

        #ifdef WORKBENCH
        UpdateDebugLines();
        #endif
    }

    protected void SpawnSingleTrafficUnit()
    {
        if (m_aVehicleOptions.IsEmpty())
        {
            Print("[TRAFFIC ERROR] No vehicle prefabs in the list!", LogLevel.ERROR);
            return;
        }

        vector spawnPos, destPos;
        if (!FindValidRoadPoints(spawnPos, destPos))
        {
            Print("[TRAFFIC DEBUG] Failed to find road points. Retrying next loop.", LogLevel.WARNING);
            return;
        }

        EntitySpawnParams params = new EntitySpawnParams();
        params.TransformMode = ETransformMode.WORLD;

        vector forward = vector.Direction(spawnPos, destPos);
        if (forward.LengthSq() < 0.0001)
            forward = "0 0 1";

        vector up = "0 1 0";
        Math3D.DirectionAndUpMatrix(forward, up, params.Transform);
        params.Transform[3] = spawnPos;

        // 1. Spawn Group
        IEntity groupEnt = GetGame().SpawnEntityPrefab(Resource.Load(m_GroupPrefab), GetGame().GetWorld(), params);
        SCR_AIGroup group = SCR_AIGroup.Cast(groupEnt);
        if (!group)
        {
            Print("[TRAFFIC ERROR] Failed to spawn AIGroup!", LogLevel.ERROR);
            return;
        }

        FactionManager factionMgr = GetGame().GetFactionManager();
        if (factionMgr)
        {
            Faction civFaction = factionMgr.GetFactionByKey("CIV");
            if (civFaction)
                group.SetFaction(civFaction);
            else
                Print("[TRAFFIC ERROR] CIV Faction not found in FactionManager!", LogLevel.ERROR);
        }

        // 2. Spawn Vehicle
        ResourceName randomCarPath = m_aVehicleOptions.GetRandomElement();
        IEntity vehEnt = GetGame().SpawnEntityPrefab(Resource.Load(randomCarPath), GetGame().GetWorld(), params);
        Vehicle vehicle = Vehicle.Cast(vehEnt);
        if (!vehicle)
        {
            Print("[TRAFFIC ERROR] Failed to spawn Vehicle entity!", LogLevel.ERROR);
            SCR_EntityHelper.DeleteEntityAndChildren(group);
            return;
        }

        m_aActiveVehicles.Insert(vehicle);
        m_mVehicleDestinations.Insert(vehicle, destPos);

        SCR_TrafficEvents.OnTrafficVehicleSpawned.Invoke(vehicle);

        // 3. Spawn Driver
        IEntity drvEnt = GetGame().SpawnEntityPrefab(Resource.Load(m_DriverPrefab), GetGame().GetWorld(), params);
        if (!drvEnt)
        {
            Print("[TRAFFIC ERROR] Failed to spawn Driver entity!", LogLevel.ERROR);
            SCR_EntityHelper.DeleteEntityAndChildren(group);
            SCR_EntityHelper.DeleteEntityAndChildren(vehicle);
            return;
        }

        // 4. Link Agent to Group
        AIControlComponent aiControl = AIControlComponent.Cast(drvEnt.FindComponent(AIControlComponent));
        if (aiControl)
        {
            AIAgent agent = aiControl.GetControlAIAgent();
            if (agent)
            {
                agent.PreventMaxLOD();
                group.AddAgent(agent);
                Print(string.Format("[TRAFFIC DEBUG] Agent %1 added to Group %2", agent, group), LogLevel.NORMAL);
            }
            else
            {
                Print("[TRAFFIC ERROR] Driver has AIControl but no AIAgent!", LogLevel.ERROR);
            }
        }
        else
        {
            Print("[TRAFFIC ERROR] Driver prefab missing AIControlComponent!", LogLevel.ERROR);
        }

        SCR_AIGroupUtilityComponent utility = SCR_AIGroupUtilityComponent.Cast(group.FindComponent(SCR_AIGroupUtilityComponent));
        if (utility)
            utility.SetCombatMode(EAIGroupCombatMode.HOLD_FIRE);

        // 5. Seat Driver
        if (MoveDriverInVehicle(vehicle, drvEnt))
            Print("[TRAFFIC DEBUG] Driver seated in Pilot seat successfully.", LogLevel.NORMAL);
        else
            Print("[TRAFFIC ERROR] Failed to seat driver! Check CompartmentAccessComponent.", LogLevel.ERROR);

        if (aiControl)
        {
            AIAgent agent = aiControl.GetControlAIAgent();
            if (agent)
            {
                agent.DeactivateAI();
                agent.ActivateAI();
            }
        }

        ForceVehicleStart(vehicle);

        // 6. Assign Waypoint
        GetGame().GetCallqueue().CallLater(DelayedWaypointAssign, 2000, false, group, destPos);

        Print(string.Format("[TRAFFIC] Spawned %1 at %2 (Heading to %3)", vehicle.GetName(), spawnPos, destPos), LogLevel.NORMAL);
    }

    protected void UpdateDebugLines()
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

    protected void DelayedWaypointAssign(SCR_AIGroup group, vector pos)
    {
        if (!group)
        {
            Print("[TRAFFIC DEBUG] DelayedWaypointAssign failed: Group is null (despawned?)", LogLevel.WARNING);
            return;
        }

        int agentCount = group.GetAgentsCount();
        if (agentCount == 0)
        {
            Print("[TRAFFIC DEBUG] DelayedWaypointAssign failed: Group is empty (no agents)!", LogLevel.WARNING);
            return;
        }

        Print(string.Format("[TRAFFIC DEBUG] Assigning waypoint to Group (Agents: %1) at Dest: %2", agentCount, pos), LogLevel.NORMAL);
        CreateWaypointForGroup(group, pos);

        AIWaypoint currentWp = group.GetCurrentWaypoint();
        if (currentWp)
            Print(string.Format("[TRAFFIC] Group %1 has waypoint %2", group, currentWp), LogLevel.NORMAL);
        else
            Print(string.Format("[TRAFFIC ERROR] Group %1 has NO waypoint after assignment!", group), LogLevel.ERROR);

        array<AIAgent> agents = {};
        group.GetAgents(agents);
        foreach (AIAgent agent : agents)
        {
            SCR_ChimeraCharacter char = SCR_ChimeraCharacter.Cast(agent.GetControlledEntity());
            if (char && char.IsInVehicle())
                Print("[TRAFFIC] Agent is physically inside vehicle.", LogLevel.NORMAL);
            else
                Print("[TRAFFIC ERROR] Agent is NOT in vehicle physics!", LogLevel.ERROR);
        }
    }

    void ForceVehicleStart(Vehicle vehicle)
    {
        CarControllerComponent carController = CarControllerComponent.Cast(vehicle.FindComponent(CarControllerComponent));
        if (carController)
        {
            carController.StartEngine();
            carController.SetPersistentHandBrake(false);
            Print(string.Format("[TRAFFIC] Hotwired vehicle %1", vehicle), LogLevel.NORMAL);
        }
    }

    protected void CreateWaypointForGroup(SCR_AIGroup group, vector destPos)
    {
        SCR_AIWorld aiWorld = SCR_AIWorld.Cast(GetGame().GetAIWorld());
        RoadNetworkManager roadMgr = aiWorld.GetRoadNetworkManager();

        vector reachablePos;
        float radius = 20.0;

        if (!roadMgr.GetReachableWaypointInRoad(group.GetOrigin(), destPos, radius, reachablePos))
            reachablePos = destPos;

        EntitySpawnParams params = new EntitySpawnParams();
        params.Transform[3] = reachablePos;

        IEntity wpEnt = GetGame().SpawnEntityPrefab(Resource.Load(m_WaypointPrefab), GetGame().GetWorld(), params);
        AIWaypoint wp = AIWaypoint.Cast(wpEnt);

        if (wp)
        {
            float distShift = vector.Distance(reachablePos, destPos);
            wp.SetCompletionRadius(Math.Max(5.0, radius - distShift));
            group.AddWaypoint(wp);

            #ifdef WORKBENCH
            vector points[2];
            points[0] = group.GetOrigin();
            points[1] = reachablePos;
            Shape debugLine = Shape.CreateLines(Color.RED, ShapeFlags.NOZBUFFER | ShapeFlags.TRANSP, points, 2);
            m_aDebugShapes.Insert(debugLine);
            Print("DEBUG: Spawned shape at " + reachablePos.ToString(), LogLevel.NORMAL);
            #endif

            SCR_MissionHeader header = SCR_MissionHeader.Cast(GetGame().GetMissionHeader());
            if (header && header.m_bShowDebugMarkers)
            {
                SCR_MapEntity mapEnt = SCR_MapEntity.GetMapInstance();
                if (mapEnt)
                    Print(string.Format("[TRAFFIC DEBUG] Path: %1 -> %2", group.GetOrigin(), reachablePos), LogLevel.NORMAL);
            }

            SCR_EditableEntityComponent editable = SCR_EditableEntityComponent.Cast(wpEnt.FindComponent(SCR_EditableEntityComponent));
            if (editable)
                editable.SetParentEntity(null);

            SCR_EditableEntityComponent groupEditable = SCR_EditableEntityComponent.Cast(group.FindComponent(SCR_EditableEntityComponent));
            SCR_EditableEntityComponent wpEditable = SCR_EditableEntityComponent.Cast(wpEnt.FindComponent(SCR_EditableEntityComponent));
            if (groupEditable && wpEditable)
                wpEditable.SetParentEntity(groupEditable);
        }
    }

    // ------------------------------------------------------------------------------------------------
    // 3. Cleanup Logic
    // ------------------------------------------------------------------------------------------------
    protected void CleanupTraffic()
    {
        array<int> indicesToDelete = {};
        array<int> playerIds = {};
        GetGame().GetPlayerManager().GetPlayers(playerIds);

        for (int i = 0; i < m_aActiveVehicles.Count(); i++)
        {
            Vehicle veh = m_aActiveVehicles[i];
            if (!veh) { indicesToDelete.Insert(i); continue; }

            DamageManagerComponent damage = DamageManagerComponent.Cast(veh.FindComponent(DamageManagerComponent));
            if (damage && damage.GetState() == EDamageState.DESTROYED)
            {
                CleanupVehicle(veh);
                indicesToDelete.Insert(i);
                continue;
            }

            vector vehPos = veh.GetOrigin();
            bool isPlayerNearby = false;

            foreach (int playerId : playerIds)
            {
                IEntity player = GetGame().GetPlayerManager().GetPlayerControlledEntity(playerId);
                if (!player) continue;

                if (vector.Distance(vehPos, player.GetOrigin()) < m_fPlayerSafeRadius)
                {
                    isPlayerNearby = true;
                    break;
                }
            }

            if (!isPlayerNearby)
            {
                float minPlayerDist = float.MAX;
                foreach (int pId : playerIds)
                {
                    IEntity player = GetGame().GetPlayerManager().GetPlayerControlledEntity(pId);
                    if (player)
                    {
                        float dist = vector.Distance(vehPos, player.GetOrigin());
                        if (dist < minPlayerDist) minPlayerDist = dist;
                    }
                }

                if (minPlayerDist > m_fDespawnDistance)
                {
                    if (IsVehicleVisibleToAnyPlayer(veh))
                    {
                        Print(string.Format("[TRAFFIC] Vehicle beyond despawn range but visible, keeping (distance: %.0fm)", minPlayerDist), LogLevel.DEBUG);
                        continue;
                    }

                    CleanupVehicle(veh);
                    indicesToDelete.Insert(i);
                    Print(string.Format("[TRAFFIC] Despawned vehicle (distance: %.0fm, not visible)", minPlayerDist), LogLevel.DEBUG);
                }
            }
        }

        for (int i = indicesToDelete.Count() - 1; i >= 0; i--)
            m_aActiveVehicles.Remove(indicesToDelete[i]);
    }

    // ------------------------------------------------------------------------------------------------
    // 4. Helpers
    // ------------------------------------------------------------------------------------------------
    protected bool FindValidRoadPoints(out vector spawn, out vector dest)
    {
        SCR_AIWorld aiWorld = SCR_AIWorld.Cast(GetGame().GetAIWorld());
        if (!aiWorld) return false;

        RoadNetworkManager roadMgr = aiWorld.GetRoadNetworkManager();
        if (!roadMgr) return false;

        array<int> playerIds = {};
        GetGame().GetPlayerManager().GetPlayers(playerIds);
        array<vector> playerPositions = {};
        foreach (int playerId : playerIds)
        {
            IEntity player = GetGame().GetPlayerManager().GetPlayerControlledEntity(playerId);
            if (player)
                playerPositions.Insert(player.GetOrigin());
        }

        for (int i = 0; i < 15; i++)
        {
            vector sPos = GetRandomMapPos();
            BaseRoad r1;
            float dist1;
            if (roadMgr.GetClosestRoad(sPos, r1, dist1) == -1) continue;

            array<vector> p1 = {};
            r1.GetPoints(p1);
            spawn = p1[0];

            bool tooCloseToPlayer = false;
            bool withinRangeOfAnyPlayer = false;

            foreach (vector playerPos : playerPositions)
            {
                float distToPlayer = vector.Distance(spawn, playerPos);
                if (distToPlayer < m_fPlayerSafeRadius)
                {
                    tooCloseToPlayer = true;
                    break;
                }
                if (!withinRangeOfAnyPlayer && distToPlayer < m_fDespawnDistance)
                    withinRangeOfAnyPlayer = true;
            }

            if (tooCloseToPlayer || !withinRangeOfAnyPlayer)
                continue;

            bool tooCloseToVehicle = false;
            foreach (Vehicle existingVeh : m_aActiveVehicles)
            {
                if (!existingVeh) continue;
                if (vector.Distance(spawn, existingVeh.GetOrigin()) < MIN_VEHICLE_SPACING)
                {
                    tooCloseToVehicle = true;
                    break;
                }
            }

            if (tooCloseToVehicle)
                continue;

            vector dPos = GetRandomMapPos();
            if (vector.Distance(spawn, dPos) < 2000) continue;

            vector validDestPos;
            float searchRadius = 500.0;

            if (roadMgr.GetReachableWaypointInRoad(spawn, dPos, searchRadius, validDestPos))
            {
                dest = validDestPos;
                return true;
            }

            Print(string.Format("[TRAFFIC] Road at %1 is not reachable from %2 (Water or Gap). Retrying...", dPos, spawn), LogLevel.DEBUG);
        }
        return false;
    }

    protected BaseRoad GetNearestRoad(vector center, float radius)
    {
        SCR_AIWorld aiWorld = SCR_AIWorld.Cast(GetGame().GetAIWorld());
        if (!aiWorld) return null;

        RoadNetworkManager roadMgr = aiWorld.GetRoadNetworkManager();
        if (!roadMgr) return null;

        array<BaseRoad> roads = {};
        roadMgr.GetRoadsInAABB(center - Vector(radius, radius, radius), center + Vector(radius, radius, radius), roads);

        if (roads.Count() == 0) return null;
        return roads.GetRandomElement();
    }

    protected bool MoveDriverInVehicle(Vehicle vehicle, IEntity driver)
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

    protected void OnDriverPanic(IEntity owner)
    {
        Print("[TRAFFIC EVENT] PANIC! Driver reacting.", LogLevel.WARNING);
        SCR_TrafficEvents.OnCivilianEvent.Invoke(owner.GetOrigin(), "gunfight");
    }

    protected void CleanupVehicle(Vehicle veh)
    {
        if (!veh) return;

        m_mVehicleDestinations.Remove(veh);
        m_mLastLOSCheck.Remove(veh);

        SCR_TrafficEvents.OnTrafficVehicleDespawned.Invoke(veh);

        string vehDesc = string.Format("%1", veh);
        SCR_EntityHelper.DeleteEntityAndChildren(veh);
        Print(string.Format("[TRAFFIC] Cleaned up vehicle %1", vehDesc), LogLevel.DEBUG);
    }

    protected vector GetRandomMapPos()
    {
        vector mapMin, mapMax;
        GetGame().GetWorldEntity().GetWorldBounds(mapMin, mapMax);
        return Vector(Math.RandomFloat(mapMin[0], mapMax[0]), 0, Math.RandomFloat(mapMin[2], mapMax[2]));
    }

    // ------------------------------------------------------------------------------------------------
    // 5. Line of Sight Helpers
    // ------------------------------------------------------------------------------------------------

    protected bool HasLineOfSight(vector from, vector to)
    {
        autoptr TraceParam trace = new TraceParam();
        trace.Start = from;
        trace.End = to;
        trace.Flags = TraceFlags.WORLD | TraceFlags.ENTS;
        trace.LayerMask = EPhysicsLayerPresets.Projectile;

        float hitDist = GetGame().GetWorld().TraceMove(trace, null);
        return hitDist >= 1.0;
    }

    protected bool IsVehicleVisibleToAnyPlayer(Vehicle veh)
    {
        if (!veh) return false;

        float currentTime = GetGame().GetWorld().GetWorldTime() / 1000.0;

        if (m_mLastLOSCheck.Contains(veh))
        {
            float lastCheck = m_mLastLOSCheck[veh];
            if (currentTime - lastCheck < LOS_CHECK_INTERVAL)
                return true;
        }

        m_mLastLOSCheck.Set(veh, currentTime);

        vector vehPos = veh.GetOrigin();
        vehPos[1] = vehPos[1] + 2.0;

        array<int> playerIds = {};
        GetGame().GetPlayerManager().GetPlayers(playerIds);

        foreach (int playerId : playerIds)
        {
            IEntity player = GetGame().GetPlayerManager().GetPlayerControlledEntity(playerId);
            if (!player) continue;

            vector playerEyePos = player.GetOrigin();
            playerEyePos[1] = playerEyePos[1] + TRAFFIC_VISIBILITY_CHECK_HEIGHT;

            float dist = vector.Distance(vehPos, playerEyePos);
            if (dist > m_fDespawnDistance) continue;

            vector playerAngles = player.GetAngles();
            vector playerDir = playerAngles.AnglesToVector();
            vector toVehicle = vehPos - playerEyePos;
            toVehicle.Normalize();

            float dotProduct = vector.Dot(playerDir, toVehicle);
            if (dotProduct > 1.0) dotProduct = 1.0;
            else if (dotProduct < -1.0) dotProduct = -1.0;
            float viewAngle = Math.Acos(dotProduct) * Math.RAD2DEG;

            if (viewAngle > 110) continue;

            if (HasLineOfSight(playerEyePos, vehPos))
            {
                Print(string.Format("[TRAFFIC DEBUG] Vehicle visible to player %1", playerId), LogLevel.DEBUG);
                return true;
            }
        }

        return false;
    }
}

// ------------------------------------------------------------------------------------------------
// Auto-Start Hook — owns the manager instance, no singleton needed
// ------------------------------------------------------------------------------------------------
modded class SCR_PlayerController
{
    protected ref SCR_AmbientTrafficManager m_TrafficManager;

    override void OnControlledEntityChanged(IEntity from, IEntity to)
    {
        super.OnControlledEntityChanged(from, to);

        // Initialize once when first entity is controlled, only on authority (server or local play)
        if (!m_TrafficManager && to && (Replication.IsServer() || !Replication.IsRunning()))
        {
            m_TrafficManager = new SCR_AmbientTrafficManager();
            GetGame().GetCallqueue().CallLater(m_TrafficManager.Initialize, 2000, false);
            Print("[TRAFFIC] Traffic manager created, initializing in 2s...", LogLevel.NORMAL);
        }
    }
}
