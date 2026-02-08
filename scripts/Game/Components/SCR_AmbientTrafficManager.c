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

    // fired when a traffic vehicle spawns or despawns
    static ref ScriptInvoker<Vehicle> OnTrafficVehicleSpawned = new ScriptInvoker<Vehicle>();
    static ref ScriptInvoker<Vehicle> OnTrafficVehicleDespawned = new ScriptInvoker<Vehicle>();
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
        if (m_aVehicleOptions.IsEmpty()) return;
        
        vector spawnPos, destPos;
        
        // 1. Find Road Points
        if (!FindValidRoadPoints(spawnPos, destPos)) return;

        // -----------------------------------------------------------------------
        // CHECK 1: PLAYER DISTANCE (Prevent Instant Despawn)
        // -----------------------------------------------------------------------
        float closestPlayerDist = float.MAX;
        array<int> playerIds = {};
        GetGame().GetPlayerManager().GetPlayers(playerIds);
        
        // If no players, don't spawn
        if (playerIds.IsEmpty()) return;

        foreach (int pid : playerIds)
        {
            IEntity player = GetGame().GetPlayerManager().GetPlayerControlledEntity(pid);
            if (!player) continue;
            
            float dist = vector.Distance(spawnPos, player.GetOrigin());
            if (dist < closestPlayerDist)
            {
                closestPlayerDist = dist;
            }
        }

        // If the closest player is further than the despawn range, abort.
        if (closestPlayerDist > m_fDespawnDistance) return;

        // -----------------------------------------------------------------------
        // CHECK 2: TRAFFIC DENSITY (Prevent Clustering)
        // -----------------------------------------------------------------------
        foreach (Vehicle activeVeh : m_aActiveVehicles)
        {
            if (!activeVeh) continue;
            
            // Check if ANY existing traffic is within 200m of this new spawn point
            if (vector.Distance(spawnPos, activeVeh.GetOrigin()) < 200.0)
            {
                // Too close! Abort this spawn attempt.
                return;
            }
        }

        // -----------------------------------------------------------------------
        // CALCULATION: ROTATION (Prevent Reversing)
        // -----------------------------------------------------------------------
        EntitySpawnParams params = new EntitySpawnParams();
        params.TransformMode = ETransformMode.WORLD;

        // Create a vector pointing from Start to Finish
        vector direction = vector.Direction(spawnPos, destPos);
        
        // Convert to angles
        vector angles = direction.VectorToAngles();
        
        // Create matrix from angles
        vector mat[4];
        Math3D.AnglesToMatrix(angles, mat);
        
        // Apply position to matrix
        mat[3] = spawnPos;
        
        // Assign to params
        params.Transform = mat;

        // -----------------------------------------------------------------------
        // SPAWNING
        // -----------------------------------------------------------------------

        // 3. Spawn Group
        IEntity groupEnt = GetGame().SpawnEntityPrefab(Resource.Load(m_GroupPrefab), GetGame().GetWorld(), params);
        SCR_AIGroup group = SCR_AIGroup.Cast(groupEnt);
        if (!group) return;
        
        FactionManager factionMgr = GetGame().GetFactionManager();
        if (factionMgr)
        {
            Faction civFaction = factionMgr.GetFactionByKey("CIV");
            if (civFaction) group.SetFaction(civFaction);
        }

        // 4. Spawn Vehicle (Inherits Rotation)
        ResourceName randomCarPath = m_aVehicleOptions.GetRandomElement();
        IEntity vehEnt = GetGame().SpawnEntityPrefab(Resource.Load(randomCarPath), GetGame().GetWorld(), params);
        Vehicle vehicle = Vehicle.Cast(vehEnt);
        if (!vehicle)
        {
             SCR_EntityHelper.DeleteEntityAndChildren(group);
             return;
        }
        
        m_aActiveVehicles.Insert(vehicle);
        m_mVehicleDestinations.Insert(vehicle, destPos);
        
        // 5. Spawn Driver
        IEntity drvEnt = GetGame().SpawnEntityPrefab(Resource.Load(m_DriverPrefab), GetGame().GetWorld(), params);
        if (!drvEnt)
        {
             SCR_EntityHelper.DeleteEntityAndChildren(group);
             SCR_EntityHelper.DeleteEntityAndChildren(vehicle);
             return;
        }
        
        // 6. Setup AI
        AIControlComponent aiControl = AIControlComponent.Cast(drvEnt.FindComponent(AIControlComponent));
        if (aiControl)
        {
            AIAgent agent = aiControl.GetControlAIAgent();
            if (agent)
            {
                agent.PreventMaxLOD();
                group.AddAgent(agent);
            }
        }
        
        SCR_AIGroupUtilityComponent utility = SCR_AIGroupUtilityComponent.Cast(group.FindComponent(SCR_AIGroupUtilityComponent));
        if (utility) utility.SetCombatMode(EAIGroupCombatMode.HOLD_FIRE);

        // 7. Move Driver In
        MoveDriverInVehicle(vehicle, drvEnt);
        
        // Wake up brain
        if (aiControl) 
        {
            AIAgent agent = aiControl.GetControlAIAgent();
            if (agent) { agent.DeactivateAI(); agent.ActivateAI(); }
        }
        
        // 8. Start Engine
        ForceVehicleStart(vehicle);

        // 9. Assign Waypoint
        GetGame().GetCallqueue().CallLater(DelayedWaypointAssign, 2000, false, group, destPos);

        SCR_TrafficEvents.OnTrafficVehicleSpawned.Invoke(vehicle);
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
	        
	        // Force handbrake off - uncommented as probable cause for civs driving backwards all the time
	        // carController.SetPersistentHandBrake(false);
	        
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
                
                if (minPlayerDist > m_fDespawnDistance)
                {
                    m_mVehicleDestinations.Remove(veh);
                    SCR_EntityHelper.DeleteEntityAndChildren(veh);
                    indicesToDelete.Insert(i);
                    SCR_TrafficEvents.OnTrafficVehicleDespawned.Invoke(veh);
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
	
    protected vector GetRandomMapPos()
    {
        vector mapMin, mapMax;
        GetGame().GetWorldEntity().GetWorldBounds(mapMin, mapMax);
        return Vector(Math.RandomFloat(mapMin[0], mapMax[0]), 0, Math.RandomFloat(mapMin[2], mapMax[2]));
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