StructureStats

Blacklist

To exclude a structure from destruction tracking, add its blueprint path to `DestructionBlacklist` in `config.json`:

"DestructionBlacklist": [
    "Blueprint'/Game/PrimalEarth/Structures/SleepingBag.SleepingBag'"
]

Blueprint paths can be found in `structure_paths.txt`, which is automatically populated as structures are destroyed on your server. A community-collected reference list is included in this repo.

I have added the Tek Sleeping Pod and Sleeping Bag as references

Limitations

- C4 destructions are not counted. The destruction hook does not expose the player who placed the C4. This is a framework limitation with no current workaround.
- Demolish and self-removal are not counted. Only destructions by an enemy player are tracked.
