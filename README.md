# KI_MCP_Gefaehrte.dll

SKSE plugin of an AI companion mod for Skyrim (SE, AE and VR). It reads what Papyrus scripts cannot get at and hands it to the script `KI_MCP_Gefaehrte_Native`.

It has four jobs:

- the open quests of the journal with the texts of their objectives, and where their targets are
- the places around the player or the companion: map markers and doors that lead somewhere
- the things close to them: furniture, containers and items that lie around
- what the characters around the player say (the subtitles of the game)

## Build

GitHub builds the DLL: every push to `master` starts the workflow "Build KI_MCP_Gefaehrte.dll". The DLL is under "Artifacts" of the finished run. It goes into `Data\SKSE\Plugins` of the game.

## Functions for Papyrus

```papyrus
int Function GetInterfaceVersion() global native
string[] Function GetOpenQuests(int maxQuests) global native
bool Function IsHearing() global native
string[] Function TakeHeardLines(int maxLines) global native
string[] Function GetQuestTargets(int maxQuests, int maxTargets, Actor origin) global native
string[] Function GetNearbyPlaces(int radiusMeters, int maxMarkers, int maxDoors, Actor origin) global native
string[] Function GetNearbyThings(int radiusMeters, int maxPerKind, Actor origin) global native
string Function GetVoiceTypeName(Actor target) global native
```

`GetOpenQuests` returns one text per quest, its parts separated by tabs: tracked (1 or 0), type of the quest, name of the quest, then the text of each open objective.

`TakeHeardLines` returns the lines heard since the last call, one text per line: seconds ago, ref ID of the speaker, name of the speaker, what was said.

The last three functions see everything from `origin` (the viewer): distances, angles and what counts as "this area". `None`, or a character that is not in the loaded area, means the player.

`GetQuestTargets` returns where the targets of the tracked quests are, one text per target: place of the quest in the list of `GetOpenQuests`, place of the objective, where (0 = in the area of the viewer, 1 = behind a door of this area, 2 = somewhere else), distance in meters, angle from where the viewer faces, name of the target, name of the place, ref ID to walk to (the target, or that door).

`GetNearbyPlaces` returns the map markers and the doors into other areas around the viewer, one text per place: kind (0 = map marker, 1 = door), ref ID, distance in meters, angle, kind of map marker, 1 if the player has been there, name.

`GetNearbyThings` returns the furniture, containers and items close to the viewer, one text per thing: kind (0 = furniture, 1 = container, 2 = item), ref ID, distance in meters, angle, 1 for a seat or a locked container, name.

`GetVoiceTypeName` returns the name of the voice of a character (the editor ID of its voice type, eg `FemaleEvenToned`), which is also the name of the folder the game looks in for the sound of what the character says.

For the subtitles the plugin hooks into two calls of the game. A line `HearSubtitles=0` in `Data\SKSE\Plugins\KI_MCP_Gefaehrte.ini` switches that off.

## Credits

Forked from [mikastamm/mantella-vanilla-dialogue](https://github.com/mikastamm/mantella-vanilla-dialogue) (MIT license), the plugin that makes Mantella aware of vanilla dialogue. The build on GitHub comes from the fork by [Racvb0](https://github.com/Racvb0/mantella-vanilla-dialogue). The places of the subtitle hook come from the original.
