# HarvestScale
Scales harvest rates based on tribe size per map using config-driven rate tiers. Applies to hand harvesting, tools, and dino harvesting.

## Setup
Place `config.json` in `ArkApi/Plugins/HarvestScale/`:
```json
{
    "Rates": [1.0, 0.75, 0.5, 0.25]
}
```

## Config
Each index corresponds to tribe member count on the current map:
- Index 0 = 1 member (solo)
- Index 1 = 2 members
- Index 2 = 3 members
- Index 3 = 4 members

Values are multipliers applied to harvest yields. If tribe size exceeds the array length, the last value is used.

### Examples
**Duo server:**
```json
{ "Rates": [1.0, 0.5] }
```
**4-man server:**
```json
{ "Rates": [1.0, 0.75, 0.5, 0.25] }
```
**6-man server:**
```json
{ "Rates": [1.0, 0.9, 0.75, 0.6, 0.45, 0.3] }
```

## Notes
- Only counts tribe members on the current map, not across the cluster
- Players not in a tribe always get the solo rate
- Rates update immediately when players join or leave a tribe
