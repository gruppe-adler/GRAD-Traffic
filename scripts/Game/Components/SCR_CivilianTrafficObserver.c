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

		// 1. Setup Damage Listener
		m_pDamageManager = SCR_CharacterDamageManagerComponent.Cast(owner.FindComponent(SCR_CharacterDamageManagerComponent));
		if (m_pDamageManager)
			m_pDamageManager.GetOnDamageStateChanged().Insert(OnDamageStateChanged);

		// 2. Hook into the Threat System
		// We drill down: Character -> CombatComp -> AIAgent -> UtilityComp -> ThreatSystem
		SCR_AICombatComponent combatComp = SCR_AICombatComponent.Cast(owner.FindComponent(SCR_AICombatComponent));
		if (combatComp)
		{
			SCR_ChimeraAIAgent agent = combatComp.GetAiAgent();
			if (agent)
			{
				SCR_AIUtilityComponent utility = SCR_AIUtilityComponent.Cast(agent.FindComponent(SCR_AIUtilityComponent));
				if (utility)
				{
					m_ThreatSystem = utility.m_ThreatSystem;
					if (m_ThreatSystem)
					{
						// This is the "Hook" - No loop required!
						m_ThreatSystem.GetOnThreatStateChanged().Insert(OnThreatStateChanged);
					}
				}
			}
		}
	}

	//------------------------------------------------------------------------------------------------
	// Event called by the AI System whenever the threat level crosses a threshold
	void OnThreatStateChanged(EAIThreatState prevState, EAIThreatState newState)
	{
		if (m_bPanicked || m_bKilled) return;

		// Trigger panic when AI reaches the THREATENED state (Threshold > 0.66)
		if (newState == EAIThreatState.THREATENED)
		{
			m_bPanicked = true;
			
			IEntity owner = GetOwner();
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