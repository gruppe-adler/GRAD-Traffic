[ComponentEditorProps(category: "Traffic System")]
class SCR_CivilianTrafficObserverClass : ScriptComponentClass {}

class SCR_CivilianTrafficObserver : ScriptComponent
{
    protected bool m_bPanicked = false;
    protected bool m_bKilled = false;
	protected SCR_CharacterDamageManagerComponent m_pDamageManager;

    override void OnPostInit(IEntity owner)
    {
        if (!Replication.IsServer()) return;

        m_pDamageManager = SCR_CharacterDamageManagerComponent.Cast(owner.FindComponent(SCR_CharacterDamageManagerComponent));
        if (m_pDamageManager)
        {
            // IMPORTANT: Match this signature below
            m_pDamageManager.GetOnDamageStateChanged().Insert(OnDamageStateChanged);
        }

        // Start checking for suppression (panic)
        GetGame().GetCallqueue().CallLater(ListenToSuppression, 1000, true, owner);
    }

    // This is the specific signature the engine invoker requires
    // Param 1: EDamageState (Enum)
    // Param 2: EDamageState (Enum)
    // Param 3: bool (isJIP)
	
    void OnDamageStateChanged()
    {
        if (m_bKilled || !m_pDamageManager) return;

        // Since the invoker didn't give us the state, we ask the manager for it manually
        if (m_pDamageManager.GetState() == EDamageState.DESTROYED)
        {
            m_bKilled = true;
            SCR_TrafficEvents.OnCivilianEvent.Invoke(GetOwner().GetOrigin(), "killed");
            Print("[TRAFFIC DEBUG] Death Event Fired", LogLevel.NORMAL);
        }
    }

    protected void ListenToSuppression(IEntity owner)
	{
	    // 1. Get the Combat Component from the entity
	    SCR_AICombatComponent combatComp = SCR_AICombatComponent.Cast(owner.FindComponent(SCR_AICombatComponent));
	    if (!combatComp) 
	        return;
	
	    // 2. Get the Agent and its Utility Component to access the Threat System
	    SCR_ChimeraAIAgent agent = combatComp.GetAiAgent();
	    if (!agent)
	        return;
	
	    SCR_AIUtilityComponent utility = SCR_AIUtilityComponent.Cast(agent.FindComponent(SCR_AIUtilityComponent));
	    if (!utility || !utility.m_ThreatSystem)
	        return;
	
	    // 3. Check the Threat Measure (Supression equivalent)
	    // A value > 0.2 indicates the AI is actively taking fire or under stress
	    float threatMeasure = utility.m_ThreatSystem.GetThreatMeasure();
	    
	    // Also check if the state is explicitly THREATENED (Suppressed)
	    bool isThreatened = (utility.m_ThreatSystem.GetState() == EAIThreatState.THREATENED);
		
		Print(string.Format("[TRAFFIC DEBUG] isThreatened: %1", isThreatened), LogLevel.NORMAL);
	
	    if (threatMeasure > 0.2 || isThreatened)
	    {
	        if (!m_bPanicked)
	        {
	            m_bPanicked = true;
	            
	            // Fire the Civilian Event
	            SCR_TrafficEvents.OnCivilianEvent.Invoke(owner.GetOrigin(), "gunfight");
	            Print(string.Format("[TRAFFIC DEBUG] Panic Event Fired. Threat Level: %1", threatMeasure), LogLevel.NORMAL);
	            
	            StartFleeing(owner);
	            
	            // Reset panic state after 15 seconds
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