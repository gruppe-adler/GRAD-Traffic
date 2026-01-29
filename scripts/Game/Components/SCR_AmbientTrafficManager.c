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

    [Attribute("1", desc: "Use advanced behavior tree for civilians (enables stuck recovery and varied panic)")]
    bool m_bUseBehaviorTree;
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

    [Attribute()]
    ref GRAD_TRAFFIC_BehaviorSettings m_BehaviorSettings;
}

[ComponentEditorProps(category: "Traffic System", description: "Ambient traffic manager - works standalone or with Mission Header. Attach to GameMode or any persistent entity.")]
class SCR_AmbientTrafficManagerClass : ScriptComponentClass
{
}

class SCR_TrafficEvents
{
    // Global hook: (Position, "gunfight" or "killed" or "alerted")
    static ref ScriptInvoker<vector, string> OnCivilianEvent = new ScriptInvoker<vector, string>();

    // Behavior state change events
    static ref ScriptInvoker<Vehicle, ECivilianBehaviorState> OnBehaviorStateChanged = new ScriptInvoker<Vehicle, ECivilianBehaviorState>();

    // Recovery attempt events (Vehicle, attempt number)
    static ref ScriptInvoker<Vehicle, int> OnRecoveryAttempt = new ScriptInvoker<Vehicle, int>();

    // Vehicle abandoned (max recovery attempts reached)
    static ref ScriptInvoker<Vehicle> OnVehicleAbandoned = new ScriptInvoker<Vehicle>();
}

class SCR_AmbientTrafficManager : ScriptComponent
{
    // --- Attributes ---
    [Attribute("10", desc: "Max active vehicles allowed")]
    protected int m_iMaxVehicles;

    [Attribute("2000", desc: "Despawn Distance (Meters)")] 
    protected float m_fDespawnDistance;

    // PREFABS
    [Attribute(desc: "List of vehicle prefabs to spawn randomly", UIWidgets.ResourceNamePicker)]
    protected ref array<ResourceName> m_aVehicleOptions;

    [Attribute("{22E43956740A6794}Prefabs/Characters/Factions/CIV/GenericCivilians/Character_CIV_Randomized.et", params: "et")]
    protected ResourceName m_DriverPrefab;

    [Attribute("{750A8D1695BD6998}Prefabs/AI/Waypoints/AIWaypoint_Move.et", params: "et")]
    protected ResourceName m_WaypointPrefab;

    [Attribute(desc: "Select a Group Prefab (e.g. Group_Base.et or similar)", params: "et")]
	protected ResourceName m_GroupPrefab;

    // Tracking
    protected ref array<Vehicle> m_aActiveVehicles = {};
    protected ref map<Vehicle, vector> m_mVehicleDestinations = new map<Vehicle, vector>(); // Track dest for dynamic lines
    protected ref array<ref Shape> m_aDebugShapes = {};
    protected ref array<Vehicle> m_aAbandonedVehicles = {}; // Vehicles marked for cleanup
	protected float m_fPlayerSafeRadius = 400.0;
    protected bool m_bUseBehaviorTree = true;

    // ------------------------------------------------------------------------------------------------
    // 1. Initialization
    // ------------------------------------------------------------------------------------------------
    override void OnPostInit(IEntity owner)
	{
	    if (!Replication.IsServer()) return;

	    // Sensible defaults that work without mission header
	    string factionToUse = "CIV";
	    bool shouldEnable = true;
	    bool useCatalog = true;
	    bool hasMissionHeader = false;

	    // Try to load from mission header (optional)
	    GRAD_TRAFFIC_MissionHeader header = GRAD_TRAFFIC_MissionHeader.Cast(GetGame().GetMissionHeader());
	    if (header && header.m_SpawnSettings && header.m_LimitSettings)
	    {
	        hasMissionHeader = true;

	        // Override defaults with mission header values
	        shouldEnable = header.m_SpawnSettings.m_bEnableTraffic;
	        factionToUse = header.m_SpawnSettings.m_sTargetFaction;
	        useCatalog = header.m_SpawnSettings.m_bUseCatalog;
	        m_bUseBehaviorTree = header.m_SpawnSettings.m_bUseBehaviorTree;

	        m_iMaxVehicles = header.m_LimitSettings.m_iMaxTrafficCount;
	        m_fDespawnDistance = header.m_LimitSettings.m_fTrafficSpawnRange;
	        m_fPlayerSafeRadius = header.m_LimitSettings.m_fPlayerSafeRadius;

	        Print("[TRAFFIC] Loaded configuration from Mission Header", LogLevel.NORMAL);
	    }
	    else
	    {
	        Print("[TRAFFIC] No Mission Header found - using component defaults", LogLevel.NORMAL);
	        // Component attribute defaults are already set, just ensure we have some vehicles
	    }

	    if (!shouldEnable)
	    {
	        Print("[TRAFFIC] Disabled via configuration.", LogLevel.NORMAL);
	        return;
	    }

	    // Try to populate vehicle list from catalog if enabled and list is empty
	    if (useCatalog && (m_aVehicleOptions.IsEmpty() || m_aVehicleOptions.Count() == 0))
	    {
	        if (!m_aVehicleOptions)
	            m_aVehicleOptions = {};

	        GetVehiclesFromCatalog(factionToUse, m_aVehicleOptions);
	    }

	    // Fallback: Try default vehicle prefabs if still empty
	    if (m_aVehicleOptions.IsEmpty())
	    {
	        Print("[TRAFFIC] No vehicles from catalog, trying default prefabs...", LogLevel.WARNING);
	        TryLoadDefaultVehicles();
	    }

	    // Final check - can't run without vehicles
	    if (m_aVehicleOptions.IsEmpty())
	    {
	        Print("[TRAFFIC] ERROR: No vehicle prefabs available! Configure m_aVehicleOptions or ensure entity catalog has CIV vehicles.", LogLevel.ERROR);
	        return;
	    }

	    // Subscribe to abandoned vehicle events
	    SCR_TrafficEvents.OnVehicleAbandoned.Insert(OnVehicleAbandoned);

	    Print(string.Format("[TRAFFIC] Initialized with %1 vehicles for faction %2 (BehaviorTree: %3, MissionHeader: %4)",
	        m_aVehicleOptions.Count(), factionToUse, m_bUseBehaviorTree, hasMissionHeader));
	    GetGame().GetCallqueue().CallLater(UpdateTrafficLoop, 1000, true);
	}

	// Try to load common civilian vehicles as fallback
	protected void TryLoadDefaultVehicles()
	{
	    // Common vanilla civilian vehicle paths
	    array<string> defaultPrefabs = {
	        "{2A8A8B72369B5765}Prefabs/Vehicles/Wheeled/S1203/S1203_transport_CIV.et",
	        "{E7C4D8176E09E19B}Prefabs/Vehicles/Wheeled/UAZ469/UAZ469_CIV.et",
	        "{CF76689A2E364B92}Prefabs/Vehicles/Wheeled/M998/M1025_unarmed_CIVWL.et",
	        "{3C3B0D4F0B4D5F85}Prefabs/Vehicles/Wheeled/Ural4320/Ural4320_transport_CIV.et"
	    };

	    foreach (string prefabPath : defaultPrefabs)
	    {
	        Resource res = Resource.Load(prefabPath);
	        if (res && res.IsValid())
	        {
	            m_aVehicleOptions.Insert(prefabPath);
	            Print(string.Format("[TRAFFIC] Loaded default vehicle: %1", prefabPath), LogLevel.NORMAL);
	        }
	    }
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

        m_aActiveVehicles.Insert(vehicle);
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
	        
	        // Force handbrake off (Critical!)
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
                m_mVehicleDestinations.Remove(veh);
                m_aAbandonedVehicles.RemoveItem(veh);
                SCR_EntityHelper.DeleteEntityAndChildren(veh);
                indicesToDelete.Insert(i);
                continue;
            }

            // Condition: Abandoned (stuck recovery failed)
            if (m_aAbandonedVehicles.Contains(veh))
            {
                Print(string.Format("[TRAFFIC] Cleaning up abandoned vehicle %1", veh), LogLevel.NORMAL);
                m_mVehicleDestinations.Remove(veh);
                m_aAbandonedVehicles.RemoveItem(veh);
                SCR_EntityHelper.DeleteEntityAndChildren(veh);
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

            // Despawn ONLY if no players are nearby AND it's far from its "spawn/manager" center
            // (Or check distance to all players; if far from EVERYONE, despawn)
            if (!isPlayerNearby)
            {
                float distToManager = vector.Distance(vehPos, GetOwner().GetOrigin());
                if (distToManager > m_fDespawnDistance)
                {
                    m_mVehicleDestinations.Remove(veh);
                    m_aAbandonedVehicles.RemoveItem(veh);
                    SCR_EntityHelper.DeleteEntityAndChildren(veh);
                    indicesToDelete.Insert(i);
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
	    SCR_TrafficEvents.OnCivilianEvent.Invoke(owner.GetOrigin(), "gunfight");
	}

	// Called when a vehicle's behavior tree marks it for respawn after failed recovery
	protected void OnVehicleAbandoned(Vehicle vehicle)
	{
	    if (!vehicle) return;
	    if (m_aAbandonedVehicles.Contains(vehicle)) return;

	    Print(string.Format("[TRAFFIC] Vehicle %1 marked as abandoned - will be cleaned up", vehicle), LogLevel.WARNING);
	    m_aAbandonedVehicles.Insert(vehicle);
	}
	
    protected vector GetRandomMapPos()
    {
        vector mapMin, mapMax;
        GetGame().GetWorldEntity().GetWorldBounds(mapMin, mapMax);
        return Vector(Math.RandomFloat(mapMin[0], mapMax[0]), 0, Math.RandomFloat(mapMin[2], mapMax[2]));
    }	
}