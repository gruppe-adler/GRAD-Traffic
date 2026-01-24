[ComponentEditorProps(category: "Traffic System")]
class SCR_CivilianTrafficObserverClass : ScriptComponentClass {}

class SCR_CivilianTrafficObserver : ScriptComponent
{
    protected bool m_bPanicked = false;
    protected bool m_bKilled = false;

    override void OnPostInit(IEntity owner)
    {
        if (!Replication.IsServer()) return;

        // Use SCR_CharacterDamageManager for characters
        SCR_CharacterDamageManagerComponent dmg = SCR_CharacterDamageManagerComponent.Cast(owner.FindComponent(SCR_CharacterDamageManagerComponent));
        if (dmg)
        {
            // IMPORTANT: Match this signature below
            dmg.GetOnDamageStateChanged().Insert(OnDamageStateChanged);
        }

        // Start checking for suppression (panic)
        GetGame().GetCallqueue().CallLater(ListenToSuppression, 1000, true, owner);
    }

    // This is the specific signature the engine invoker requires
    // Param 1: EDamageState (int)
    // Param 2: EDamageState (int)
    // Param 3: bool (isJIP)
    protected void OnDamageStateChanged(EDamageState newState, EDamageState previousState, bool isJIP)
    {
        if (!m_bKilled && newState == EDamageState.DESTROYED)
        {
            m_bKilled = true;
            SCR_TrafficEvents.OnCivilianEvent.Invoke(GetOwner().GetOrigin(), "killed");
            Print("[TRAFFIC DEBUG] Death Event Fired", LogLevel.NORMAL);
        }
    }

    protected void ListenToSuppression(IEntity owner)
    {
        SignalsManagerComponent signals = SignalsManagerComponent.Cast(owner.FindComponent(SignalsManagerComponent));
        if (!signals) return;

        int sigIdx = signals.FindSignal("Suppression");
        if (sigIdx != -1 && signals.GetSignalValue(sigIdx) > 0.2)
        {
            if (!m_bPanicked)
            {
                m_bPanicked = true;
                SCR_TrafficEvents.OnCivilianEvent.Invoke(owner.GetOrigin(), "gunfight");
                Print("[TRAFFIC DEBUG] Panic Event Fired", LogLevel.NORMAL);
                
                // Optional: Start Flee Logic here
                
                GetGame().GetCallqueue().CallLater(ResetPanic, 15000);
            }
        }
    }
	
	protected void StartFleeing(IEntity owner)
	{
	    AIControlComponent aiControl = AIControlComponent.Cast(owner.FindComponent(AIControlComponent));
	    if (!aiControl) return;
	
	    AIAgent agent = aiControl.GetControlAIAgent();
	    if (!agent) return;
	
	    SCR_AIGroup group = SCR_AIGroup.Cast(agent.GetParentGroup());
	    if (!group) return;
	
	    // 2. Clear old waypoints
	    array<AIWaypoint> waypoints = {};
	    group.GetWaypoints(waypoints);
	    foreach (AIWaypoint wp : waypoints)
	        group.RemoveWaypoint(wp);
	
	    // 3. Calculate escape point: 500m behind current vehicle/character heading
	    vector fleePos = owner.GetOrigin() - (owner.GetWorldTransformAxis(2) * 500);
	
	    // 4. Create and Configure Waypoint
	    ResourceName wpPrefab = "{750A8D1695BD6998}Prefabs/AI/Waypoints/AIWaypoint_Move.et";
	    EntitySpawnParams params = new EntitySpawnParams();
	    params.Transform[3] = fleePos;
	
	    IEntity wpEnt = GetGame().SpawnEntityPrefab(Resource.Load(wpPrefab), GetGame().GetWorld(), params);
	    
	    // IMPORTANT: Cast to SCR_AIWaypoint to access movement parameters
	    SCR_AIWaypoint escapeWp = SCR_AIWaypoint.Cast(wpEnt);
	    
	    if (escapeWp)
	    {
	        escapeWp.SetCompletionRadius(20.0);
	        
	        // Fix: Use the correct Enum for completion
	        escapeWp.SetCompletionType(EAIWaypointCompletionType.Any);
	        
	        group.AddWaypoint(escapeWp);
	    }
	}

    protected void ResetPanic() { m_bPanicked = false; }
}