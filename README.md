# KI_MCP_Gefaehrte.dll

SKSE plugin of an AI companion mod for Skyrim (SE, AE and VR). It reads what Papyrus scripts cannot get at and hands it to the script `KI_MCP_Gefaehrte_Native`.

So far it has one job: the open quests of the journal with the texts of their objectives.

## Build

GitHub builds the DLL: every push to `master` starts the workflow "Build KI_MCP_Gefaehrte.dll". The DLL is under "Artifacts" of the finished run. It goes into `Data\SKSE\Plugins` of the game.

## Functions for Papyrus

```papyrus
int Function GetInterfaceVersion() global native
string[] Function GetOpenQuests(int maxQuests) global native
```

`GetOpenQuests` returns one text per quest, its parts separated by tabs: tracked (1 or 0), type of the quest, name of the quest, then the text of each open objective.

## Credits

Forked from [mikastamm/mantella-vanilla-dialogue](https://github.com/mikastamm/mantella-vanilla-dialogue) (MIT license), the plugin that makes Mantella aware of vanilla dialogue. The build on GitHub comes from the fork by [Racvb0](https://github.com/Racvb0/mantella-vanilla-dialogue). The dialogue tracking of the original is not part of this plugin at the moment.
