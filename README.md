# GRAD Traffic

Simplistic Civilian Traffic System for Reforger.

Spawns civilian cars, drives them somewhere, despawns them. Raises some events for threat level changes:

`SCR_TrafficEvents.OnCivilianEvent.Invoke(owner.GetOrigin(), "gunfight");` and 
`SCR_TrafficEvents.OnCivilianEvent.Invoke(owner.GetOrigin(), "killed");`

handing over the position of the event to any script.


## Sample Mission Header Config
```
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

## WIP

<img width="480" height="420" alt="thumbnail" src="https://github.com/user-attachments/assets/6d5e19f4-f426-48fb-93e2-58d8e65b73c3" />
