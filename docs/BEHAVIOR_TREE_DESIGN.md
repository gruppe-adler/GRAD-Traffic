# Civilian Behavior Tree Design for GRAD-Traffic

## Overview

This document outlines the design for an improved civilian behavior system using a behavior tree-like architecture, inspired by Enfusion's AI patterns. The system addresses:

1. **Stuck vehicle detection and recovery**
2. **Varied panic/gunfight reactions**
3. **State-based behavior management**
4. **Improved flee logic with road awareness**
5. **Decoupled architecture - works on any map without configuration**

---

## Quick Start (No Configuration Required)

GRAD-Traffic now works **out of the box** on any map with roads:

### Option 1: Standalone Spawner (Recommended for Mod Maps)

```
1. Create an empty entity anywhere in your mission
2. Add SCR_StandaloneTrafficSpawner component
3. Done - traffic will spawn automatically
```

**Features:**
- Auto-detects road network from SCR_AIWorld
- Auto-loads vehicles from entity catalog OR uses built-in defaults
- No GameMode dependency
- No mission header required
- Full behavior tree support

### Option 2: Original Manager (GameMode Integration)

```
1. Add SCR_AmbientTrafficManager to your GameMode entity
2. Optionally configure vehicle prefabs
3. Optionally add GRAD_TRAFFIC_MissionHeader for fine-tuning
```

**Features:**
- Integrates with existing GameMode
- Mission header configuration (optional)
- Falls back to defaults when no config found

### Component Comparison

| Feature | StandaloneTrafficSpawner | AmbientTrafficManager |
|---------|--------------------------|----------------------|
| GameMode required | No | No (optional) |
| Mission Header | Not supported | Optional |
| Entity Catalog | Auto-detect | Auto-detect |
| Default Vehicles | Built-in fallbacks | Built-in fallbacks |
| Behavior Tree | Always enabled | Configurable |
| Attach to | Any entity | Any entity |

---

## Current System Analysis

### Problems Identified

| Issue | Description | Impact |
|-------|-------------|--------|
| No stuck detection | Vehicles can get stuck indefinitely | Performance waste, immersion breaking |
| Simple flee logic | Always flee 500m backward regardless of road | Vehicles may drive off-road or into obstacles |
| Single behavior | All civilians react identically | Unrealistic, predictable |
| No speed management | No slow-down during alert states | Jarring transitions |
| No recovery mechanism | Failed escapes result in stuck AI | Accumulating stuck vehicles |
| Collision-induced stalls | Vehicles colliding stay stuck | Traffic jams |

---

## Proposed Behavior Tree Architecture

### Behavior States (Priority Order)

```
ECivilianBehaviorState
{
    DEAD,           // Highest priority - no actions possible
    PANICKED,       // Flee behavior active
    STOPPING,       // Emergency brake engaged
    ALERTED,        // Slow driving, watching threat
    NORMAL,         // Standard driving to destination
    STUCK,          // Recovery behavior active
    IDLE            // Awaiting instructions
}
```

### Behavior Tree Structure

```
ROOT (Selector)
|
+-- [Priority 1] DEATH_CHECK (Condition)
|   +-- Is driver dead? -> Terminate all behaviors
|
+-- [Priority 2] PANIC_SEQUENCE (Sequence)
|   +-- Is threat level THREATENED?
|   +-- Select panic behavior (weighted random)
|   |   +-- FLEE_REVERSE (40%) - Reverse quickly, then flee
|   |   +-- FLEE_FORWARD (30%) - Accelerate away from threat
|   |   +-- STOP_AND_EXIT (15%) - Stop, driver exits vehicle
|   |   +-- ERRATIC_DRIVING (15%) - Swerve while fleeing
|   +-- Execute selected behavior
|   +-- Monitor for 60 seconds or until safe
|
+-- [Priority 3] STUCK_RECOVERY (Sequence)
|   +-- Has velocity been < 1 m/s for > 10 seconds?
|   +-- Is engine running?
|   +-- Try recovery actions:
|       +-- [Attempt 1] Toggle handbrake, force throttle
|       +-- [Attempt 2] Reverse for 3 seconds
|       +-- [Attempt 3] Recalculate waypoint to nearest road
|       +-- [Attempt 4] Teleport to nearest valid road point
|       +-- [Attempt 5] Despawn and respawn elsewhere
|
+-- [Priority 4] ALERT_BEHAVIOR (Sequence)
|   +-- Is threat level VIGILANT or ALERTED?
|   +-- Reduce speed by 50%
|   +-- Increase awareness (look around)
|   +-- Continue toward destination cautiously
|
+-- [Priority 5] NORMAL_DRIVING (Action)
|   +-- Follow waypoint to destination
|   +-- Standard speed
```

---

## Implementation Details

### 1. Stuck Vehicle Detection

**Monitoring Parameters:**
- `m_fVelocityThreshold = 1.0` (m/s) - Below this, consider "stopped"
- `m_fStuckTimeThreshold = 10.0` (seconds) - Time before triggering recovery
- `m_iMaxRecoveryAttempts = 5` - Before forced despawn

**Detection Logic:**
```c
void MonitorVelocity()
{
    float currentSpeed = GetVehicleSpeed();

    if (currentSpeed < m_fVelocityThreshold && m_CurrentState == NORMAL)
    {
        m_fStuckTimer += DELTA_TIME;

        if (m_fStuckTimer > m_fStuckTimeThreshold)
        {
            SetState(ECivilianBehaviorState.STUCK);
            AttemptRecovery();
        }
    }
    else
    {
        m_fStuckTimer = 0;
    }
}
```

### 2. Panic Behavior Variations

**FLEE_REVERSE (Most Common)**
- Emergency brake for 1 second
- Reverse for 2-3 seconds
- Turn around and accelerate away

**FLEE_FORWARD**
- Floor the accelerator immediately
- Head toward nearest road point away from threat

**STOP_AND_EXIT**
- Emergency brake
- Driver exits vehicle
- Driver runs away on foot (if SCR_AIGroup supports infantry waypoints)
- Vehicle becomes abandoned

**ERRATIC_DRIVING**
- Random steering inputs while accelerating
- Simulates panicked driver losing control
- Higher chance of crashing (intentional for realism)

### 3. Improved Flee Logic with Road Awareness

Instead of blindly fleeing 500m backward:

```c
protected vector CalculateSmartFleePosition(IEntity owner, vector threatPos)
{
    SCR_AIWorld aiWorld = SCR_AIWorld.Cast(GetGame().GetAIWorld());
    RoadNetworkManager roadMgr = aiWorld.GetRoadNetworkManager();

    // Calculate direction away from threat
    vector fleeDirection = owner.GetOrigin() - threatPos;
    fleeDirection.Normalize();

    // Find a point 300-600m in that direction
    float fleeDistance = Math.RandomFloat(300, 600);
    vector idealFleePos = owner.GetOrigin() + (fleeDirection * fleeDistance);

    // Find nearest reachable road point
    vector validFleePos;
    if (roadMgr.GetReachableWaypointInRoad(owner.GetOrigin(), idealFleePos, 200.0, validFleePos))
    {
        return validFleePos;
    }

    // Fallback: Original backward flee
    return owner.GetOrigin() - (owner.GetWorldTransformAxis(2) * 500);
}
```

### 4. Recovery Actions

**Level 1: Soft Recovery**
```c
void TrySoftRecovery()
{
    CarControllerComponent controller = GetCarController();

    // Toggle handbrake
    controller.SetPersistentHandBrake(true);
    Sleep(500);
    controller.SetPersistentHandBrake(false);

    // Force engine restart
    controller.StartEngine();
}
```

**Level 2: Reverse Attempt**
```c
void TryReverseRecovery()
{
    // Apply reverse gear and throttle for 3 seconds
    // Then attempt forward movement
}
```

**Level 3: Waypoint Recalculation**
```c
void TryWaypointRecovery()
{
    // Find nearest road point
    // Clear all waypoints
    // Create new waypoint to nearest valid road
}
```

**Level 4: Teleport Recovery**
```c
void TryTeleportRecovery()
{
    // Find nearest valid road point within 50m
    // Teleport vehicle to that point
    // Re-enable AI
}
```

**Level 5: Force Respawn**
```c
void ForceRespawn()
{
    // Mark vehicle for despawn
    // Traffic manager will spawn replacement elsewhere
}
```

---

## Configuration Options

Add to `GRAD_TRAFFIC_MissionHeader`:

```c
[BaseContainerProps()]
class GRAD_TRAFFIC_BehaviorSettings
{
    [Attribute("10", desc: "Seconds before vehicle is considered stuck")]
    float m_fStuckTimeThreshold;

    [Attribute("5", desc: "Max recovery attempts before despawn")]
    int m_iMaxRecoveryAttempts;

    [Attribute("1", desc: "Enable varied panic behaviors")]
    bool m_bEnableVariedPanic;

    [Attribute("1", desc: "Allow drivers to exit vehicles when panicked")]
    bool m_bAllowPanicExit;

    [Attribute("60", desc: "Panic duration in seconds")]
    float m_fPanicDuration;
}
```

---

## Event System Extensions

Add new event types:

```c
class SCR_TrafficEvents
{
    static ref ScriptInvoker<vector, string> OnCivilianEvent = new ScriptInvoker<vector, string>();

    // New events
    static ref ScriptInvoker<Vehicle, ECivilianBehaviorState> OnBehaviorStateChanged =
        new ScriptInvoker<Vehicle, ECivilianBehaviorState>();

    static ref ScriptInvoker<Vehicle, int> OnRecoveryAttempt =
        new ScriptInvoker<Vehicle, int>();

    static ref ScriptInvoker<Vehicle> OnVehicleAbandoned =
        new ScriptInvoker<Vehicle>();
}
```

---

## Performance Considerations

1. **Stagger velocity checks** - Don't check all vehicles every frame
2. **Use distance culling** - Only run complex behaviors near players
3. **Pool waypoint entities** - Reuse instead of create/destroy
4. **Limit concurrent panic behaviors** - Cap at 3-5 to prevent chaos

---

## Migration Path

1. Add `ECivilianBehaviorState` enum
2. Add state management to `SCR_CivilianTrafficObserver`
3. Implement velocity monitoring in traffic manager
4. Add recovery logic
5. Implement panic behavior variations
6. Add configuration options
7. Test and tune thresholds

---

## Files to Modify

| File | Changes |
|------|---------|
| `SCR_AmbientTrafficManager.c` | Add velocity monitoring, recovery callbacks |
| `SCR_CivilianTrafficObserver.c` | Complete rewrite with behavior tree |
| `GRAD_TRAFFIC_MissionHeader` | Add behavior settings class |

---

## Testing Checklist

- [ ] Vehicles recover from stuck states
- [ ] Different panic behaviors trigger correctly
- [ ] Flee positions are on valid roads
- [ ] Recovery doesn't cause physics issues
- [ ] Performance is acceptable with 15+ vehicles
- [ ] Configuration options work correctly
- [ ] Events fire correctly for external listeners
