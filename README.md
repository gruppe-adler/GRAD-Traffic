# GRAD Traffic

Simplistic Civilian Traffic System for Reforger.

Spawns civilian cars, drives them somewhere, despawns them. Raises some events for threat level changes:

`SCR_TrafficEvents.OnCivilianEvent.Invoke(owner.GetOrigin(), "gunfight");` and 
`SCR_TrafficEvents.OnCivilianEvent.Invoke(owner.GetOrigin(), "killed");`

handing over the position of the event to any script.

---

## Mission Header Configuration

### Sample Config
```json
{
  "GRAD_TRAFFIC_MissionHeader": {
    "m_bShowDebugMarkers": true,
    "m_SpawnSettings": {
      "m_bEnableTraffic": true,
      "m_sTargetFaction": "CIV",
      "m_bUseCatalog": true
    },
    "m_LimitSettings": {
      "m_iMaxTrafficCount": 15,
      "m_fTrafficSpawnRange": 2500.0,
      "m_fPlayerSafeRadius": 500.0
    }
  }
}
```

### Settings Explained

#### Debug Settings
- **`m_bShowDebugMarkers`** (`bool`, default: `false`)  
  Display debug lines and markers in Game Master view. Shows waypoint paths and vehicle destinations for troubleshooting.

#### Spawn Settings
- **`m_bEnableTraffic`** (`bool`, default: `true`)  
  Master switch for the entire traffic system. Set to `false` to disable all traffic spawning.

- **`m_sTargetFaction`** (`string`, default: `"CIV"`)  
  Faction key to use for spawned traffic. Vehicles and drivers will be assigned to this faction. Common values: `"CIV"`, `"US"`, `"USSR"`.

- **`m_bUseCatalog`** (`bool`, default: `false`)  
  When `true`, dynamically loads vehicles from the Entity Catalog that match `m_sTargetFaction`.  
  When `false`, uses hardcoded S1203 van variants (see "Adding Custom Vehicles" below).

#### Limit Settings
- **`m_iMaxTrafficCount`** (`int`, default: `10`)  
  Maximum number of traffic vehicles allowed on the map simultaneously.

- **`m_fTrafficSpawnRange`** (`float`, default: `2000.0`)  
  Maximum distance from any player where traffic can spawn or remain active (in meters).  
  Vehicles beyond this range will despawn to save performance.

- **`m_fPlayerSafeRadius`** (`float`, default: `400.0`)  
  Minimum distance from players where traffic can spawn (in meters).  
  Prevents vehicles from popping into existence right in front of players.

---

## Adding Custom Vehicles

### Method 1: Using the Faction Catalog (Recommended)
Set `m_bUseCatalog` to `true` in your mission header. The system will automatically find all vehicles assigned to the specified faction.

**Requirements:**
- Vehicles must have a `FactionAffiliationComponent` with matching `m_sFactionKey`
- Vehicles must be registered in the Entity Catalog

**Pros:** Dynamic, no code changes needed  
**Cons:** Less control over specific vehicle selection

### Method 2: Hardcoded Vehicle List
If `m_bUseCatalog` is `false`, the system uses a hardcoded array of vehicle prefabs.

**To add custom vehicles:**

1. Open `SCR_AmbientTrafficManager.c`
2. Find the `m_aVehicleOptions` array (around line 68)
3. Add your vehicle prefab paths:

```c
protected ref array<ResourceName> m_aVehicleOptions = {
    "{D2BCF98E80CF634C}Prefabs/Vehicles/Wheeled/S1203/S1203_cargo_beige.et",
    "{YOUR_VEHICLE_GUID}Prefabs/Path/To/Your/Vehicle.et",
    "{ANOTHER_VEHICLE}Prefabs/Path/To/Another.et"
};
```


---

## WIP

<img width="480" height="420" alt="thumbnail" src="https://github.com/user-attachments/assets/6d5e19f4-f426-48fb-93e2-58d8e65b73c3" />
