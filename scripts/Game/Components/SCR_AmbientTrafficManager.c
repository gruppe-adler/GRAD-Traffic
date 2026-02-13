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

// --- The Main Header ---
[ComponentEditorProps(category: "Traffic System", description: "Mission Header with Traffic Control")]
class GRAD_TRAFFIC_MissionHeader : SCR_MissionHeader
{
    // Headline Variable (Top level category)
    [Attribute("0", desc: "Display lines and markers for debugging?", category: "TRAFFIC SYSTEM: MASTER CONTROL")]
    bool m_bShowDebugMarkers;

    // Nested ACE-style categories
    [Attribute()]
    ref GRAD_TRAFFIC_TrafficSpawnSettings m_SpawnSettings;

    [Attribute()]
    ref GRAD_TRAFFIC_TrafficLimitSettings m_LimitSettings;
}

class SCR_TrafficEvents
{
    // Global hook: (Position, "gunfight" or "killed")
    static ref ScriptInvoker<vector, string> OnCivilianEvent = new ScriptInvoker<vector, string>();
}

class SCR_AmbientTrafficManager
{
    protected static ref SCR_AmbientTrafficManager s_Instance;
    
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
    const float LOS_CHECK_INTERVAL = 3.0; // Check LOS every 3 seconds per vehicle
    const float TRAFFIC_VISIBILITY_CHECK_HEIGHT = 1.5; // Eye level offset

    // ------------------------------------------------------------------------------------------------
    // Singleton & Auto-Init
    // ------------------------------------------------------------------------------------------------
    static SCR_AmbientTrafficManager GetInstance()
    {
        if (!s_Instance)
            s_Instance = new SCR_AmbientTrafficManager();
        return s_Instance;
    }
    
    void SCR_AmbientTrafficManager()
    {
        // Auto-hook into game start
        if (GetGame())
        {
            GetGame().GetCallqueue().CallLater(Initialize, 1000, false);
        }
    }

    // ------------------------------------------------------------------------------------------------
    // 1. Initialization
    // ------------------------------------------------------------------------------------------------
    protected void Initialize()
	{
	    if (!Replication.IsServer()) return;
	
	    // Default settings (used if no mission header found)
	    string factionToUse = "CIV";
	    bool shouldEnable = true;
	    bool useCatalog = false;
	    
	    // The class already has these defaults from member variables:
	    // m_iMaxVehicles = 10;
	    // m_fDespawnDistance = 2000;
	    // m_fPlayerSafeRadius = 400.0;
	    // m_aVehicleOptions already contains the default S1203 prefab
	
	    // Try to load settings from mission header (optional)
	    GRAD_TRAFFIC_MissionHeader header = GRAD_TRAFFIC_MissionHeader.Cast(GetGame().GetMissionHeader());
	    if (header && header.m_SpawnSettings && header.m_LimitSettings)
	    {
	        Print("[TRAFFIC] Loading settings from mission header", LogLevel.NORMAL);
	        
	        shouldEnable         = header.m_SpawnSettings.m_bEnableTraffic;
	        m_iMaxVehicles       = header.m_LimitSettings.m_iMaxTrafficCount;
	        m_fDespawnDistance   = header.m_LimitSettings.m_fTrafficSpawnRange;
	        m_fPlayerSafeRadius  = header.m_LimitSettings.m_fPlayerSafeRadius;
	        factionToUse         = header.m_SpawnSettings.m_sTargetFaction;
	        useCatalog           = header.m_SpawnSettings.m_bUseCatalog;
	
	        if (!shouldEnable)
	        {
	            Print("[TRAFFIC] System disabled via Mission Header.", LogLevel.NORMAL);
	            return; 
	        }
	    }
	    else
	    {
	        Print("[TRAFFIC] No mission header found - using built-in defaults", LogLevel.NORMAL);
	    }
	    
	    // Optionally load vehicles from catalog
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
	    BaseGameMode gameMode = GetGame().GetGameMode();
	    if (!gameMode) return;
	
	    SCR_EntityCatalogManagerComponent catalogManager = SCR_EntityCatalogManagerComponent.Cast(gameMode.FindComponent(SCR_EntityCatalogManagerComponent));
	    if (!catalogManager) return;
	
	    // Get the vehicle catalog
	    SCR_EntityCatalog catalog = catalogManager.GetEntityCatalogOfType(EEntityCatalogType.VEHICLE);
	    if (!catalog) return;
	
	    array<SCR_EntityCatalogEntry> entries;
		catalog.GetEntityList(entries);
	    if (!entries) return;
	
	    foreach (SCR_EntityCatalogEntry entry : entries)
	    {
	        ResourceName prefab = entry.GetPrefab();
	        if (prefab == "") continue;
	
	        // --- THE ERROR-PROOF CHECK ---
	        // Instead of SCR_EntityCatalogFactionData, we look at the Prefab Data directly
	        Resource resource = Resource.Load(prefab);
	        if (!resource || !resource.IsValid()) continue;
	
	        IEntitySource source = resource.GetResource().ToEntitySource();
	        
	        // Find the FactionAffiliationComponent in the prefab's components
	        for (int i = 0; i < source.GetComponentCount(); i++)
	        {
	            IEntityComponentSource compSource = source.GetComponent(i);
	            if (compSource.GetClassName().Contains("FactionAffiliationComponent"))
	            {
	                string prefabFaction;
	                compSource.Get("m_sFactionKey", prefabFaction);
	                
	                if (prefabFaction == targetFactionKey)
	                {
	                    outPrefabs.Insert(prefab);
	                    break; 
	                }
	            }
	        }
	    }
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
		    // Civilians MUST have a faction to navigate road networks properly
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
    				agent.PreventMaxLOD(); // Keeps the brain active
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
		{
		    utility.SetCombatMode(EAIGroupCombatMode.HOLD_FIRE); // Civilians should be passive
		}

        // 5. Seat Driver
        if (MoveDriverInVehicle(vehicle, drvEnt))
        {
             Print("[TRAFFIC DEBUG] Driver seated in Pilot seat successfully.", LogLevel.NORMAL);
        }
        else
        {
             Print("[TRAFFIC ERROR] Failed to seat driver! Check CompartmentAccessComponent.", LogLevel.ERROR);
        }
		
		// lets ai check its not walking but in a vehicle now
		if (aiControl) {
			
			AIAgent agent = aiControl.GetControlAIAgent();
		    if (agent)
		    {
		        // This is the most "violent" way to wake up an agent
		        agent.DeactivateAI();
		        agent.ActivateAI(); 
		    }
		}
		
		ForceVehicleStart(vehicle);

        // 6. Assign Waypoint
        // We pass the Group ID to the callqueue so we can verify it still exists later
        GetGame().GetCallqueue().CallLater(DelayedWaypointAssign, 2000, false, group, destPos);

        Print(string.Format("[TRAFFIC] Spawned %1 at %2 (Heading to %3)", vehicle.GetName(), spawnPos, destPos), LogLevel.NORMAL);
    }
	
	protected void UpdateDebugLines()
    {
        m_aDebugShapes.Clear(); // Wipe old lines every frame/loop

        foreach (Vehicle veh, vector dest : m_mVehicleDestinations)
        {
            if (!veh) continue;

            vector points[2];
            points[0] = veh.GetOrigin(); // Start is now dynamic current position
            points[1] = dest;

            m_aDebugShapes.Insert(Shape.CreateLines(Color.CYAN, ShapeFlags.NOZBUFFER | ShapeFlags.TRANSP, points, 2));
        }
    }

    // Helper to ensure AI is ready before receiving orders
    // Helper to ensure AI is ready before receiving orders
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
		
		// DEBUG: Check if the group actually accepted it
	    AIWaypoint currentWp = group.GetCurrentWaypoint();
	    if (currentWp)
	    {
	        Print(string.Format("[TRAFFIC] Group %1 has waypoint %2", group, currentWp), LogLevel.NORMAL);
	    }
	    else
	    {
	        Print(string.Format("[TRAFFIC ERROR] Group %1 has NO waypoint after assignment!", group), LogLevel.ERROR);
	    }
	
	    // DEBUG: Check if the AI perceives the vehicle
	    array<AIAgent> agents = {};
	    group.GetAgents(agents);
	    foreach (AIAgent agent : agents)
	    {
	        SCR_ChimeraCharacter char = SCR_ChimeraCharacter.Cast(agent.GetControlledEntity());
	        if (char && char.IsInVehicle())
	        {
	             Print(string.Format("[TRAFFIC] Agent is physically inside vehicle."), LogLevel.NORMAL);
	        }
	        else
	        {
	             Print(string.Format("[TRAFFIC ERROR] Agent is NOT in vehicle physics!"), LogLevel.ERROR);
	        }
	    }
    }
	
	// Add this helper function
	void ForceVehicleStart(Vehicle vehicle)
	{
	    // Get the controller (handles engine, brakes, gears)
	    CarControllerComponent carController = CarControllerComponent.Cast(vehicle.FindComponent(CarControllerComponent));
	    
	    if (carController)
	    {
	        // Force engine start
	        carController.StartEngine();
	        
	        // Release handbrake so vehicle can drive normally
	        carController.SetPersistentHandBrake(false);
	        
	        // Optional: Force into first gear/drive if needed, though automatic usually handles this
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
			// We use a Line (easier to see sometimes) or a Cylinder for the destination
			vector points[2];
			points[0] = group.GetOrigin();
			points[1] = reachablePos;
			
			// Use NOZBUFFER so it shows through the ground
			// Use ShapeFlags.VISIBLE to ensure it's rendered
			Shape debugLine = Shape.CreateLines(Color.RED, ShapeFlags.NOZBUFFER | ShapeFlags.TRANSP, points, 2);
			
			// Store it. IMPORTANT: If this array is cleared anywhere else, the arrow disappears immediately.
			m_aDebugShapes.Insert(debugLine); 
			
			// Optional: Print to console to confirm the code actually reached this line
			Print("DEBUG: Spawned shape at " + reachablePos.ToString(), LogLevel.NORMAL);
			#endif
	
	        // --- NEW: DEBUG & GAME MASTER LOGIC ---
	        GRAD_TRAFFIC_MissionHeader header = GRAD_TRAFFIC_MissionHeader.Cast(GetGame().GetMissionHeader());
	        
	        // 1. Map Debug Markers (Visual lines/icons on map)
	        if (header && header.m_bShowDebugMarkers)
	        {
	            // This adds a temporary line on the map from start to finish
	            SCR_MapEntity mapEnt = SCR_MapEntity.GetMapInstance();
	            if (mapEnt)
	            {
	                // Note: Real-time map drawing usually requires a MapDescriptorComponent 
	                // on the waypoint prefab itself for permanent visibility.
	                Print(string.Format("[TRAFFIC DEBUG] Path: %1 -> %2", group.GetOrigin(), reachablePos), LogLevel.NORMAL);
	            }
	        }
	
	        // 1. Game Master Visibility
	        SCR_EditableEntityComponent editable = SCR_EditableEntityComponent.Cast(wpEnt.FindComponent(SCR_EditableEntityComponent));
	        if (editable)
	        {
	            // According to your source, SetParentEntity(null) triggers Register() 
	            // and places it in the root of the Editor hierarchy.
	            editable.SetParentEntity(null);
	        }
			
			SCR_EditableEntityComponent groupEditable = SCR_EditableEntityComponent.Cast(group.FindComponent(SCR_EditableEntityComponent));
			SCR_EditableEntityComponent wpEditable = SCR_EditableEntityComponent.Cast(wpEnt.FindComponent(SCR_EditableEntityComponent));
			
			if (groupEditable && wpEditable)
			{
			    // This makes the Waypoint a 'child' of the AI Group in the GM hierarchy.
			    // Zeus will now see the line connecting the group to this waypoint.
			    wpEditable.SetParentEntity(groupEditable);
			}
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

            // Condition: Destroyed
            DamageManagerComponent damage = DamageManagerComponent.Cast(veh.FindComponent(DamageManagerComponent));
            if (damage && damage.GetState() == EDamageState.DESTROYED)
            {
                CleanupVehicle(veh);
                indicesToDelete.Insert(i);
                continue;
            }

            // Condition: Proximity & Distance Check
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

            // Despawn if no players are nearby AND it's far from ALL players
            if (!isPlayerNearby)
            {
                // Check minimum distance to any player
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

                // Only despawn if:
                // 1. Beyond despawn distance AND
                // 2. Not visible to any player (LOS check)
                if (minPlayerDist > m_fDespawnDistance)
                {
                    // Check line of sight before despawning
                    if (IsVehicleVisibleToAnyPlayer(veh))
                    {
                        Print(string.Format("[TRAFFIC] Vehicle beyond despawn range but visible, keeping (distance: %.0fm)", minPlayerDist), LogLevel.DEBUG);
                        continue; // Don't despawn if visible
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
	
	    for(int i = 0; i < 15; i++) 
	    {
	        // 1. Pick a random start
	        vector sPos = GetRandomMapPos();
	        BaseRoad r1;
	        float dist1;
	        if (roadMgr.GetClosestRoad(sPos, r1, dist1) == -1) continue;
	
	        array<vector> p1 = {};
	        r1.GetPoints(p1);
	        spawn = p1[0];
	        
	        // 2. Pick a random destination
	        vector dPos = GetRandomMapPos();
	        if (vector.Distance(spawn, dPos) < 2000) continue;
	
	        // 3. THE REACHABILITY TEST
	        // We ask the manager: "Find a spot on a road near dPos reachable from spawn"
	        vector validDestPos;
	        float searchRadius = 500.0; // How far from dPos we're willing to look for a road
	        
	        if (roadMgr.GetReachableWaypointInRoad(spawn, dPos, searchRadius, validDestPos))
	        {
	            dest = validDestPos;
	            return true; // Connection confirmed!
	        }
	        
	        Print(string.Format("[TRAFFIC] Road at %1 is not reachable from %2 (Water or Gap). Retrying...", dPos, spawn), LogLevel.DEBUG);
	    }
	    return false;
	}

    // Optimized Helper
    protected BaseRoad GetNearestRoad(vector center, float radius)
    {
        SCR_AIWorld aiWorld = SCR_AIWorld.Cast(GetGame().GetAIWorld());
        if (!aiWorld) return null;
        
        RoadNetworkManager roadMgr = aiWorld.GetRoadNetworkManager();
        if (!roadMgr) return null;

        array<BaseRoad> roads = {};
        // OPTIMIZATION: Reduced radius from 500 to 50
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
	    // CHANGE THIS LINE:
	    SCR_TrafficEvents.OnCivilianEvent.Invoke(owner.GetOrigin(), "gunfight");
	}

    // Helper to properly cleanup a vehicle and its tracking data
    protected void CleanupVehicle(Vehicle veh)
    {
        if (!veh) return;

        // Remove from all tracking structures
        m_mVehicleDestinations.Remove(veh);
        m_mLastLOSCheck.Remove(veh);

        // Delete the entity
        SCR_EntityHelper.DeleteEntityAndChildren(veh);

        Print(string.Format("[TRAFFIC] Cleaned up vehicle %1", veh), LogLevel.DEBUG);
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

    // Performs a line of sight trace between two points
    protected bool HasLineOfSight(vector from, vector to)
    {
        autoptr TraceParam trace = new TraceParam();
        trace.Start = from;
        trace.End = to;
        trace.Flags = TraceFlags.WORLD | TraceFlags.ENTS; // Check world geometry and entities
        trace.LayerMask = EPhysicsLayerPresets.Projectile; // Use projectile layer (blocks on terrain, buildings)

        float hitDist = GetGame().GetWorld().TraceMove(trace, null);

        // If hitDist is 1.0, trace reached destination without hitting anything
        return hitDist >= 1.0;
    }

    // Checks if any player has line of sight to the vehicle
    protected bool IsVehicleVisibleToAnyPlayer(Vehicle veh)
    {
        if (!veh) return false;

        float currentTime = GetGame().GetWorld().GetWorldTime() / 1000.0; // Convert to seconds

        // Check if we've checked this vehicle recently (optimization)
        if (m_mLastLOSCheck.Contains(veh))
        {
            float lastCheck = m_mLastLOSCheck[veh];
            if (currentTime - lastCheck < LOS_CHECK_INTERVAL)
                return true; // Assume visible to be safe (don't despawn yet)
        }

        // Update check time
        m_mLastLOSCheck.Set(veh, currentTime);

        vector vehPos = veh.GetOrigin();
        vehPos[1] = vehPos[1] + 2.0; // Check center of vehicle, slightly elevated

        array<int> playerIds = {};
        GetGame().GetPlayerManager().GetPlayers(playerIds);

        foreach (int playerId : playerIds)
        {
            IEntity player = GetGame().GetPlayerManager().GetPlayerControlledEntity(playerId);
            if (!player) continue;

            vector playerEyePos = player.GetOrigin();
            playerEyePos[1] = playerEyePos[1] + TRAFFIC_VISIBILITY_CHECK_HEIGHT;

            // Quick distance check first (optimization)
            float dist = vector.Distance(vehPos, playerEyePos);
            if (dist > m_fDespawnDistance) continue; // Too far to matter

            // Check if within view frustum (cheap check before raycast)
            vector playerAngles = player.GetAngles();
            vector playerDir = playerAngles.AnglesToVector();
            vector toVehicle = vehPos - playerEyePos;
            toVehicle.Normalize();

            float dotProduct = vector.Dot(playerDir, toVehicle);
            float viewAngle = Math.Acos(dotProduct) * Math.RAD2DEG;

            // Only trace if within ~110 degree FOV (peripheral vision)
            if (viewAngle > 110) continue;

            // Perform line of sight trace
            if (HasLineOfSight(playerEyePos, vehPos))
            {
                Print(string.Format("[TRAFFIC DEBUG] Vehicle visible to player %1", playerId), LogLevel.DEBUG);
                return true; // Player can see this vehicle
            }
        }

        return false; // No player has LOS
    }
}

// ------------------------------------------------------------------------------------------------
// Auto-Start Hook
// ------------------------------------------------------------------------------------------------
modded class SCR_PlayerController
{
    protected bool m_bTrafficInitialized = false;
    
    override void OnControlledEntityChanged(IEntity from, IEntity to)
    {
        super.OnControlledEntityChanged(from, to);
        
        // Initialize traffic when first player spawns (only once, only on server)
        if (!m_bTrafficInitialized && Replication.IsServer() && to)
        {
            m_bTrafficInitialized = true;
            GetGame().GetCallqueue().CallLater(InitTraffic, 2000, false);
        }
    }
    
    protected void InitTraffic()
    {
        SCR_AmbientTrafficManager.GetInstance();
        Print("[TRAFFIC] Auto-initialized via player controller", LogLevel.NORMAL);
    }
}