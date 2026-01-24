[ComponentEditorProps(category: "Traffic System", description: "Attach this to your GameMode entity.")]
class SCR_AmbientTrafficManagerClass : ScriptComponentClass
{
}

class SCR_TrafficEvents
{
    // Global hook: (Position, "gunfight" or "killed")
    static ref ScriptInvoker<vector, string> OnCivilianEvent = new ScriptInvoker<vector, string>();
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

    // ------------------------------------------------------------------------------------------------
    // 1. Initialization
    // ------------------------------------------------------------------------------------------------
    override void OnPostInit(IEntity owner)
    {
        if (!Replication.IsServer()) return;

        // Check for AI World
        if (!GetGame().GetAIWorld())
        {
            Print("[TRAFFIC ERROR] No SCR_AIWorld found! Traffic cannot navigate.", LogLevel.ERROR);
            return;
        }

        Print("[TRAFFIC] System Initialized. Waiting 5s to start loop...", LogLevel.NORMAL);
        GetGame().GetCallqueue().CallLater(UpdateTrafficLoop, 1000, true);
    }

    // ------------------------------------------------------------------------------------------------
    // 2. The Main Loop
    // ------------------------------------------------------------------------------------------------

    protected void UpdateTrafficLoop()
    {
        CleanupTraffic();
        
        if (m_aActiveVehicles.Count() < m_iMaxVehicles)
            SpawnSingleTrafficUnit();
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
	    float radius = 20.0; // Your completion radius
	
	    // Clue #1: Use the vanilla 'Reachable' check
	    if (!roadMgr.GetReachableWaypointInRoad(group.GetOrigin(), destPos, radius, reachablePos))
	        reachablePos = destPos;
	
	    EntitySpawnParams params = new EntitySpawnParams();
	    params.Transform[3] = reachablePos;
	    
	    IEntity wpEnt = GetGame().SpawnEntityPrefab(Resource.Load(m_WaypointPrefab), GetGame().GetWorld(), params);
	    AIWaypoint wp = AIWaypoint.Cast(wpEnt);
	    
	    if (wp)
	    {
	        // Clue #2: Dynamic radius calculation
	        // The radius should be the original radius minus the offset we shifted the waypoint
	        float distShift = vector.Distance(reachablePos, destPos);
	        wp.SetCompletionRadius(Math.Max(5.0, radius - distShift));
	        
	        group.AddWaypoint(wp);
	    }
	}

    // ------------------------------------------------------------------------------------------------
    // 3. Cleanup Logic
    // ------------------------------------------------------------------------------------------------
    protected void CleanupTraffic()
    {
        array<int> indicesToDelete = {};

        for (int i = 0; i < m_aActiveVehicles.Count(); i++)
        {
            Vehicle veh = m_aActiveVehicles[i];

            // Condition 1: Null (Deleted by engine)
            if (!veh) 
            {
                indicesToDelete.Insert(i);
                continue;
            }

            // Condition 2: Destroyed
            DamageManagerComponent damageMgr = DamageManagerComponent.Cast(veh.FindComponent(DamageManagerComponent));
            if (damageMgr && damageMgr.GetState() == EDamageState.DESTROYED)
            {
                SCR_EntityHelper.DeleteEntityAndChildren(veh);
                indicesToDelete.Insert(i);
                continue;
            }
            
            // Condition 3: Distance Check
            // (Simple check implemented here for demonstration)
            // If you want to check distance to players, you'd need to iterate PlayerManager.
        }

        // Remove from array backwards to keep indices valid
        for (int i = indicesToDelete.Count() - 1; i >= 0; i--)
        {
            m_aActiveVehicles.Remove(indicesToDelete[i]);
        }
    }

    // ------------------------------------------------------------------------------------------------
    // 4. Helpers
    // ------------------------------------------------------------------------------------------------
    protected bool FindValidRoadPoints(out vector spawn, out vector dest)
    {
        // Reduce attempts to 5 to prevent freezing
        for(int i = 0; i < 5; i++)
        {
            vector searchPos = GetRandomMapPos();
            // Reduce radius to 50m (approx 100x100m box)
            BaseRoad r1 = GetNearestRoad(searchPos, 50); 
            
            if (!r1) continue;

            array<vector> p1 = {};
            r1.GetPoints(p1);
            if (p1.IsEmpty()) continue;
            
            spawn = p1[0];
            
            // Find destination
            BaseRoad r2 = GetNearestRoad(GetRandomMapPos(), 50);
            if (r2)
            {
                array<vector> p2 = {};
                r2.GetPoints(p2);
                if (!p2.IsEmpty()) 
                {
                    dest = p2[0];
                    return true;
                }
            }
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