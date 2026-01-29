// ============================================================================
// GRAD-Traffic Civilian Behavior Tree System
// ============================================================================
// A lightweight behavior tree implementation for civilian AI reactions
// Inspired by Enfusion's behavior tree patterns
// ============================================================================

// --- Behavior State Enum ---
enum ECivilianBehaviorState
{
	IDLE,           // Awaiting instructions
	NORMAL,         // Standard driving to destination
	ALERTED,        // Slow driving, watching threat
	STOPPING,       // Emergency brake engaged
	PANICKED,       // Flee behavior active
	STUCK,          // Recovery behavior active
	RECOVERING,     // Currently attempting recovery
	ABANDONED,      // Driver has left vehicle
	DEAD            // Driver is dead
}

// --- Panic Behavior Types ---
enum ECivilianPanicBehavior
{
	FLEE_REVERSE,       // Reverse quickly, then flee (40%)
	FLEE_FORWARD,       // Accelerate away from threat (30%)
	STOP_AND_COWER,     // Stop and wait (15%)
	ERRATIC_DRIVING     // Swerve while fleeing (15%)
}

// --- Recovery Stage Tracking ---
enum ERecoveryStage
{
	NONE,
	SOFT_RECOVERY,      // Toggle handbrake, force throttle
	REVERSE_ATTEMPT,    // Reverse for a few seconds
	WAYPOINT_RECALC,    // Recalculate waypoint to nearest road
	TELEPORT_RECOVERY,  // Teleport to nearest valid road
	FORCE_RESPAWN       // Despawn and let manager respawn
}

// --- Behavior Settings Configuration ---
[BaseContainerProps()]
class GRAD_TRAFFIC_BehaviorSettings
{
	[Attribute("8", desc: "Seconds before vehicle is considered stuck")]
	float m_fStuckTimeThreshold;

	[Attribute("5", desc: "Max recovery attempts before despawn")]
	int m_iMaxRecoveryAttempts;

	[Attribute("1", desc: "Enable varied panic behaviors")]
	bool m_bEnableVariedPanic;

	[Attribute("0", desc: "Allow drivers to exit vehicles when panicked (experimental)")]
	bool m_bAllowPanicExit;

	[Attribute("60", desc: "Panic duration in seconds")]
	float m_fPanicDuration;

	[Attribute("1.5", desc: "Speed threshold below which vehicle is considered stuck (m/s)")]
	float m_fStuckSpeedThreshold;

	[Attribute("0.5", desc: "Speed multiplier when alerted (0.5 = 50% speed)")]
	float m_fAlertSpeedMultiplier;
}

// ============================================================================
// Behavior Tree Node Base Classes
// ============================================================================

// Abstract base for all behavior tree nodes
class GRAD_BehaviorNode
{
	// Returns: 0 = FAILURE, 1 = SUCCESS, 2 = RUNNING
	int Execute(SCR_CivilianBehaviorTree context) { return 0; }
}

// Selector: Runs children until one succeeds
class GRAD_SelectorNode : GRAD_BehaviorNode
{
	protected ref array<ref GRAD_BehaviorNode> m_aChildren = {};

	void AddChild(GRAD_BehaviorNode child)
	{
		m_aChildren.Insert(child);
	}

	override int Execute(SCR_CivilianBehaviorTree context)
	{
		foreach (GRAD_BehaviorNode child : m_aChildren)
		{
			int result = child.Execute(context);
			if (result != 0) return result; // SUCCESS or RUNNING
		}
		return 0; // All children failed
	}
}

// Sequence: Runs children until one fails
class GRAD_SequenceNode : GRAD_BehaviorNode
{
	protected ref array<ref GRAD_BehaviorNode> m_aChildren = {};

	void AddChild(GRAD_BehaviorNode child)
	{
		m_aChildren.Insert(child);
	}

	override int Execute(SCR_CivilianBehaviorTree context)
	{
		foreach (GRAD_BehaviorNode child : m_aChildren)
		{
			int result = child.Execute(context);
			if (result != 1) return result; // FAILURE or RUNNING
		}
		return 1; // All children succeeded
	}
}

// ============================================================================
// Condition Nodes
// ============================================================================

class GRAD_IsDeadCondition : GRAD_BehaviorNode
{
	override int Execute(SCR_CivilianBehaviorTree context)
	{
		return context.IsDriverDead() ? 1 : 0;
	}
}

class GRAD_IsPanickedCondition : GRAD_BehaviorNode
{
	override int Execute(SCR_CivilianBehaviorTree context)
	{
		return context.GetCurrentState() == ECivilianBehaviorState.PANICKED ? 1 : 0;
	}
}

class GRAD_IsStuckCondition : GRAD_BehaviorNode
{
	override int Execute(SCR_CivilianBehaviorTree context)
	{
		return context.IsVehicleStuck() ? 1 : 0;
	}
}

class GRAD_IsAlertedCondition : GRAD_BehaviorNode
{
	override int Execute(SCR_CivilianBehaviorTree context)
	{
		return context.GetCurrentState() == ECivilianBehaviorState.ALERTED ? 1 : 0;
	}
}

class GRAD_IsThreatenedCondition : GRAD_BehaviorNode
{
	override int Execute(SCR_CivilianBehaviorTree context)
	{
		return context.GetThreatLevel() >= 0.66 ? 1 : 0;
	}
}

// ============================================================================
// Action Nodes
// ============================================================================

class GRAD_ExecutePanicAction : GRAD_BehaviorNode
{
	override int Execute(SCR_CivilianBehaviorTree context)
	{
		context.ExecutePanicBehavior();
		return 2; // RUNNING - panic behavior takes time
	}
}

class GRAD_ExecuteRecoveryAction : GRAD_BehaviorNode
{
	override int Execute(SCR_CivilianBehaviorTree context)
	{
		return context.AttemptRecovery();
	}
}

class GRAD_ExecuteAlertDrivingAction : GRAD_BehaviorNode
{
	override int Execute(SCR_CivilianBehaviorTree context)
	{
		context.SetAlertDriving(true);
		return 1;
	}
}

class GRAD_ExecuteNormalDrivingAction : GRAD_BehaviorNode
{
	override int Execute(SCR_CivilianBehaviorTree context)
	{
		context.SetAlertDriving(false);
		return 1;
	}
}

// ============================================================================
// Main Behavior Tree Component
// ============================================================================

[ComponentEditorProps(category: "Traffic System", description: "Behavior tree for civilian AI")]
class SCR_CivilianBehaviorTreeClass : ScriptComponentClass {}

class SCR_CivilianBehaviorTree : ScriptComponent
{
	// --- State Tracking ---
	protected ECivilianBehaviorState m_eCurrentState = ECivilianBehaviorState.IDLE;
	protected ECivilianPanicBehavior m_eSelectedPanicBehavior;
	protected ERecoveryStage m_eRecoveryStage = ERecoveryStage.NONE;

	// --- Threat Tracking ---
	protected float m_fCurrentThreatLevel = 0;
	protected vector m_vLastKnownThreatPos;
	protected SCR_AIThreatSystem m_ThreatSystem;

	// --- Stuck Detection ---
	protected float m_fStuckTimer = 0;
	protected float m_fLastSpeed = 0;
	protected int m_iRecoveryAttempts = 0;
	protected vector m_vLastPosition;
	protected float m_fPositionCheckTimer = 0;

	// --- Component References ---
	protected Vehicle m_Vehicle;
	protected IEntity m_Driver;
	protected SCR_AIGroup m_Group;
	protected CarControllerComponent m_CarController;
	protected SCR_CharacterDamageManagerComponent m_DamageManager;

	// --- Settings ---
	protected ref GRAD_TRAFFIC_BehaviorSettings m_Settings;

	// --- Behavior Tree Root ---
	protected ref GRAD_SelectorNode m_BehaviorTreeRoot;

	// --- Flags ---
	protected bool m_bInitialized = false;
	protected bool m_bDriverDead = false;
	protected bool m_bPanicActive = false;

	// ========================================================================
	// Initialization
	// ========================================================================

	override void OnPostInit(IEntity owner)
	{
		if (!Replication.IsServer()) return;

		m_Driver = owner;

		// Load or create default settings
		m_Settings = new GRAD_TRAFFIC_BehaviorSettings();
		LoadSettingsFromMissionHeader();

		// Hook damage manager
		m_DamageManager = SCR_CharacterDamageManagerComponent.Cast(owner.FindComponent(SCR_CharacterDamageManagerComponent));
		if (m_DamageManager)
			m_DamageManager.GetOnDamageStateChanged().Insert(OnDamageStateChanged);

		// Delay initialization to allow AI brain connection
		GetGame().GetCallqueue().CallLater(DelayedInit, 1500, false, owner);
	}

	protected void LoadSettingsFromMissionHeader()
	{
		GRAD_TRAFFIC_MissionHeader header = GRAD_TRAFFIC_MissionHeader.Cast(GetGame().GetMissionHeader());
		if (header && header.m_BehaviorSettings)
		{
			m_Settings = header.m_BehaviorSettings;
		}
	}

	protected void DelayedInit(IEntity owner)
	{
		// Get vehicle reference
		m_Vehicle = GetVehicle(owner);
		if (!m_Vehicle)
		{
			Print("[BEHAVIOR] No vehicle found for driver, retrying...", LogLevel.WARNING);
			GetGame().GetCallqueue().CallLater(DelayedInit, 2000, false, owner);
			return;
		}

		m_CarController = CarControllerComponent.Cast(m_Vehicle.FindComponent(CarControllerComponent));

		// Get AI group
		AIControlComponent aiControl = AIControlComponent.Cast(owner.FindComponent(AIControlComponent));
		if (aiControl)
		{
			AIAgent agent = aiControl.GetControlAIAgent();
			if (agent)
				m_Group = SCR_AIGroup.Cast(agent.GetParentGroup());
		}

		// Hook threat system
		TryHookThreatSystem(owner);

		// Build behavior tree
		BuildBehaviorTree();

		// Store initial position
		m_vLastPosition = m_Vehicle.GetOrigin();

		// Start behavior loop
		m_bInitialized = true;
		SetState(ECivilianBehaviorState.NORMAL);

		GetGame().GetCallqueue().CallLater(BehaviorLoop, 500, true);

		Print("[BEHAVIOR] Civilian behavior tree initialized", LogLevel.NORMAL);
	}

	protected void TryHookThreatSystem(IEntity owner)
	{
		SCR_AICombatComponent combatComp = SCR_AICombatComponent.Cast(owner.FindComponent(SCR_AICombatComponent));
		if (!combatComp) return;

		SCR_ChimeraAIAgent agent = combatComp.GetAiAgent();
		if (!agent)
		{
			GetGame().GetCallqueue().CallLater(TryHookThreatSystem, 2000, false, owner);
			return;
		}

		SCR_AIUtilityComponent utility = SCR_AIUtilityComponent.Cast(agent.FindComponent(SCR_AIUtilityComponent));
		if (utility && utility.m_ThreatSystem)
		{
			m_ThreatSystem = utility.m_ThreatSystem;
			m_ThreatSystem.GetOnThreatStateChanged().Insert(OnThreatStateChanged);
			Print("[BEHAVIOR] Threat system hooked", LogLevel.NORMAL);
		}
	}

	protected void BuildBehaviorTree()
	{
		// Root selector - tries each branch in priority order
		m_BehaviorTreeRoot = new GRAD_SelectorNode();

		// Priority 1: Death check
		GRAD_SequenceNode deathSequence = new GRAD_SequenceNode();
		deathSequence.AddChild(new GRAD_IsDeadCondition());
		m_BehaviorTreeRoot.AddChild(deathSequence);

		// Priority 2: Panic behavior
		GRAD_SequenceNode panicSequence = new GRAD_SequenceNode();
		panicSequence.AddChild(new GRAD_IsThreatenedCondition());
		panicSequence.AddChild(new GRAD_ExecutePanicAction());
		m_BehaviorTreeRoot.AddChild(panicSequence);

		// Priority 3: Stuck recovery
		GRAD_SequenceNode stuckSequence = new GRAD_SequenceNode();
		stuckSequence.AddChild(new GRAD_IsStuckCondition());
		stuckSequence.AddChild(new GRAD_ExecuteRecoveryAction());
		m_BehaviorTreeRoot.AddChild(stuckSequence);

		// Priority 4: Alert driving
		GRAD_SequenceNode alertSequence = new GRAD_SequenceNode();
		alertSequence.AddChild(new GRAD_IsAlertedCondition());
		alertSequence.AddChild(new GRAD_ExecuteAlertDrivingAction());
		m_BehaviorTreeRoot.AddChild(alertSequence);

		// Priority 5: Normal driving (always succeeds as fallback)
		m_BehaviorTreeRoot.AddChild(new GRAD_ExecuteNormalDrivingAction());
	}

	// ========================================================================
	// Main Behavior Loop
	// ========================================================================

	protected void BehaviorLoop()
	{
		if (!m_bInitialized || m_bDriverDead) return;
		if (!m_Vehicle || !m_Driver) return;

		// Update stuck detection
		UpdateStuckDetection();

		// Execute behavior tree
		if (m_BehaviorTreeRoot)
			m_BehaviorTreeRoot.Execute(this);
	}

	protected void UpdateStuckDetection()
	{
		if (m_eCurrentState == ECivilianBehaviorState.PANICKED) return;
		if (m_eCurrentState == ECivilianBehaviorState.RECOVERING) return;
		if (m_eCurrentState == ECivilianBehaviorState.STOPPING) return;

		// Get current vehicle speed
		Physics physics = m_Vehicle.GetPhysics();
		if (!physics) return;

		float currentSpeed = physics.GetVelocity().Length();
		m_fLastSpeed = currentSpeed;

		// Position-based stuck detection (more reliable than velocity alone)
		m_fPositionCheckTimer += 0.5; // Loop runs every 500ms

		if (m_fPositionCheckTimer >= 3.0)
		{
			float distanceMoved = vector.Distance(m_Vehicle.GetOrigin(), m_vLastPosition);
			m_vLastPosition = m_Vehicle.GetOrigin();
			m_fPositionCheckTimer = 0;

			// If moved less than 2m in 3 seconds while supposedly driving
			if (distanceMoved < 2.0 && m_eCurrentState == ECivilianBehaviorState.NORMAL)
			{
				m_fStuckTimer += 3.0;
			}
			else
			{
				m_fStuckTimer = Math.Max(0, m_fStuckTimer - 1.5);
			}
		}

		// Also check instant velocity
		if (currentSpeed < m_Settings.m_fStuckSpeedThreshold && m_eCurrentState == ECivilianBehaviorState.NORMAL)
		{
			m_fStuckTimer += 0.5;
		}
	}

	// ========================================================================
	// State Management
	// ========================================================================

	ECivilianBehaviorState GetCurrentState()
	{
		return m_eCurrentState;
	}

	void SetState(ECivilianBehaviorState newState)
	{
		if (m_eCurrentState == newState) return;

		ECivilianBehaviorState oldState = m_eCurrentState;
		m_eCurrentState = newState;

		Print(string.Format("[BEHAVIOR] State: %1 -> %2",
			typename.EnumToString(ECivilianBehaviorState, oldState),
			typename.EnumToString(ECivilianBehaviorState, newState)), LogLevel.NORMAL);

		// Fire state change event
		if (m_Vehicle)
			SCR_TrafficEvents.OnBehaviorStateChanged.Invoke(m_Vehicle, newState);
	}

	float GetThreatLevel()
	{
		return m_fCurrentThreatLevel;
	}

	bool IsDriverDead()
	{
		return m_bDriverDead;
	}

	bool IsVehicleStuck()
	{
		return m_fStuckTimer >= m_Settings.m_fStuckTimeThreshold;
	}

	// ========================================================================
	// Event Handlers
	// ========================================================================

	void OnThreatStateChanged(EAIThreatState prevState, EAIThreatState newState)
	{
		if (m_bDriverDead) return;

		// Update threat level
		switch (newState)
		{
			case EAIThreatState.SAFE:
				m_fCurrentThreatLevel = 0;
				if (m_eCurrentState == ECivilianBehaviorState.ALERTED)
					SetState(ECivilianBehaviorState.NORMAL);
				break;

			case EAIThreatState.ALERTED:
				m_fCurrentThreatLevel = 0.33;
				if (m_eCurrentState == ECivilianBehaviorState.NORMAL)
				{
					SetState(ECivilianBehaviorState.ALERTED);
					SCR_TrafficEvents.OnCivilianEvent.Invoke(m_Driver.GetOrigin(), "alerted");
				}
				break;

			case EAIThreatState.VIGILANT:
				m_fCurrentThreatLevel = 0.5;
				if (m_eCurrentState != ECivilianBehaviorState.PANICKED)
				{
					SetState(ECivilianBehaviorState.ALERTED);
					SCR_TrafficEvents.OnCivilianEvent.Invoke(m_Driver.GetOrigin(), "gunfight");
				}
				break;

			case EAIThreatState.THREATENED:
				m_fCurrentThreatLevel = 1.0;
				if (!m_bPanicActive)
				{
					SetState(ECivilianBehaviorState.PANICKED);
					SCR_TrafficEvents.OnCivilianEvent.Invoke(m_Driver.GetOrigin(), "gunfight");
				}
				break;
		}
	}

	void OnDamageStateChanged()
	{
		if (!m_DamageManager) return;

		if (m_DamageManager.GetState() == EDamageState.DESTROYED)
		{
			m_bDriverDead = true;
			SetState(ECivilianBehaviorState.DEAD);
			SCR_TrafficEvents.OnCivilianEvent.Invoke(m_Driver.GetOrigin(), "killed");

			// Stop behavior loop
			GetGame().GetCallqueue().Remove(BehaviorLoop);
		}
	}

	// ========================================================================
	// Panic Behavior Implementation
	// ========================================================================

	void ExecutePanicBehavior()
	{
		if (m_bPanicActive) return;
		m_bPanicActive = true;

		// Select panic behavior based on weighted random
		if (m_Settings.m_bEnableVariedPanic)
			m_eSelectedPanicBehavior = SelectWeightedPanicBehavior();
		else
			m_eSelectedPanicBehavior = ECivilianPanicBehavior.FLEE_REVERSE;

		Print(string.Format("[BEHAVIOR] Executing panic: %1",
			typename.EnumToString(ECivilianPanicBehavior, m_eSelectedPanicBehavior)), LogLevel.NORMAL);

		switch (m_eSelectedPanicBehavior)
		{
			case ECivilianPanicBehavior.FLEE_REVERSE:
				ExecuteFleeReverse();
				break;
			case ECivilianPanicBehavior.FLEE_FORWARD:
				ExecuteFleeForward();
				break;
			case ECivilianPanicBehavior.STOP_AND_COWER:
				ExecuteStopAndCower();
				break;
			case ECivilianPanicBehavior.ERRATIC_DRIVING:
				ExecuteErraticDriving();
				break;
		}

		// Reset panic after configured duration
		GetGame().GetCallqueue().CallLater(ResetPanic, m_Settings.m_fPanicDuration * 1000);
	}

	protected ECivilianPanicBehavior SelectWeightedPanicBehavior()
	{
		int roll = Math.RandomInt(0, 100);

		if (roll < 40)
			return ECivilianPanicBehavior.FLEE_REVERSE;
		else if (roll < 70)
			return ECivilianPanicBehavior.FLEE_FORWARD;
		else if (roll < 85)
			return ECivilianPanicBehavior.STOP_AND_COWER;
		else
			return ECivilianPanicBehavior.ERRATIC_DRIVING;
	}

	protected void ExecuteFleeReverse()
	{
		if (!m_CarController) return;

		// Emergency brake
		m_CarController.SetPersistentHandBrake(true);

		// After 1.5 seconds, release and flee
		GetGame().GetCallqueue().CallLater(StartFleeAfterBrake, 1500, false);
	}

	protected void StartFleeAfterBrake()
	{
		if (!m_CarController) return;

		m_CarController.SetPersistentHandBrake(false);

		// Calculate smart flee position
		vector fleePos = CalculateSmartFleePosition();
		CreateFleeWaypoint(fleePos);
	}

	protected void ExecuteFleeForward()
	{
		if (!m_CarController) return;

		// Immediate acceleration forward
		m_CarController.SetPersistentHandBrake(false);

		// Calculate forward flee position
		vector fleePos = m_Driver.GetOrigin() + (m_Driver.GetWorldTransformAxis(2) * 500);
		fleePos = ValidateFleePosition(fleePos);
		CreateFleeWaypoint(fleePos);
	}

	protected void ExecuteStopAndCower()
	{
		if (!m_CarController) return;

		// Full stop
		SetState(ECivilianBehaviorState.STOPPING);
		m_CarController.SetPersistentHandBrake(true);

		// Clear waypoints - just stop
		if (m_Group)
		{
			array<AIWaypoint> waypoints = {};
			m_Group.GetWaypoints(waypoints);
			foreach (AIWaypoint wp : waypoints)
				m_Group.RemoveWaypoint(wp);
		}

		// Release brake after panic duration
		GetGame().GetCallqueue().CallLater(ReleaseHandbrakeAfterCower, m_Settings.m_fPanicDuration * 1000);
	}

	protected void ReleaseHandbrakeAfterCower()
	{
		if (m_CarController)
			m_CarController.SetPersistentHandBrake(false);
	}

	protected void ExecuteErraticDriving()
	{
		// Flee forward but with erratic behavior flag
		ExecuteFleeForward();
		// Could add steering variations here in future
	}

	protected vector CalculateSmartFleePosition()
	{
		SCR_AIWorld aiWorld = SCR_AIWorld.Cast(GetGame().GetAIWorld());
		if (!aiWorld) return m_Driver.GetOrigin() - (m_Driver.GetWorldTransformAxis(2) * 500);

		RoadNetworkManager roadMgr = aiWorld.GetRoadNetworkManager();
		if (!roadMgr) return m_Driver.GetOrigin() - (m_Driver.GetWorldTransformAxis(2) * 500);

		// Calculate direction away from threat (or just backward if no threat pos)
		vector fleeDirection;
		if (m_vLastKnownThreatPos != vector.Zero)
		{
			fleeDirection = m_Driver.GetOrigin() - m_vLastKnownThreatPos;
			fleeDirection.Normalize();
		}
		else
		{
			fleeDirection = -m_Driver.GetWorldTransformAxis(2);
		}

		// Find a point 300-600m in that direction
		float fleeDistance = Math.RandomFloat(300, 600);
		vector idealFleePos = m_Driver.GetOrigin() + (fleeDirection * fleeDistance);

		return ValidateFleePosition(idealFleePos);
	}

	protected vector ValidateFleePosition(vector idealPos)
	{
		SCR_AIWorld aiWorld = SCR_AIWorld.Cast(GetGame().GetAIWorld());
		if (!aiWorld) return idealPos;

		RoadNetworkManager roadMgr = aiWorld.GetRoadNetworkManager();
		if (!roadMgr) return idealPos;

		// Find nearest reachable road point
		vector validFleePos;
		if (roadMgr.GetReachableWaypointInRoad(m_Driver.GetOrigin(), idealPos, 200.0, validFleePos))
		{
			return validFleePos;
		}

		// Fallback to ideal position
		return idealPos;
	}

	protected void CreateFleeWaypoint(vector fleePos)
	{
		if (!m_Group) return;

		// Clear old waypoints
		array<AIWaypoint> waypoints = {};
		m_Group.GetWaypoints(waypoints);
		foreach (AIWaypoint wp : waypoints)
			m_Group.RemoveWaypoint(wp);

		// Create new flee waypoint
		ResourceName wpPrefab = "{750A8D1695BD6998}Prefabs/AI/Waypoints/AIWaypoint_Move.et";
		EntitySpawnParams params = new EntitySpawnParams();
		params.Transform[3] = fleePos;

		IEntity wpEnt = GetGame().SpawnEntityPrefab(Resource.Load(wpPrefab), GetGame().GetWorld(), params);
		SCR_AIWaypoint escapeWp = SCR_AIWaypoint.Cast(wpEnt);

		if (escapeWp)
		{
			escapeWp.SetCompletionRadius(30.0);
			escapeWp.SetCompletionType(EAIWaypointCompletionType.Any);
			m_Group.AddWaypoint(escapeWp);
		}
	}

	protected void ResetPanic()
	{
		m_bPanicActive = false;
		m_fCurrentThreatLevel = 0;

		if (m_eCurrentState == ECivilianBehaviorState.PANICKED ||
			m_eCurrentState == ECivilianBehaviorState.STOPPING)
		{
			SetState(ECivilianBehaviorState.NORMAL);
		}
	}

	// ========================================================================
	// Recovery Implementation
	// ========================================================================

	int AttemptRecovery()
	{
		if (m_eCurrentState == ECivilianBehaviorState.RECOVERING)
			return 2; // Still in progress

		m_iRecoveryAttempts++;
		SetState(ECivilianBehaviorState.RECOVERING);

		Print(string.Format("[BEHAVIOR] Recovery attempt %1/%2",
			m_iRecoveryAttempts, m_Settings.m_iMaxRecoveryAttempts), LogLevel.WARNING);

		// Fire recovery event
		if (m_Vehicle)
			SCR_TrafficEvents.OnRecoveryAttempt.Invoke(m_Vehicle, m_iRecoveryAttempts);

		// Determine recovery stage based on attempt number
		if (m_iRecoveryAttempts <= 1)
		{
			TrySoftRecovery();
		}
		else if (m_iRecoveryAttempts == 2)
		{
			TryReverseRecovery();
		}
		else if (m_iRecoveryAttempts == 3)
		{
			TryWaypointRecovery();
		}
		else if (m_iRecoveryAttempts == 4)
		{
			TryTeleportRecovery();
		}
		else
		{
			// Max attempts reached - mark for respawn
			MarkForRespawn();
			return 1; // Success (will be removed)
		}

		// Schedule recovery completion check
		GetGame().GetCallqueue().CallLater(CheckRecoveryResult, 5000, false);

		return 2; // RUNNING
	}

	protected void TrySoftRecovery()
	{
		Print("[BEHAVIOR] Trying soft recovery...", LogLevel.NORMAL);

		if (!m_CarController) return;

		// Toggle handbrake
		m_CarController.SetPersistentHandBrake(true);
		GetGame().GetCallqueue().CallLater(ReleaseBrakeForRecovery, 500, false);

		// Force engine restart
		m_CarController.StartEngine();
	}

	protected void ReleaseBrakeForRecovery()
	{
		if (m_CarController)
			m_CarController.SetPersistentHandBrake(false);
	}

	protected void TryReverseRecovery()
	{
		Print("[BEHAVIOR] Trying reverse recovery...", LogLevel.NORMAL);

		if (!m_Group || !m_Vehicle) return;

		// Create waypoint behind vehicle
		vector reversePos = m_Vehicle.GetOrigin() - (m_Vehicle.GetWorldTransformAxis(2) * 30);

		// Clear current waypoints
		array<AIWaypoint> waypoints = {};
		m_Group.GetWaypoints(waypoints);
		foreach (AIWaypoint wp : waypoints)
			m_Group.RemoveWaypoint(wp);

		// Create reverse waypoint
		CreateRecoveryWaypoint(reversePos);
	}

	protected void TryWaypointRecovery()
	{
		Print("[BEHAVIOR] Trying waypoint recalculation...", LogLevel.NORMAL);

		SCR_AIWorld aiWorld = SCR_AIWorld.Cast(GetGame().GetAIWorld());
		if (!aiWorld || !m_Vehicle || !m_Group) return;

		RoadNetworkManager roadMgr = aiWorld.GetRoadNetworkManager();
		if (!roadMgr) return;

		// Find nearest road point
		BaseRoad nearestRoad;
		float roadDist;
		if (roadMgr.GetClosestRoad(m_Vehicle.GetOrigin(), nearestRoad, roadDist) != -1)
		{
			array<vector> roadPoints = {};
			nearestRoad.GetPoints(roadPoints);

			if (roadPoints.Count() > 0)
			{
				// Clear and create new waypoint
				array<AIWaypoint> waypoints = {};
				m_Group.GetWaypoints(waypoints);
				foreach (AIWaypoint wp : waypoints)
					m_Group.RemoveWaypoint(wp);

				CreateRecoveryWaypoint(roadPoints[0]);
			}
		}
	}

	protected void TryTeleportRecovery()
	{
		Print("[BEHAVIOR] Trying teleport recovery...", LogLevel.WARNING);

		SCR_AIWorld aiWorld = SCR_AIWorld.Cast(GetGame().GetAIWorld());
		if (!aiWorld || !m_Vehicle) return;

		RoadNetworkManager roadMgr = aiWorld.GetRoadNetworkManager();
		if (!roadMgr) return;

		// Find nearest road point
		BaseRoad nearestRoad;
		float roadDist;
		if (roadMgr.GetClosestRoad(m_Vehicle.GetOrigin(), nearestRoad, roadDist) != -1)
		{
			array<vector> roadPoints = {};
			nearestRoad.GetPoints(roadPoints);

			if (roadPoints.Count() > 0)
			{
				// Teleport vehicle to road
				vector teleportPos = roadPoints[0];

				// Get proper ground height
				float groundY = GetGame().GetWorld().GetSurfaceY(teleportPos[0], teleportPos[2]);
				teleportPos[1] = groundY + 0.5;

				m_Vehicle.SetOrigin(teleportPos);

				// Reset physics
				Physics physics = m_Vehicle.GetPhysics();
				if (physics)
				{
					physics.SetVelocity(vector.Zero);
					physics.SetAngularVelocity(vector.Zero);
				}

				// Restart engine
				if (m_CarController)
				{
					m_CarController.StartEngine();
					m_CarController.SetPersistentHandBrake(false);
				}
			}
		}
	}

	protected void CreateRecoveryWaypoint(vector pos)
	{
		if (!m_Group) return;

		ResourceName wpPrefab = "{750A8D1695BD6998}Prefabs/AI/Waypoints/AIWaypoint_Move.et";
		EntitySpawnParams params = new EntitySpawnParams();
		params.Transform[3] = pos;

		IEntity wpEnt = GetGame().SpawnEntityPrefab(Resource.Load(wpPrefab), GetGame().GetWorld(), params);
		SCR_AIWaypoint wp = SCR_AIWaypoint.Cast(wpEnt);

		if (wp)
		{
			wp.SetCompletionRadius(10.0);
			m_Group.AddWaypoint(wp);
		}
	}

	protected void CheckRecoveryResult()
	{
		if (m_eCurrentState != ECivilianBehaviorState.RECOVERING) return;

		// Check if vehicle is moving now
		Physics physics = m_Vehicle.GetPhysics();
		if (physics && physics.GetVelocity().Length() > m_Settings.m_fStuckSpeedThreshold)
		{
			// Recovery successful
			Print("[BEHAVIOR] Recovery successful!", LogLevel.NORMAL);
			m_fStuckTimer = 0;
			SetState(ECivilianBehaviorState.NORMAL);
		}
		else
		{
			// Recovery failed, will try again in next loop
			SetState(ECivilianBehaviorState.STUCK);
		}
	}

	protected void MarkForRespawn()
	{
		Print("[BEHAVIOR] Max recovery attempts reached - marking for respawn", LogLevel.WARNING);

		// Set state to allow traffic manager to clean up
		SetState(ECivilianBehaviorState.ABANDONED);

		// Fire abandoned event for traffic manager to handle
		if (m_Vehicle)
			SCR_TrafficEvents.OnVehicleAbandoned.Invoke(m_Vehicle);
	}

	// ========================================================================
	// Alert Driving
	// ========================================================================

	void SetAlertDriving(bool alert)
	{
		// This would ideally modify AI driving behavior
		// For now, we just manage the state
		if (alert && m_eCurrentState == ECivilianBehaviorState.NORMAL)
		{
			SetState(ECivilianBehaviorState.ALERTED);
		}
		else if (!alert && m_eCurrentState == ECivilianBehaviorState.ALERTED)
		{
			SetState(ECivilianBehaviorState.NORMAL);
		}
	}

	// ========================================================================
	// Utility
	// ========================================================================

	protected Vehicle GetVehicle(IEntity owner)
	{
		SCR_CompartmentAccessComponent compartmentAccess = SCR_CompartmentAccessComponent.Cast(owner.FindComponent(SCR_CompartmentAccessComponent));
		if (!compartmentAccess) return null;

		BaseCompartmentSlot slot = compartmentAccess.GetCompartment();
		if (!slot) return null;

		return Vehicle.Cast(slot.GetOwner());
	}

	// ========================================================================
	// Cleanup
	// ========================================================================

	override void OnDelete(IEntity owner)
	{
		GetGame().GetCallqueue().Remove(BehaviorLoop);
		GetGame().GetCallqueue().Remove(DelayedInit);

		if (m_DamageManager)
			m_DamageManager.GetOnDamageStateChanged().Remove(OnDamageStateChanged);

		if (m_ThreatSystem)
			m_ThreatSystem.GetOnThreatStateChanged().Remove(OnThreatStateChanged);
	}
}
