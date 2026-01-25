[ComponentEditorProps(category: "Traffic System")]
class SCR_CivilianTrafficObserverClass : ScriptComponentClass {}

class SCR_CivilianTrafficObserver : ScriptComponent
{
	protected bool m_bPanicked = false;
	protected bool m_bKilled = false;
	protected SCR_CharacterDamageManagerComponent m_pDamageManager;
	protected SCR_AIThreatSystem m_ThreatSystem;

	//------------------------------------------------------------------------------------------------
	override void OnPostInit(IEntity owner)
	{
	    if (!Replication.IsServer()) return;
	
	    // Damage manager is usually internal to the prefab and safe to hook immediately
	    m_pDamageManager = SCR_CharacterDamageManagerComponent.Cast(owner.FindComponent(SCR_CharacterDamageManagerComponent));
	    if (m_pDamageManager)
	        m_pDamageManager.GetOnDamageStateChanged().Insert(OnDamageStateChanged);
	
	    // Delay the threat system hook by a few frames to let the AI "Brain" connect to the "Body"
	    GetGame().GetCallqueue().CallLater(TryHookThreatSystem, 1000, false, owner);
	}
	
	// Separate the logic so we can call it after a delay
	protected void TryHookThreatSystem(IEntity owner)
	{
	    SCR_AICombatComponent combatComp = SCR_AICombatComponent.Cast(owner.FindComponent(SCR_AICombatComponent));
	    if (!combatComp) return;
	
	    SCR_ChimeraAIAgent agent = combatComp.GetAiAgent();
	    if (!agent)
	    {
	        // If it's still null, try one more time in 2 seconds
	        Print("[TRAFFIC DEBUG] AI Agent not found yet, retrying...", LogLevel.WARNING);
	        GetGame().GetCallqueue().CallLater(TryHookThreatSystem, 2000, false, owner);
	        return;
	    }
	
	    SCR_AIUtilityComponent utility = SCR_AIUtilityComponent.Cast(agent.FindComponent(SCR_AIUtilityComponent));
	    if (utility && utility.m_ThreatSystem)
	    {
	        m_ThreatSystem = utility.m_ThreatSystem;
	        m_ThreatSystem.GetOnThreatStateChanged().Insert(OnThreatStateChanged);
	        Print("[TRAFFIC DEBUG] Threat System Hooked Successfully!", LogLevel.NORMAL);
	    }
	}

	//------------------------------------------------------------------------------------------------
	// Event called by the AI System whenever the threat level crosses a threshold
	void OnThreatStateChanged(EAIThreatState prevState, EAIThreatState newState)
	{
		if (m_bPanicked || m_bKilled) return;
		
		IEntity owner = GetOwner();

		// Trigger panic when AI reaches the THREATENED state (Threshold > 0.66)
		if (newState == EAIThreatState.ALERTED && 
		prevState != EAIThreatState.THREATENED && 
		prevState != EAIThreatState.VIGILANT) {
			SCR_TrafficEvents.OnCivilianEvent.Invoke(owner.GetOrigin(), "gunfight");
			Print(string.Format("[TRAFFIC DEBUG] Gunfight Event Fired. State: %1", typename.EnumToString(EAIThreatState, newState)), LogLevel.NORMAL);
		}
		if (newState == EAIThreatState.VIGILANT && 
		prevState != EAIThreatState.THREATENED) {
			SCR_TrafficEvents.OnCivilianEvent.Invoke(owner.GetOrigin(), "gunfight");
			Print(string.Format("[TRAFFIC DEBUG] Gunfight Event Fired. State: %1", typename.EnumToString(EAIThreatState, newState)), LogLevel.NORMAL);
		}
		if (newState == EAIThreatState.THREATENED)
		{
			m_bPanicked = true;
			
			SCR_TrafficEvents.OnCivilianEvent.Invoke(owner.GetOrigin(), "gunfight");
			
			Print(string.Format("[TRAFFIC DEBUG] Panic Event Fired. State: %1", typename.EnumToString(EAIThreatState, newState)), LogLevel.NORMAL);
			
			StartFleeing(owner);
			
			// Reset panic state after 60 seconds
			GetGame().GetCallqueue().CallLater(ResetPanic, 60000);
		}
	}

	//------------------------------------------------------------------------------------------------
	void OnDamageStateChanged()
	{
		if (m_bKilled || !m_pDamageManager) return;

		if (m_pDamageManager.GetState() == EDamageState.DESTROYED)
		{
			m_bKilled = true;
			SCR_TrafficEvents.OnCivilianEvent.Invoke(GetOwner().GetOrigin(), "killed");
			Print("[TRAFFIC DEBUG] Death Event Fired", LogLevel.NORMAL);
		}
	}

	//------------------------------------------------------------------------------------------------
	protected void StartFleeing(IEntity owner)
	{
		AIControlComponent aiControl = AIControlComponent.Cast(owner.FindComponent(AIControlComponent));
		if (!aiControl) return;

		AIAgent agent = aiControl.GetControlAIAgent();
		if (!agent) return;

		SCR_AIGroup group = SCR_AIGroup.Cast(agent.GetParentGroup());
		if (!group) return;

		// Clear old waypoints
		array<AIWaypoint> waypoints = {};
		group.GetWaypoints(waypoints);
		foreach (AIWaypoint wp : waypoints)
			group.RemoveWaypoint(wp);

		// Calculate escape point: 500m behind current heading
		vector fleePos = owner.GetOrigin() - (owner.GetWorldTransformAxis(2) * 500);

		// Create and Configure Waypoint
		ResourceName wpPrefab = "{750A8D1695BD6998}Prefabs/AI/Waypoints/AIWaypoint_Move.et";
		EntitySpawnParams params = new EntitySpawnParams();
		params.Transform[3] = fleePos;

		IEntity wpEnt = GetGame().SpawnEntityPrefab(Resource.Load(wpPrefab), GetGame().GetWorld(), params);
		SCR_AIWaypoint escapeWp = SCR_AIWaypoint.Cast(wpEnt);
		
		if (escapeWp)
		{
			escapeWp.SetCompletionRadius(20.0);
			escapeWp.SetCompletionType(EAIWaypointCompletionType.Any);
			group.AddWaypoint(escapeWp);
			
			Vehicle vehicle = GetVehicle(owner);
			if (vehicle)
			{
				CarControllerComponent carController = CarControllerComponent.Cast(vehicle.FindComponent(CarControllerComponent));
				if (carController)
				{
					carController.SetPersistentHandBrake(true);
					GetGame().GetCallqueue().CallLater(ReleaseHandbrake, 2000, false, carController);
				}
			}
		}
	}
	
	//------------------------------------------------------------------------------------------------
	protected void ReleaseHandbrake(CarControllerComponent carController)
	{
		if (carController)
		{
			carController.SetPersistentHandBrake(false);
			Print("[TRAFFIC DEBUG] Handbrake RELEASED - Flooring it", LogLevel.NORMAL);
		}
	}

	protected void ResetPanic() { m_bPanicked = false; }
	
	protected Vehicle GetVehicle(IEntity owner)
	{
		SCR_CompartmentAccessComponent compartmentAccess = SCR_CompartmentAccessComponent.Cast(owner.FindComponent(SCR_CompartmentAccessComponent));
		if (!compartmentAccess) return null;
	
		BaseCompartmentSlot slot = compartmentAccess.GetCompartment();
		if (!slot) return null;
	
		return Vehicle.Cast(slot.GetOwner());
	}

	//------------------------------------------------------------------------------------------------
	override void OnDelete(IEntity owner)
	{
		// Critical: Clean up listeners to prevent Null Pointer crashes
		if (m_pDamageManager)
			m_pDamageManager.GetOnDamageStateChanged().Remove(OnDamageStateChanged);
			
		if (m_ThreatSystem)
			m_ThreatSystem.GetOnThreatStateChanged().Remove(OnThreatStateChanged);
	}
}