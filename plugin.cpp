// Plugin of the AI companion mod for Skyrim (KI_MCP_Gefaehrte.esp).
// It reads what Papyrus scripts cannot get at and hands it to the script KI_MCP_Gefaehrte_Native.
// So far: the open quests of the player with the texts of their objectives and where their targets are,
// the places around the player or the companion (map markers and doors that lead somewhere),
// the things close to them (furniture, containers and items that lie around),
// and what the characters around the player say (the subtitles of the game).
//
// The project layout and the places of the subtitle hook come from mikastamm/mantella-vanilla-dialogue (MIT license),
// the build setup from Racvb0.
// Quests and places are only read. For the subtitles the plugin hooks into two calls of the game (see InstallSubtitleHook).

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <numbers>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "PCH.h"
#include "logger.h"

namespace Companion {

    // Goes up whenever the functions for the script change. The script passes it on to the companion server
    constexpr std::int32_t INTERFACE_VERSION = 11;
    constexpr std::string_view SCRIPT_NAME = "KI_MCP_Gefaehrte_Native";
    // Stands between the parts of one quest line. CleanText makes sure that no text of the game contains it
    constexpr char FIELD_SEPARATOR = '\t';
    constexpr std::size_t MAX_OBJECTIVES_PER_QUEST = 6;
    // In bytes. Longer texts are cut off
    constexpr std::size_t MAX_TEXT_LENGTH = 200;
    constexpr std::size_t DEFAULT_MAX_QUESTS = 25;
    // At most this many characters of a building that is not loaded, if the script names no number
    constexpr std::size_t DEFAULT_MAX_CHARACTERS = 30;
    constexpr std::string_view CUT_OFF_MARK = "...";

    // What was heard and not fetched by the script yet. Older lines make room for newer ones
    constexpr std::size_t MAX_STORED_HEARD_LINES = 40;
    constexpr std::size_t DEFAULT_MAX_HEARD_LINES = 20;
    constexpr std::size_t MAX_HEARD_TEXT_LENGTH = 300;
    // Lines older than this are not handed out any more
    constexpr std::int64_t MAX_HEARD_AGE_SECONDS = 600;
    // The same line of the same speaker within this time counts as one. The game hands a line over again
    // when it shows it again (eg after a menu was closed), so this is longer than a line takes to speak
    constexpr std::int64_t SAME_LINE_SECONDS = 60;
    // How many lines the plugin remembers to tell a repeated line apart, also after the script has fetched them
    constexpr std::size_t MAX_REMEMBERED_LINES = 60;

    // A line "HearSubtitles=0" in this file (seen from the folder of the game) keeps the plugin from hooking into the game
    constexpr char SETTINGS_FILE[] = "Data\\SKSE\\Plugins\\KI_MCP_Gefaehrte.ini";
    constexpr std::string_view SETTING_HEARING_OFF = "hearsubtitles=0";

    // Where the game calls SubtitleManager::ShowSubtitle: IDs of the two calling functions in the Address Library
    // (Skyrim SE and VR, then AE) and how far into the function the call is. Taken from mantella-vanilla-dialogue
    constexpr std::uint64_t REFERENCE_SOUND_ID_SE = 19119;
    constexpr std::uint64_t REFERENCE_SOUND_ID_AE = 19521;
    constexpr std::ptrdiff_t REFERENCE_SOUND_CALL_OFFSET = 0x2B2;
    constexpr std::uint64_t ACTOR_SOUND_ID_SE = 36543;
    constexpr std::uint64_t ACTOR_SOUND_ID_AE = 37544;
    constexpr std::ptrdiff_t ACTOR_SOUND_CALL_OFFSET_SE = 0x8EC;
    constexpr std::ptrdiff_t ACTOR_SOUND_CALL_OFFSET_AE = 0x8C2;
    // SubtitleManager::ShowSubtitle itself
    constexpr std::uint64_t SHOW_SUBTITLE_ID_SE = 51753;
    constexpr std::uint64_t SHOW_SUBTITLE_ID_AE = 52626;
    // A call to a fixed address is the byte E8 followed by the distance to the target as four bytes.
    // A jump to a fixed address looks the same with the byte E9
    constexpr std::uint8_t DIRECT_CALL_BYTE = 0xE8;
    constexpr std::uint8_t DIRECT_JUMP_BYTE = 0xE9;
    constexpr std::size_t DIRECT_CALL_LENGTH = 5;
    // Each redirected call needs 14 bytes of the trampoline
    constexpr std::size_t TRAMPOLINE_BYTES_PER_CALL = 14;
    constexpr std::size_t TRAMPOLINE_SPARE_BYTES = 16;
    // The game calls ShowSubtitle from a handful of places. Far more than that found in its code would mean
    // that the search went wrong, and then only the two known places are used
    constexpr std::size_t MAX_SUBTITLE_CALLS = 16;
    // A jump back to the start of ShowSubtitle from inside ShowSubtitle would be no call from outside.
    // The function is far shorter than this, so nothing this close behind its start is touched
    constexpr std::uintptr_t SUBTITLE_FUNCTION_REACH = 0x800;
    constexpr std::string_view UNKNOWN_VALUE = "?";

    // Distances go to the script in meters. One meter is about 70 game units
    constexpr float GAME_UNITS_PER_METER = 70.0f;
    constexpr float DEGREES_PER_RADIAN = 180.0f / std::numbers::pi_v<float>;
    constexpr float FULL_CIRCLE_DEGREES = 360.0f;
    // A small worldspace (eg a walled city) can lie inside a bigger one and share its coordinates. This many steps up are followed
    constexpr std::size_t MAX_WORLD_PARENTS = 4;

    // Targets of quests: how many targets of one objective are looked at and how many of them are handed out
    constexpr std::size_t MAX_CHECKED_TARGETS = 16;
    constexpr std::size_t MAX_TARGETS_PER_OBJECTIVE = 3;
    constexpr std::size_t DEFAULT_MAX_QUEST_TARGETS = 12;
    // The game keeps the doors on the way to a target in a list. A longer list than this is taken for broken data
    constexpr std::size_t MAX_DOORS_ON_THE_WAY = 32;
    // Everything the game has an ID for starts the same way, and the ID stands within these first bytes
    constexpr std::size_t FORM_START_BYTES = 0x20;
    // Where a target is: in the same area as the player, behind a door of this area, or somewhere else
    constexpr std::int32_t TARGET_IS_HERE = 0;
    constexpr std::int32_t TARGET_IS_BEHIND_DOOR = 1;
    constexpr std::int32_t TARGET_IS_ELSEWHERE = 2;

    // Places: kinds of lines, and the limits if the script names none
    constexpr std::int32_t PLACE_IS_MAP_MARKER = 0;
    constexpr std::int32_t PLACE_IS_DOOR = 1;
    constexpr std::int32_t DEFAULT_PLACE_RADIUS_METERS = 300;
    constexpr std::size_t DEFAULT_MAX_MAP_MARKERS = 8;
    constexpr std::size_t DEFAULT_MAX_DOORS = 8;
    // What the script learns about a map marker: whether the player has been there (can travel to it)
    constexpr std::int32_t MARKER_IS_KNOWN = 1;

    // Things close to the player: kinds of lines, what the script learns about a thing, and the limits if the script names none
    constexpr std::int32_t THING_IS_FURNITURE = 0;
    constexpr std::int32_t THING_IS_CONTAINER = 1;
    constexpr std::int32_t THING_IS_ITEM = 2;
    constexpr std::size_t THING_KIND_COUNT = 3;
    // Traps go out through a function of their own (GetNearbyTraps), in lines built like those of the things
    constexpr std::int32_t THING_IS_TRAP = 3;
    // For furniture: a character can sit on it. For a container: it is locked
    constexpr std::int32_t FURNITURE_IS_SEAT = 1;
    constexpr std::int32_t CONTAINER_IS_LOCKED = 1;
    constexpr std::int32_t DEFAULT_THING_RADIUS_METERS = 10;
    // Tables are no furniture for the game: they are scenery without a name. They are told by the file name of their
    // model, which holds this word (eg nobletablelong01.nif, commontablesquare01.nif) ...
    constexpr std::string_view TABLE_MODEL_WORD = "table";
    // ... and none of these: other words with "table" in them, and what only lies or stands on a table
    constexpr std::array<std::string_view, 6> NOT_A_TABLE_MODEL_WORDS = {"stable", "vegetable", "portable", "cloth", "bench", "candle"};
    // The name such a table goes out with
    constexpr std::string_view TABLE_NAME = "Table";
    constexpr std::size_t DEFAULT_MAX_THINGS_PER_KIND = 4;
    // Traps and what sets them off (tripwires, pressure plates, bear traps, swinging maces ...) have no name either.
    // The models of all of them lie in this folder of the game, each kind in a folder of its own
    // (eg traps\tripwire\traptripwire01.nif, traps\pressureplate\trapstonepressureplate01.nif), and that folder names the kind
    constexpr std::string_view TRAP_MODEL_FOLDER = "traps\\";
    constexpr std::int32_t DEFAULT_TRAP_RADIUS_METERS = 15;
    constexpr std::size_t DEFAULT_MAX_TRAPS = 6;
    // An arrow that was shot stays in the world as a projectile, not as an item. One that has hit something,
    // or that moves slower than this (game units per second), has come to rest and can be picked up
    constexpr float RESTING_ARROW_SPEED = 1.0f;

    // say: the text the two lines of the game mod carry instead of what is really spoken, and how long the text
    // the script names for the subtitle stays valid (a long sentence takes a while to speak)
    constexpr std::string_view OWN_LINE_TEXT = "...";
    constexpr std::int64_t SAY_TEXT_SECONDS = 90;

    // Finding by name: the kinds of lines, what the script learns about a character, how many lines go out
    // if the script names no number, and how many map markers and doors are looked through at most
    constexpr std::int32_t FOUND_IS_CHARACTER = 0;
    constexpr std::int32_t FOUND_IS_DOOR = 1;
    constexpr std::int32_t FOUND_IS_MAP_MARKER = 2;
    constexpr std::int32_t FOUND_IS_THING = 3;
    constexpr std::int32_t FOUND_IS_DEAD = 1;
    constexpr std::size_t DEFAULT_MAX_FOUND = 8;
    constexpr std::size_t MAX_PLACES_TO_SEARCH = 1000;
    // In UTF-8 the capital letters with marks on them (the German umlauts among them) are this byte followed by one
    // from the range below, and the small letter is that second byte plus the step
    constexpr unsigned char MARKED_LETTER_BYTE = 0xC3;
    constexpr unsigned char FIRST_MARKED_CAPITAL = 0x80;
    constexpr unsigned char LAST_MARKED_CAPITAL = 0x9E;
    constexpr unsigned char MULTIPLICATION_SIGN_BYTE = 0x97;
    constexpr unsigned char SMALL_LETTER_STEP = 0x20;

    // Surroundings: rays through the physics of the game tell what is around a character.
    // In how many directions (spread evenly, the first one straight ahead) and how many numbers go out per direction
    constexpr std::int32_t SURROUNDING_DIRECTIONS = 8;
    // What a ray found within the range: nothing, something upright (a wall, a rock, a tree), or ground that rises
    constexpr std::int32_t OBSTACLE_NONE = 0;
    constexpr std::int32_t OBSTACLE_WALL = 1;
    constexpr std::int32_t OBSTACLE_SLOPE = 2;
    constexpr std::int32_t NO_DROP = -1;
    constexpr std::int32_t DEFAULT_SURROUNDINGS_METERS = 15;
    constexpr std::int32_t MAX_SURROUNDINGS_METERS = 30;
    // The rays start at chest height, outside the body of the character
    constexpr float RAY_HEIGHT_UNITS = 90.0f;
    constexpr float RAY_START_UNITS = 30.0f;
    // A surface whose normal points up at least this much is ground one can walk on, not a wall
    constexpr float WALKABLE_NORMAL_UP = 0.6f;
    // Looking for a drop: every so far along a direction a ray goes down. Ground that lies more than DROP_HEIGHT_UNITS
    // lower than at the step before (or no ground within DROP_PROBE_UNITS) is a drop
    constexpr float DROP_STEP_UNITS = 140.0f;
    constexpr float DROP_PROBE_UNITS = 700.0f;
    constexpr float DROP_HEIGHT_UNITS = 150.0f;
    // A ray that meets something that is no obstacle (a character, a trigger, loose clutter) goes on behind it,
    // this far behind the hit and at most this often
    constexpr float RAY_PASS_STEP_UNITS = 10.0f;
    constexpr std::int32_t MAX_RAY_PASSES = 6;
    // Game units into the units of the physics of the game, if the Address Library cannot tell
    constexpr float DEFAULT_PHYSICS_SCALE = 0.0142875f;

    // Functions of the game this plugin calls through the Address Library, by their IDs for Skyrim SE (which Skyrim VR shares).
    // The Address Library for Skyrim VR is a file that grows from release to release, and an ID it does not have
    // ends the game with an error. So the plugin looks into the file first (see HasGameAddress)
    constexpr std::uint64_t CONDITION_IS_TRUE_ID = 29074;
    constexpr std::uint64_t WORLD_SINGLETON_ID = 516923;
    constexpr std::uint64_t CELL_PHYSICS_ID = 18536;
    constexpr std::uint64_t PHYSICS_SCALE_ID = 231896;
    constexpr std::uint64_t PHYSICS_LOCK_ID = 66976;
    constexpr std::uint64_t PHYSICS_UNLOCK_ID = 66982;
    constexpr std::uint64_t DISABLE_REFERENCE_ID = 19373;
    // RE::Actor::IsHostileToActor, for the order of GetCharactersAround
    constexpr std::uint64_t HOSTILE_TO_ACTOR_ID = 36537;
    // The file name is these two parts around the version of the game, eg "1-4-15-0"
    constexpr std::string_view ADDRESS_FILE_START = "Data/SKSE/Plugins/version-";
    constexpr std::string_view ADDRESS_FILE_END = ".csv";

    // ------------------------------------------------------------------ texts

    std::string_view ViewOf(const char* text) { return text ? std::string_view(text) : std::string_view(); }

    bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
        if (left.size() != right.size()) return false;
        for (std::size_t index = 0; index < left.size(); ++index) {
            const int leftLetter = std::tolower(static_cast<unsigned char>(left[index]));
            const int rightLetter = std::tolower(static_cast<unsigned char>(right[index]));
            if (leftLetter != rightLetter) return false;
        }
        return true;
    }

    bool StartsWithIgnoreCase(std::string_view text, std::string_view start) {
        return text.size() >= start.size() && EqualsIgnoreCase(text.substr(0, start.size()), start);
    }

    bool IsFollowByte(unsigned char byte) { return (byte & 0xC0) == 0x80; }

    // The HTTP plugin of the mod drops a whole list if one text in it is not valid UTF-8
    bool IsValidUtf(std::string_view text) {
        std::size_t index = 0;
        while (index < text.size()) {
            const auto lead = static_cast<unsigned char>(text[index]);
            std::size_t followCount = 0;
            unsigned char lowestSecond = 0x80;
            unsigned char highestSecond = 0xBF;
            if (lead < 0x80) {
                followCount = 0;
            } else if (lead >= 0xC2 && lead <= 0xDF) {
                followCount = 1;
            } else if (lead >= 0xE0 && lead <= 0xEF) {
                followCount = 2;
                if (lead == 0xE0) lowestSecond = 0xA0;
                if (lead == 0xED) highestSecond = 0x9F;
            } else if (lead >= 0xF0 && lead <= 0xF4) {
                followCount = 3;
                if (lead == 0xF0) lowestSecond = 0x90;
                if (lead == 0xF4) highestSecond = 0x8F;
            } else {
                return false;
            }
            if (followCount > text.size() - index - 1) return false;
            for (std::size_t offset = 1; offset <= followCount; ++offset) {
                const auto byte = static_cast<unsigned char>(text[index + offset]);
                if (!IsFollowByte(byte)) return false;
                if (offset == 1 && (byte < lowestSecond || byte > highestSecond)) return false;
            }
            index += followCount + 1;
        }
        return true;
    }

    void AppendCodePoint(std::string& target, char32_t codePoint) {
        if (codePoint < 0x80) {
            target += static_cast<char>(codePoint);
        } else if (codePoint < 0x800) {
            target += static_cast<char>(0xC0 | (codePoint >> 6));
            target += static_cast<char>(0x80 | (codePoint & 0x3F));
        } else {
            target += static_cast<char>(0xE0 | (codePoint >> 12));
            target += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
            target += static_cast<char>(0x80 | (codePoint & 0x3F));
        }
    }

    // What the bytes 80 to 9F mean in the Windows code page for Western Europe (a question mark where it has nothing)
    constexpr char32_t WINDOWS_SPECIALS[] = {0x20AC, 0x003F, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                             0x2039, 0x0152, 0x003F, 0x017D, 0x003F, 0x003F, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                             0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x003F, 0x017E, 0x0178};

    // Some language versions of the game keep their texts in the Windows code page instead of UTF-8
    std::string FromWindowsText(std::string_view text) {
        std::string converted;
        converted.reserve(text.size() + text.size() / 2);
        for (const char letter : text) {
            const auto byte = static_cast<unsigned char>(letter);
            if (byte < 0x80) {
                converted += letter;
            } else if (byte < 0xA0) {
                AppendCodePoint(converted, WINDOWS_SPECIALS[byte - 0x80]);
            } else {
                AppendCodePoint(converted, static_cast<char32_t>(byte));
            }
        }
        return converted;
    }

    // Returns the text as valid UTF-8 in one line, without spaces around it and not longer than maxLength bytes
    std::string CleanText(std::string_view raw, std::size_t maxLength = MAX_TEXT_LENGTH) {
        std::string text = IsValidUtf(raw) ? std::string(raw) : FromWindowsText(raw);
        for (char& letter : text) {
            // Tabs, line breaks and the like. The tab is the separator of the quest lines
            if (static_cast<unsigned char>(letter) < 0x20) letter = ' ';
        }
        const auto first = text.find_first_not_of(' ');
        if (first == std::string::npos) return std::string();
        const auto last = text.find_last_not_of(' ');
        text = text.substr(first, last - first + 1);

        if (text.size() > maxLength) {
            std::size_t cut = maxLength;
            // Do not cut a letter of several bytes in the middle
            while (cut > 0 && IsFollowByte(static_cast<unsigned char>(text[cut]))) --cut;
            text.resize(cut);
            text += CUT_OFF_MARK;
        }
        return text;
    }

    std::string FormatNumber(float value) { return std::to_string(std::llround(value)); }

    // ------------------------------------------------- placeholders in texts
    // The text of an objective can contain placeholders that the game fills in when it shows the text:
    // <Alias=Name> (the character, thing or place in a quest alias) and <Global=Name> (a number, eg 3 of 10).
    // The game stores the values per run of the quest ("instance") in TESQuest::instanceData.

    const RE::BGSQuestInstanceText* FindInstance(const RE::TESQuest* quest, std::uint32_t instanceID) {
        for (const auto* instance : quest->instanceData) {
            if (instance && instance->id == instanceID) return instance;
        }
        return nullptr;
    }

    const RE::BGSBaseAlias* FindAlias(const RE::TESQuest* quest, std::string_view aliasName) {
        for (const auto* alias : quest->aliases) {
            if (alias && EqualsIgnoreCase(static_cast<std::string_view>(alias->aliasName), aliasName)) return alias;
        }
        return nullptr;
    }

    // The name behind <Alias=...>, or an empty text if it cannot be found
    std::string GetAliasText(const RE::TESQuest* quest, std::uint32_t instanceID, std::string_view aliasName) {
        const auto* alias = FindAlias(quest, aliasName);
        if (!alias) return std::string();

        // The name the game stored for this run of the quest
        const auto* instance = FindInstance(quest, instanceID);
        if (instance) {
            for (const auto& stored : instance->stringData) {
                if (stored.aliasID != alias->aliasID) continue;
                const auto* form = RE::TESForm::LookupByID(stored.fullNameFormID);
                if (form) {
                    const auto name = ViewOf(form->GetName());
                    if (!name.empty()) return std::string(name);
                }
            }
        }

        // Otherwise the character or thing that fills the alias at the moment
        const auto handle = quest->GetAliasedRef(alias->aliasID);
        const auto reference = handle.get();
        if (reference) {
            const auto name = ViewOf(reference->GetDisplayFullName());
            if (!name.empty()) return std::string(name);
        }
        return std::string();
    }

    // The number behind <Global=...>, or an empty text if it cannot be found
    std::string GetGlobalText(const RE::TESQuest* quest, std::uint32_t instanceID, std::string_view globalName) {
        const auto* instance = FindInstance(quest, instanceID);
        if (instance) {
            for (const auto& stored : instance->valueData) {
                if (stored.global && EqualsIgnoreCase(ViewOf(stored.global->GetFormEditorID()), globalName)) {
                    return FormatNumber(stored.value);
                }
            }
        }
        if (quest->textGlobals) {
            for (const auto* global : *quest->textGlobals) {
                if (global && EqualsIgnoreCase(ViewOf(global->GetFormEditorID()), globalName)) {
                    return FormatNumber(global->value);
                }
            }
        }
        return std::string();
    }

    // tag is what stands between < and >, eg "Alias=Jarl", "Alias.ShortName=Jarl" or "Global=Count"
    std::string ResolveTag(std::string_view tag, const RE::TESQuest* quest, std::uint32_t instanceID) {
        const auto equalsPlace = tag.find('=');
        if (equalsPlace == std::string_view::npos) return std::string(tag);
        const auto kind = tag.substr(0, equalsPlace);
        const auto name = tag.substr(equalsPlace + 1);

        if (StartsWithIgnoreCase(kind, "Alias")) {
            const auto text = GetAliasText(quest, instanceID, name);
            // Better the name of the alias (eg "Jarl") than a gap
            return text.empty() ? std::string(name) : text;
        }
        if (EqualsIgnoreCase(kind, "Global")) {
            const auto text = GetGlobalText(quest, instanceID, name);
            return text.empty() ? std::string(UNKNOWN_VALUE) : text;
        }
        return std::string(name);
    }

    std::string ResolveTags(std::string_view text, const RE::TESQuest* quest, std::uint32_t instanceID) {
        std::string resolved;
        std::size_t position = 0;
        while (position < text.size()) {
            const auto openPlace = text.find('<', position);
            const auto closePlace = openPlace == std::string_view::npos ? openPlace : text.find('>', openPlace);
            if (closePlace == std::string_view::npos) {
                resolved.append(text.substr(position));
                break;
            }
            resolved.append(text.substr(position, openPlace - position));
            resolved.append(ResolveTag(text.substr(openPlace + 1, closePlace - openPlace - 1), quest, instanceID));
            position = closePlace + 1;
        }
        return resolved;
    }

    // ----------------------------------------------------------------- quests

    struct OpenQuest {
        const RE::TESQuest* quest = nullptr;
        std::vector<std::string> objectives;
        // The objectives behind the texts above, in the same order
        std::vector<const RE::BGSQuestObjective*> objectiveForms;
    };

    // The list behind the journal: every objective the player has ever been shown, with its state.
    // Skyrim VR keeps it at another place of the player than the other versions of the game
    const RE::BSTArray<RE::BGSInstancedQuestObjective>* GetJournalEntries() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return nullptr;
        if (REL::Module::IsVR()) {
            auto* virtualRealityData = player->GetVRPlayerRuntimeData();
            return virtualRealityData ? &virtualRealityData->objectives : nullptr;
        }
        return &player->GetPlayerRuntimeData().objectives;
    }

    // The quests with at least one objective that is shown and not done, tracked quests first
    std::vector<OpenQuest> CollectOpenQuests() {
        std::vector<OpenQuest> quests;
        const auto* entries = GetJournalEntries();
        if (!entries) return quests;

        // If an objective is in the list more than once (a quest that ran again), the newest entry counts
        std::vector<const RE::BGSInstancedQuestObjective*> newestEntries;
        std::unordered_map<const RE::BGSQuestObjective*, std::size_t> placeOfObjective;
        for (const auto& entry : *entries) {
            if (!entry.Objective) continue;
            const auto known = placeOfObjective.find(entry.Objective);
            if (known == placeOfObjective.end()) {
                placeOfObjective.emplace(entry.Objective, newestEntries.size());
                newestEntries.push_back(&entry);
            } else {
                newestEntries[known->second] = &entry;
            }
        }

        std::unordered_map<const RE::TESQuest*, std::size_t> placeOfQuest;
        for (const auto* entry : newestEntries) {
            if (entry->InstanceState != RE::QUEST_OBJECTIVE_STATE::kDisplayed) continue;
            const auto* objective = entry->Objective;
            const auto* quest = objective->ownerQuest;
            if (!quest) continue;
            if (quest->IsCompleted() || quest->data.flags.all(RE::QuestFlag::kFailed)) continue;

            std::string text = CleanText(ResolveTags(static_cast<std::string_view>(objective->displayText), quest, entry->instanceID));
            if (text.empty()) continue;

            auto known = placeOfQuest.find(quest);
            if (known == placeOfQuest.end()) {
                OpenQuest added;
                added.quest = quest;
                known = placeOfQuest.emplace(quest, quests.size()).first;
                quests.push_back(std::move(added));
            }
            auto& openQuest = quests[known->second];
            if (openQuest.objectives.size() < MAX_OBJECTIVES_PER_QUEST) {
                openQuest.objectives.push_back(std::move(text));
                openQuest.objectiveForms.push_back(objective);
            }
        }

        std::stable_partition(quests.begin(), quests.end(), [](const OpenQuest& entry) { return entry.quest->IsActive(); });
        return quests;
    }

    // -------------------------------------------------------------- positions
    // Where something is, seen from a viewer: how far away, and at which angle from where the viewer faces.
    // The viewer is the player, or the companion if the script asks for its surroundings (see GetViewer)

    // Whether the Address Library of this game has the function with this ID. Only Skyrim VR can lack one
    bool HasGameAddress(std::uint64_t wantedID) {
        if (!REL::Module::IsVR()) return true;
        static std::unordered_set<std::uint64_t> knownIDs;
        static bool isRead = false;
        if (!isRead) {
            isRead = true;
            const std::string fileName = std::string(ADDRESS_FILE_START) + REL::Module::get().version().string() + std::string(ADDRESS_FILE_END);
            std::ifstream file(fileName);
            std::string line;
            // One line per function: its ID, a comma and its place in the game. The first two lines are headings
            while (std::getline(file, line)) {
                std::uint64_t number = 0;
                std::size_t index = 0;
                while (index < line.size() && line[index] >= '0' && line[index] <= '9') {
                    number = number * 10 + static_cast<std::uint64_t>(line[index] - '0');
                    ++index;
                }
                if (index > 0 && index < line.size() && line[index] == ',') knownIDs.insert(number);
            }
            logger::info("Address Library {}: {} IDs read.", fileName, knownIDs.size());
        }
        return knownIDs.contains(wantedID);
    }

    struct View {
        std::int32_t distanceMeters = 0;
        // Negative = to the left of where the viewer faces, positive = to the right
        std::int32_t angleDegrees = 0;
    };

    View GetViewFrom(const RE::TESObjectREFR* viewer, const RE::NiPoint3& point) {
        const auto own = viewer->GetPosition();
        const float east = point.x - own.x;
        const float north = point.y - own.y;
        const float up = point.z - own.z;
        // The game counts directions clockwise from north, like a compass
        const float direction = std::atan2(east, north) * DEGREES_PER_RADIAN;
        const float facing = viewer->GetAngleZ() * DEGREES_PER_RADIAN;
        View view;
        view.distanceMeters = static_cast<std::int32_t>(std::lround(std::sqrt(east * east + north * north + up * up) / GAME_UNITS_PER_METER));
        view.angleDegrees = static_cast<std::int32_t>(std::lround(std::remainder(direction - facing, FULL_CIRCLE_DEGREES)));
        return view;
    }

    // The cell a reference is in. The game keeps it elsewhere for a reference whose cell is not loaded
    const RE::TESObjectCELL* GetCellOf(const RE::TESObjectREFR* reference) {
        if (!reference) return nullptr;
        const auto* cell = reference->GetParentCell();
        return cell ? cell : reference->GetSaveParentCell();
    }

    // The worldspace whose coordinates an outdoor cell uses: its own, or the bigger one it lies in (Whiterun lies in Skyrim)
    const RE::TESWorldSpace* GetRootWorldOf(const RE::TESObjectCELL* cell) {
        if (!cell || !cell->IsExteriorCell()) return nullptr;
        const auto* world = cell->GetRuntimeData().worldSpace;
        for (std::size_t step = 0; step < MAX_WORLD_PARENTS; ++step) {
            if (!world || !world->parentWorld) break;
            if (!world->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLandData)) break;
            world = world->parentWorld;
        }
        return world;
    }

    // Only then a distance and a direction between two positions mean anything
    bool IsSameSpace(const RE::TESObjectCELL* left, const RE::TESObjectCELL* right) {
        if (!left || !right) return false;
        if (left == right) return true;
        if (left->IsInteriorCell() || right->IsInteriorCell()) return false;
        const auto* leftWorld = GetRootWorldOf(left);
        return leftWorld && leftWorld == GetRootWorldOf(right);
    }

    // The name the game shows for a place: the name of the indoor cell, or outdoors the name of the worldspace
    std::string GetNameOfCell(const RE::TESObjectCELL* cell) {
        if (!cell) return std::string();
        if (cell->IsInteriorCell()) return CleanText(ViewOf(cell->GetFullName()));
        const auto* world = cell->GetRuntimeData().worldSpace;
        return world ? CleanText(ViewOf(world->GetFullName())) : std::string();
    }

    // Where a door leads that loads another area, or an empty text if it is no such door or the game has no name for the place.
    // It is the name the game shows when the player looks at the door
    std::string GetNameBehindDoor(const RE::TESObjectREFR* door) {
        const auto* teleport = door->extraList.GetByType<RE::ExtraTeleport>();
        const auto* link = teleport ? teleport->teleportData : nullptr;
        if (!link) return std::string();
        const auto otherDoor = link->linkedDoor.get();
        if (!otherDoor) return std::string();
        return GetNameOfCell(GetCellOf(otherDoor.get()));
    }

    // Whether these bytes of memory can be read without an error
    bool IsReadableMemory(const void* address, std::size_t length) {
        if (!address) return false;
        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQuery(address, &information, sizeof(information)) == 0) return false;
        if (information.State != MEM_COMMIT) return false;
        if ((information.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(information.BaseAddress) + information.RegionSize;
        return reinterpret_cast<std::uintptr_t>(address) + length <= regionEnd;
    }

    // Whether the pointer really leads to something the game knows (a character, a door, a topic ...). For pointers out of
    // data whose layout is not confirmed for every version of the game: nothing is used before the game itself
    // has named the same object for the ID that stands at the start of it
    bool IsKnownForm(const RE::TESForm* candidate) {
        if (!IsReadableMemory(candidate, FORM_START_BYTES)) return false;
        return RE::TESForm::LookupByID(candidate->GetFormID()) == candidate;
    }

    // ---------------------------------------------------------- quest targets
    // An objective can have targets: the characters, things or places the compass of the game points to.
    // A target names an alias of its quest, and it can have conditions that say whether it counts at the moment.
    // If a target is in another area, the game keeps the doors on the way to it with the target (TESQuestTarget::teleportPath)

    struct QuestTarget {
        std::int32_t kind = TARGET_IS_ELSEWHERE;
        // What the companion can walk to: the target itself, or the door on the way to it (0 = neither is at hand)
        RE::FormID referenceID = 0;
        View view;
        std::string name;
        std::string place;
    };

    // The first door on the way to the target that is in the area of the viewer, or nullptr
    const RE::TESObjectREFR* FindDoorOnTheWay(const RE::TESQuestTarget* target, const RE::TESObjectCELL* viewerCell) {
        const auto& doors = target->teleportPath.teleportRefs;
        if (doors.empty() || doors.size() > MAX_DOORS_ON_THE_WAY || doors.size() > doors.capacity()) return nullptr;
        if (!IsReadableMemory(doors.data(), doors.size() * sizeof(RE::TeleportPath::TeleportLink))) return nullptr;
        for (const auto& door : doors) {
            if (!IsKnownForm(door.ref)) continue;
            if (IsSameSpace(viewerCell, GetCellOf(door.ref))) return door.ref;
        }
        return nullptr;
    }

    // Whether a character is inside a building (or cave ...) the player is not in. The game has not loaded that place
    // then: the character has no body, and of the place the game only keeps in memory what it always keeps
    // (the characters who live there, the doors, and little else)
    bool IsInUnloadedInterior(const RE::Actor* character, const RE::PlayerCharacter* player) {
        if (!character || character->Is3DLoaded()) return false;
        const auto* cell = GetCellOf(character);
        return cell && cell->IsInteriorCell() && cell != GetCellOf(player);
    }

    // Goes through what is around the viewer. If the viewer stands in the loaded area, that is what the game has loaded
    // around it. If it is inside a building the player is not in, it is what the game keeps of that building in memory
    void ForEachReferenceAround(RE::Actor* viewer, float radius, const std::function<RE::BSContainer::ForEachResult(RE::TESObjectREFR*)>& callback) {
        if (viewer->Is3DLoaded()) {
            if (!HasGameAddress(WORLD_SINGLETON_ID)) return;
            auto* world = RE::TES::GetSingleton();
            if (world) world->ForEachReferenceInRange(viewer, radius, callback);
            return;
        }
        const auto* cell = GetCellOf(viewer);
        if (cell && cell->IsInteriorCell()) cell->ForEachReferenceInRange(viewer->GetPosition(), radius, callback);
    }

    // A door in the loaded area around the viewer that leads straight into this cell, the nearest one, or nullptr.
    // For targets inside a building of the town the player stands in, if the game has no way to the target at hand
    const RE::TESObjectREFR* FindLoadedDoorInto(const RE::TESObjectCELL* targetCell, RE::Actor* viewer) {
        if (!targetCell || !targetCell->IsInteriorCell()) return nullptr;
        const auto own = viewer->GetPosition();
        const RE::TESObjectREFR* nearestDoor = nullptr;
        float nearestDistance = 0.0f;

        ForEachReferenceAround(viewer, static_cast<float>(DEFAULT_PLACE_RADIUS_METERS) * GAME_UNITS_PER_METER, [&](RE::TESObjectREFR* reference) {
            if (!reference || reference->IsDisabled()) return RE::BSContainer::ForEachResult::kContinue;
            const auto* base = reference->GetBaseObject();
            if (!base || !base->Is(RE::FormType::Door)) return RE::BSContainer::ForEachResult::kContinue;
            const auto* teleport = reference->extraList.GetByType<RE::ExtraTeleport>();
            const auto* link = teleport ? teleport->teleportData : nullptr;
            if (!link) return RE::BSContainer::ForEachResult::kContinue;
            const auto otherDoor = link->linkedDoor.get();
            if (!otherDoor || GetCellOf(otherDoor.get()) != targetCell) return RE::BSContainer::ForEachResult::kContinue;
            const float distance = own.GetDistance(reference->GetPosition());
            if (!nearestDoor || distance < nearestDistance) {
                nearestDoor = reference;
                nearestDistance = distance;
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
        return nearestDoor;
    }

    // The targets of one objective that count at the moment, at most MAX_TARGETS_PER_OBJECTIVE
    std::vector<QuestTarget> CollectTargets(const RE::TESQuest* quest, const RE::BGSQuestObjective* objective, RE::PlayerCharacter* player,
                                            RE::Actor* viewer, bool canCheckConditions) {
        std::vector<QuestTarget> targets;
        if (!objective->targets) return targets;
        const auto* viewerCell = GetCellOf(viewer);
        const std::size_t count = std::min<std::size_t>(objective->numTargets, MAX_CHECKED_TARGETS);
        std::vector<const RE::TESObjectREFR*> seen;

        for (std::size_t index = 0; index < count && targets.size() < MAX_TARGETS_PER_OBJECTIVE; ++index) {
            const auto* target = objective->targets[index];
            if (!target) continue;
            const auto reference = quest->GetAliasedRef(target->alias).get();
            if (!reference || reference->IsDisabled()) continue;
            if (std::find(seen.begin(), seen.end(), reference.get()) != seen.end()) continue;
            seen.push_back(reference.get());
            // The conditions are asked about the target itself. A target without conditions always counts
            if (canCheckConditions && target->conditions.head && !target->conditions.IsTrue(reference.get(), player)) continue;

            QuestTarget found;
            found.name = CleanText(ViewOf(reference->GetDisplayFullName()));
            const auto* targetCell = GetCellOf(reference.get());
            if (IsSameSpace(viewerCell, targetCell)) {
                found.kind = TARGET_IS_HERE;
                found.referenceID = reference->GetFormID();
                found.view = GetViewFrom(viewer, reference->GetPosition());
            } else {
                // First the way the game itself has worked out, then a door that leads straight to where the target is
                const auto* door = FindDoorOnTheWay(target, viewerCell);
                if (!door) door = FindLoadedDoorInto(targetCell, viewer);
                if (door) {
                    found.kind = TARGET_IS_BEHIND_DOOR;
                    found.referenceID = door->GetFormID();
                    found.view = GetViewFrom(viewer, door->GetPosition());
                    found.place = GetNameBehindDoor(door);
                } else {
                    found.kind = TARGET_IS_ELSEWHERE;
                    found.place = GetNameOfCell(targetCell);
                }
            }
            targets.push_back(std::move(found));
        }
        return targets;
    }

    // ----------------------------------------------------------------- places
    // What helps to say where the player is: the markers of the map (farms, mills, towers, caves ...) and,
    // because a town has hardly any markers inside its walls, the doors that lead into buildings

    struct Place {
        std::int32_t kind = PLACE_IS_MAP_MARKER;
        // The map marker or the door itself, so that the companion can walk to it
        RE::FormID referenceID = 0;
        float distance = 0.0f;
        View view;
        // For a map marker: the kind of place as the game numbers it (RE::MARKER_TYPE), and MARKER_IS_KNOWN or 0
        std::int32_t type = 0;
        std::int32_t flags = 0;
        std::string name;
    };

    // Keeps the nearest places, nearest first, and of two places with the same name the nearer one
    void KeepNearest(std::vector<Place>& places, std::size_t limit) {
        std::sort(places.begin(), places.end(), [](const Place& left, const Place& right) { return left.distance < right.distance; });
        std::vector<Place> kept;
        for (auto& place : places) {
            if (kept.size() >= limit) break;
            const bool isKnownName = std::any_of(kept.begin(), kept.end(), [&](const Place& other) { return other.name == place.name; });
            if (!isKnownName) kept.push_back(std::move(place));
        }
        places = std::move(kept);
    }

    // The list of all map markers the game keeps with the player.
    // Skyrim VR keeps it at another place of the player than the other versions of the game
    const RE::BSTArray<RE::ObjectRefHandle>* GetMapMarkers(RE::PlayerCharacter* player) {
        if (REL::Module::IsVR()) {
            auto* virtualRealityData = player->GetVRPlayerRuntimeData();
            return virtualRealityData ? &virtualRealityData->currentMapMarkers : nullptr;
        }
        return &player->GetPlayerRuntimeData().currentMapMarkers;
    }

    std::vector<Place> CollectMapMarkers(RE::PlayerCharacter* player, RE::Actor* viewer, float radius, std::size_t limit) {
        std::vector<Place> places;
        const auto* viewerCell = GetCellOf(viewer);
        const auto* markers = GetMapMarkers(player);
        // Indoors the map of the world outside says nothing about what is near
        if (!markers || !viewerCell || viewerCell->IsInteriorCell()) return places;
        const auto own = viewer->GetPosition();

        for (const auto& handle : *markers) {
            const auto marker = handle.get();
            if (!marker || marker->IsDisabled()) continue;
            const float distance = own.GetDistance(marker->GetPosition());
            if (distance > radius) continue;
            const auto* extra = marker->extraList.GetByType<RE::ExtraMapMarker>();
            const auto* data = extra ? extra->mapData : nullptr;
            if (!data || !IsSameSpace(viewerCell, GetCellOf(marker.get()))) continue;

            Place place;
            place.kind = PLACE_IS_MAP_MARKER;
            place.referenceID = marker->GetFormID();
            place.name = CleanText(ViewOf(data->locationName.GetFullName()));
            if (place.name.empty()) continue;
            place.distance = distance;
            place.view = GetViewFrom(viewer, marker->GetPosition());
            place.type = static_cast<std::int32_t>(data->type.underlying());
            place.flags = data->flags.any(RE::MapMarkerData::Flag::kCanTravelTo) ? MARKER_IS_KNOWN : 0;
            places.push_back(std::move(place));
        }
        KeepNearest(places, limit);
        return places;
    }

    // The doors in the loaded area around the viewer that lead into another area (a building, a cave, out of town)
    std::vector<Place> CollectDoors(RE::Actor* viewer, float radius, std::size_t limit) {
        std::vector<Place> places;
        const auto own = viewer->GetPosition();

        ForEachReferenceAround(viewer, radius, [&](RE::TESObjectREFR* reference) {
            if (!reference || reference->IsDisabled()) return RE::BSContainer::ForEachResult::kContinue;
            const auto* base = reference->GetBaseObject();
            if (!base || !base->Is(RE::FormType::Door)) return RE::BSContainer::ForEachResult::kContinue;
            std::string name = GetNameBehindDoor(reference);
            if (name.empty()) return RE::BSContainer::ForEachResult::kContinue;

            Place place;
            place.kind = PLACE_IS_DOOR;
            place.referenceID = reference->GetFormID();
            place.name = std::move(name);
            place.distance = own.GetDistance(reference->GetPosition());
            place.view = GetViewFrom(viewer, reference->GetPosition());
            places.push_back(std::move(place));
            return RE::BSContainer::ForEachResult::kContinue;
        });
        KeepNearest(places, limit);
        return places;
    }

    // ----------------------------------------------------------------- things
    // What stands and lies around the viewer: furniture (chairs, benches, beds, workbenches), containers (chests, barrels,
    // sacks) and items that can be picked up. The companion can sit down on a seat and empty a container,
    // so each thing goes out with its ref ID

    struct Thing {
        std::int32_t kind = THING_IS_ITEM;
        RE::FormID referenceID = 0;
        float distance = 0.0f;
        View view;
        std::int32_t flags = 0;
        std::string name;
    };

    // Which kind of thing a reference is by what it is made from, or -1 if it is nothing for this list
    std::int32_t GetThingKind(const RE::TESBoundObject* base) {
        switch (base->GetFormType()) {
            case RE::FormType::Furniture:
                return THING_IS_FURNITURE;
            case RE::FormType::Container:
                return THING_IS_CONTAINER;
            case RE::FormType::Weapon:
            case RE::FormType::Armor:
            case RE::FormType::Ammo:
            case RE::FormType::AlchemyItem:
            case RE::FormType::Ingredient:
            case RE::FormType::Book:
            case RE::FormType::Scroll:
            case RE::FormType::SoulGem:
            case RE::FormType::KeyMaster:
            case RE::FormType::Misc:
                return THING_IS_ITEM;
            default:
                return -1;
        }
    }

    // The model file of scenery, in small letters and with backslashes, or an empty text for everything else
    std::string GetSceneryModel(const RE::TESBoundObject* base, bool withActivators) {
        const RE::TESModel* model = base->As<RE::TESObjectSTAT>();
        if (!model) model = base->As<RE::BGSMovableStatic>();
        if (!model && withActivators) model = base->As<RE::TESObjectACTI>();
        if (!model) return std::string();
        std::string lowered(ViewOf(model->GetModel()));
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char letter) {
            return letter == '/' ? '\\' : static_cast<char>(std::tolower(letter));
        });
        return lowered;
    }

    // Which kind of trap this is (the folder its model lies in, see TRAP_MODEL_FOLDER), or an empty text if it is none
    std::string GetTrapKind(const RE::TESBoundObject* base) {
        const std::string model = GetSceneryModel(base, true);
        std::size_t start = std::string::npos;
        if (model.starts_with(TRAP_MODEL_FOLDER)) {
            start = TRAP_MODEL_FOLDER.size();
        } else {
            const auto inside = model.find(std::string("\\") + std::string(TRAP_MODEL_FOLDER));
            if (inside != std::string::npos) start = inside + 1 + TRAP_MODEL_FOLDER.size();
        }
        if (start == std::string::npos) return std::string();
        // Below the folder of the traps comes the folder of the kind. A model right in the folder goes by its file name
        auto end = model.find('\\', start);
        if (end == std::string::npos) end = model.find('.', start);
        return CleanText(std::string_view(model).substr(start, end == std::string::npos ? std::string::npos : end - start));
    }

    // The ammunition an arrow in the world gives back, or nullptr if the reference is no arrow that has come to rest
    RE::TESAmmo* GetRestingArrowAmmo(RE::TESObjectREFR* reference) {
        auto* arrow = reference ? reference->As<RE::ArrowProjectile>() : nullptr;
        if (!arrow || arrow->IsDisabled() || arrow->IsMarkedForDeletion()) return nullptr;
        const auto& flight = arrow->GetProjectileRuntimeData();
        if (!flight.ammoSource) return nullptr;
        if (flight.impacts.empty() && flight.linearVelocity.Length() > RESTING_ARROW_SPEED) return nullptr;
        return flight.ammoSource;
    }

    // Whether this is the scenery the game builds its tables from (see TABLE_MODEL_WORD)
    bool IsTable(const RE::TESBoundObject* base) {
        const std::string model = GetSceneryModel(base, false);
        const auto lastSeparator = model.find_last_of('\\');
        const std::string lowered = lastSeparator == std::string::npos ? model : model.substr(lastSeparator + 1);
        if (lowered.find(TABLE_MODEL_WORD) == std::string::npos) return false;
        for (const auto word : NOT_A_TABLE_MODEL_WORDS) {
            if (lowered.find(word) != std::string::npos) return false;
        }
        return true;
    }

    // The things within the radius around the viewer: of every kind the nearest ones, at most limitPerKind,
    // furniture first, then containers, then items. Things without a name are left out (the game has many
    // invisible pieces of furniture that only tell characters where to lean or stand), except for tables,
    // which go out as furniture under the name TABLE_NAME. Arrows that were shot and lie or stick somewhere
    // go out as items under the name of their ammunition
    std::vector<Thing> CollectThings(RE::Actor* viewer, float radius, std::size_t limitPerKind) {
        std::array<std::vector<Thing>, THING_KIND_COUNT> thingsByKind;
        const auto own = viewer->GetPosition();

        ForEachReferenceAround(viewer, radius, [&](RE::TESObjectREFR* reference) {
            if (!reference || reference == viewer || reference->IsDisabled() || reference->IsMarkedForDeletion()) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            const auto* base = reference->GetBaseObject();
            if (!base) return RE::BSContainer::ForEachResult::kContinue;
            const auto* arrowAmmo = GetRestingArrowAmmo(reference);
            const bool isTable = !arrowAmmo && IsTable(base);
            const std::int32_t kind = arrowAmmo ? THING_IS_ITEM : (isTable ? THING_IS_FURNITURE : GetThingKind(base));
            if (kind < 0) return RE::BSContainer::ForEachResult::kContinue;
            std::string name = CleanText(ViewOf(arrowAmmo ? arrowAmmo->GetName() : reference->GetDisplayFullName()));
            if (name.empty() && isTable) name = TABLE_NAME;
            if (name.empty()) return RE::BSContainer::ForEachResult::kContinue;

            Thing thing;
            thing.kind = kind;
            thing.referenceID = reference->GetFormID();
            thing.name = std::move(name);
            thing.distance = own.GetDistance(reference->GetPosition());
            thing.view = GetViewFrom(viewer, reference->GetPosition());
            if (kind == THING_IS_FURNITURE) {
                const auto* furniture = base->As<RE::TESFurniture>();
                if (furniture && furniture->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSit)) thing.flags = FURNITURE_IS_SEAT;
            } else if (kind == THING_IS_CONTAINER) {
                const auto* extra = reference->extraList.GetByType<RE::ExtraLock>();
                if (extra && extra->lock && extra->lock->IsLocked()) thing.flags = CONTAINER_IS_LOCKED;
            }
            thingsByKind[static_cast<std::size_t>(kind)].push_back(std::move(thing));
            return RE::BSContainer::ForEachResult::kContinue;
        });

        std::vector<Thing> things;
        for (auto& ofOneKind : thingsByKind) {
            std::sort(ofOneKind.begin(), ofOneKind.end(), [](const Thing& left, const Thing& right) { return left.distance < right.distance; });
            std::size_t keptCount = 0;
            std::vector<std::string_view> keptNames;
            for (auto& thing : ofOneKind) {
                if (keptCount >= limitPerKind) break;
                // Ten plates on a table are one line. Chairs and chests stay apart, because each can be used on its own
                if (thing.kind == THING_IS_ITEM) {
                    if (std::find(keptNames.begin(), keptNames.end(), std::string_view(thing.name)) != keptNames.end()) continue;
                    keptNames.push_back(thing.name);
                }
                things.push_back(thing);
                ++keptCount;
            }
        }
        return things;
    }

    // The traps within the radius around the viewer, nearest first, at most limit. Each part of a trap is a thing
    // of its own for the game (the tripwire, the mace it lets loose), and each goes out
    std::vector<Thing> CollectTraps(RE::Actor* viewer, float radius, std::size_t limit) {
        std::vector<Thing> traps;
        const auto own = viewer->GetPosition();
        ForEachReferenceAround(viewer, radius, [&](RE::TESObjectREFR* reference) {
            if (!reference || reference->IsDisabled() || reference->IsMarkedForDeletion()) return RE::BSContainer::ForEachResult::kContinue;
            const auto* base = reference->GetBaseObject();
            if (!base) return RE::BSContainer::ForEachResult::kContinue;
            std::string trapKind = GetTrapKind(base);
            if (trapKind.empty()) return RE::BSContainer::ForEachResult::kContinue;
            Thing trap;
            trap.kind = THING_IS_TRAP;
            trap.referenceID = reference->GetFormID();
            trap.name = std::move(trapKind);
            trap.distance = own.GetDistance(reference->GetPosition());
            trap.view = GetViewFrom(viewer, reference->GetPosition());
            traps.push_back(std::move(trap));
            return RE::BSContainer::ForEachResult::kContinue;
        });
        std::sort(traps.begin(), traps.end(), [](const Thing& left, const Thing& right) { return left.distance < right.distance; });
        if (traps.size() > limit) traps.resize(limit);
        return traps;
    }

    // One thing as the text the script gets: kind, ref ID as eight hex digits, distance to the viewer in meters,
    // angle from where the viewer faces in degrees (negative = left), a flag that depends on the kind, name
    std::string GetThingLine(const Thing& thing) {
        std::string line = std::to_string(thing.kind);
        line += FIELD_SEPARATOR;
        line += std::format("{:08X}", thing.referenceID);
        line += FIELD_SEPARATOR;
        line += std::to_string(thing.view.distanceMeters);
        line += FIELD_SEPARATOR;
        line += std::to_string(thing.view.angleDegrees);
        line += FIELD_SEPARATOR;
        line += std::to_string(thing.flags);
        line += FIELD_SEPARATOR;
        line += thing.name;
        return line;
    }

    // ------------------------------------------------------------------- find
    // Finding something by its name anywhere in the loaded area: characters, doors (by the place they lead to),
    // map markers and things. The lists of the state only hold the nearest few of each, so a shop at the other end
    // of town or a character across the hall is not in them

    // A text in small letters, for comparing without minding capitals. German umlauts are made small as well
    std::string ToSearchText(std::string_view text) {
        std::string lowered(text);
        for (std::size_t index = 0; index < lowered.size(); ++index) {
            const auto letter = static_cast<unsigned char>(lowered[index]);
            const bool isAfterMark = index > 0 && static_cast<unsigned char>(lowered[index - 1]) == MARKED_LETTER_BYTE;
            if (isAfterMark && letter >= FIRST_MARKED_CAPITAL && letter <= LAST_MARKED_CAPITAL && letter != MULTIPLICATION_SIGN_BYTE) {
                lowered[index] = static_cast<char>(letter + SMALL_LETTER_STEP);
            } else if (letter < 0x80) {
                lowered[index] = static_cast<char>(std::tolower(letter));
            }
        }
        return lowered;
    }

    // The words of a text in small letters
    std::vector<std::string> GetSearchWords(std::string_view text) {
        std::vector<std::string> words;
        const std::string lowered = ToSearchText(text);
        std::size_t start = 0;
        while (start < lowered.size()) {
            const auto end = lowered.find_first_of(" \t\r\n", start);
            if (end != start) words.push_back(lowered.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return words;
    }

    // Whether a name holds every one of the words, anywhere in it
    bool HoldsAllWords(std::string_view name, const std::vector<std::string>& words) {
        const std::string lowered = ToSearchText(name);
        return std::all_of(words.begin(), words.end(), [&](const std::string& word) { return lowered.find(word) != std::string::npos; });
    }

    // Everything in the loaded area around the viewer whose name holds all the words, nearest first, at most limit.
    // Of doors, map markers and things with the same name only the nearest one is kept
    std::vector<Thing> FindAround(RE::PlayerCharacter* player, RE::Actor* viewer, const std::vector<std::string>& words, std::size_t limit) {
        std::vector<Thing> found;
        const auto own = viewer->GetPosition();
        const float radius = static_cast<float>(DEFAULT_PLACE_RADIUS_METERS) * GAME_UNITS_PER_METER;
        const auto keep = [&](std::int32_t kind, RE::FormID referenceID, const RE::NiPoint3& position, std::int32_t flags, std::string name) {
            Thing match;
            match.kind = kind;
            match.referenceID = referenceID;
            match.distance = own.GetDistance(position);
            match.view = GetViewFrom(viewer, position);
            match.flags = flags;
            match.name = std::move(name);
            found.push_back(std::move(match));
        };

        ForEachReferenceAround(viewer, radius, [&](RE::TESObjectREFR* reference) {
            if (!reference || reference == viewer || reference == player || reference->IsDisabled() || reference->IsMarkedForDeletion()) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            const auto* base = reference->GetBaseObject();
            if (!base) return RE::BSContainer::ForEachResult::kContinue;
            const auto* actor = reference->As<RE::Actor>();
            std::int32_t kind = FOUND_IS_THING;
            std::string name;
            if (actor) {
                kind = FOUND_IS_CHARACTER;
                name = CleanText(ViewOf(reference->GetDisplayFullName()));
            } else if (base->Is(RE::FormType::Door)) {
                kind = FOUND_IS_DOOR;
                name = GetNameBehindDoor(reference);
            } else if (GetThingKind(base) >= 0) {
                name = CleanText(ViewOf(reference->GetDisplayFullName()));
            }
            if (name.empty() || !HoldsAllWords(name, words)) return RE::BSContainer::ForEachResult::kContinue;
            keep(kind, reference->GetFormID(), reference->GetPosition(), actor && actor->IsDead() ? FOUND_IS_DEAD : 0, std::move(name));
            return RE::BSContainer::ForEachResult::kContinue;
        });
        for (auto& marker : CollectMapMarkers(player, viewer, radius, MAX_PLACES_TO_SEARCH)) {
            if (!HoldsAllWords(marker.name, words)) continue;
            Thing match;
            match.kind = FOUND_IS_MAP_MARKER;
            match.referenceID = marker.referenceID;
            match.distance = marker.distance;
            match.view = marker.view;
            match.name = std::move(marker.name);
            found.push_back(std::move(match));
        }

        std::sort(found.begin(), found.end(), [](const Thing& left, const Thing& right) { return left.distance < right.distance; });
        std::vector<Thing> kept;
        for (auto& match : found) {
            if (kept.size() >= limit) break;
            const bool isKnown = match.kind != FOUND_IS_CHARACTER && std::any_of(kept.begin(), kept.end(), [&](const Thing& other) {
                return other.kind == match.kind && other.name == match.name;
            });
            if (!isKnown) kept.push_back(std::move(match));
        }
        return kept;
    }

    // ------------------------------------------------------------ surroundings
    // What is around a character, found by rays through the physics of the game: how far it is to the next wall
    // in each direction, whether the ground only rises there, and where the ground drops away.
    // The rays go from the character, so it does not matter where the player or the camera is

    struct RayHit {
        bool isHit = false;
        // From the start of the ray, in game units
        float distance = 0.0f;
        // How much the surface that was hit faces upwards: 1 = flat ground, 0 = an upright wall
        float normalUp = 0.0f;
    };

    // Whether something in this layer of the physics stops a walking character for good: the world itself,
    // not characters, loose things, triggers or the zones the game puts around things
    bool IsSolidLayer(RE::COL_LAYER layer) {
        switch (layer) {
            case RE::COL_LAYER::kUnidentified:
            case RE::COL_LAYER::kStatic:
            case RE::COL_LAYER::kAnimStatic:
            case RE::COL_LAYER::kTransparent:
            case RE::COL_LAYER::kTrees:
            case RE::COL_LAYER::kProps:
            case RE::COL_LAYER::kTerrain:
            case RE::COL_LAYER::kGround:
            case RE::COL_LAYER::kInvisibleWall:
            case RE::COL_LAYER::kStairHelper:
                return true;
            default:
                return false;
        }
    }

    // Sends a ray from one point to another (both in game units) and returns the first solid thing in its way
    RayHit CastRay(RE::bhkWorld* physics, float scale, const RE::NiPoint3& from, const RE::NiPoint3& to) {
        RayHit result;
        const RE::NiPoint3 whole = to - from;
        const float length = whole.Length();
        if (length <= 0.0f) return result;
        const RE::NiPoint3 direction = whole / length;
        float covered = 0.0f;
        for (std::int32_t pass = 0; pass < MAX_RAY_PASSES && covered < length; ++pass) {
            const RE::NiPoint3 start = from + direction * covered;
            RE::bhkPickData pick;
            pick.rayInput.from = RE::hkVector4(start.x * scale, start.y * scale, start.z * scale, 0.0f);
            pick.rayInput.to = RE::hkVector4(to.x * scale, to.y * scale, to.z * scale, 0.0f);
            physics->PickObject(pick);
            if (!pick.rayOutput.HasHit()) return result;
            const float hitDistance = covered + pick.rayOutput.hitFraction * (length - covered);
            if (IsSolidLayer(pick.rayOutput.rootCollidable->GetCollisionLayer())) {
                float normal[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                _mm_storeu_ps(normal, pick.rayOutput.normal.quad);
                result.isHit = true;
                result.distance = hitDistance;
                result.normalUp = normal[2];
                return result;
            }
            covered = hitDistance + RAY_PASS_STEP_UNITS;
        }
        return result;
    }

    std::int32_t ToWholeMeters(float gameUnits) { return static_cast<std::int32_t>(std::lround(gameUnits / GAME_UNITS_PER_METER)); }

    // ---------------------------------------------------------------- hearing
    // The game calls SubtitleManager::ShowSubtitle for every line a character near the player speaks and it would
    // show as a subtitle: lines of a dialogue, and what characters say in passing (the game calls those "general subtitles").
    // The plugin redirects the calls through its own function, which passes the call on and keeps the line.
    // In a test the lines only arrived while both kinds of subtitles were switched on in the settings of the game.
    // The script fetches the kept lines when the companion server asks for the state.

    struct HeardLine {
        RE::FormID speakerID = 0;
        std::string speakerName;
        std::string text;
        std::chrono::steady_clock::time_point time;
    };

    std::mutex heardLock;
    std::deque<HeardLine> heardLines;
    // The lines that were kept last, without their texts being handed out from here. The script empties heardLines
    // whenever it fetches them, and a line the game shows once more must not count as new then
    std::deque<HeardLine> rememberedLines;
    // The choice in the dialogue menu that was kept last, so that it is kept only once
    const void* lastChoice = nullptr;
    std::string lastChoiceText;
    bool isHearing = false;

    std::int64_t SecondsBetween(std::chrono::steady_clock::time_point earlier, std::chrono::steady_clock::time_point later) {
        return std::chrono::duration_cast<std::chrono::seconds>(later - earlier).count();
    }

    void KeepHeardLine(RE::FormID speakerID, std::string_view speakerName, std::string_view rawText) {
        std::string text = CleanText(rawText, MAX_HEARD_TEXT_LENGTH);
        if (text.empty()) return;
        const auto now = std::chrono::steady_clock::now();

        const std::lock_guard<std::mutex> guard(heardLock);
        while (!rememberedLines.empty() && SecondsBetween(rememberedLines.front().time, now) >= SAME_LINE_SECONDS) rememberedLines.pop_front();
        for (const auto& known : rememberedLines) {
            if (known.speakerID == speakerID && known.text == text) return;
        }
        HeardLine line;
        line.speakerID = speakerID;
        line.speakerName = CleanText(speakerName);
        line.text = std::move(text);
        line.time = now;
        rememberedLines.push_back(line);
        while (rememberedLines.size() > MAX_REMEMBERED_LINES) rememberedLines.pop_front();
        heardLines.push_back(std::move(line));
        while (heardLines.size() > MAX_STORED_HEARD_LINES) heardLines.pop_front();
    }

    // Subtitles like "(Heavy breathing)" describe a sound. Nobody said anything there
    bool IsSoundDescription(std::string_view subtitle) {
        const auto first = subtitle.find_first_not_of(" \t\r\n");
        const auto last = subtitle.find_last_not_of(" \t\r\n");
        return first != std::string_view::npos && subtitle[first] == '(' && subtitle[last] == ')';
    }

    // While the dialogue menu is open, the line the player picked last is what the character answers to.
    // It has no subtitle of its own, so it is kept here, once per choice and before the answer
    void KeepPlayerChoice() {
        const auto* topics = RE::MenuTopicManager::GetSingleton();
        if (!topics || !topics->menuOpen) return;
        const auto* choice = topics->lastSelectedDialogue;
        if (!choice) return;
        const auto text = ViewOf(choice->topicText.c_str());
        if (text.empty()) return;
        // When the player addresses a character, nothing has been picked yet. What stands here then is the greeting
        // of the character, and its text is only the internal name of that topic (eg "DialogueGenericHello")
        const auto* topicInfo = choice->parentTopicInfo;
        const auto* topic = IsKnownForm(topicInfo) ? topicInfo->parentTopic : nullptr;
        if (IsKnownForm(topic)) {
            if (topic->data.subtype == RE::DIALOGUE_DATA::Subtype::kHello) return;
            if (EqualsIgnoreCase(ViewOf(topic->GetFormEditorID()), text)) return;
        }
        {
            const std::lock_guard<std::mutex> guard(heardLock);
            if (choice == lastChoice && text == lastChoiceText) return;
            lastChoice = choice;
            lastChoiceText = std::string(text);
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        KeepHeardLine(player->GetFormID(), ViewOf(player->GetDisplayFullName()), text);
    }

    void OnSubtitle(RE::TESObjectREFR* speaker, const char* subtitle) noexcept {
        try {
            if (!speaker || !subtitle || subtitle[0] == '\0' || IsSoundDescription(subtitle)) return;
            KeepPlayerChoice();
            KeepHeardLine(speaker->GetFormID(), ViewOf(speaker->GetDisplayFullName()), subtitle);
        } catch (...) {
            // Whatever goes wrong here must not reach the game
        }
    }

    void ForgetHeardLines() {
        const std::lock_guard<std::mutex> guard(heardLock);
        heardLines.clear();
        rememberedLines.clear();
        lastChoice = nullptr;
        lastChoiceText.clear();
    }

    // say: what the companion really says, told by the script (see SetSayText), for whom, and since when
    std::mutex sayLock;
    RE::FormID saySpeakerID = 0;
    std::string sayText;
    std::chrono::steady_clock::time_point sayTime;

    // The subtitle to show for a line: what the script named if this is the own line of the game mod spoken by the
    // character the script named (then the text is put into shown and that is handed out), otherwise the subtitle itself
    const char* PickShownSubtitle(RE::TESObjectREFR* speaker, const char* subtitle, std::string& shown) noexcept {
        try {
            if (!speaker || !subtitle) return subtitle;
            const auto text = ViewOf(subtitle);
            const auto first = text.find_first_not_of(" \t\r\n");
            const auto last = text.find_last_not_of(" \t\r\n");
            if (first == std::string_view::npos || text.substr(first, last - first + 1) != OWN_LINE_TEXT) return subtitle;
            const std::lock_guard<std::mutex> guard(sayLock);
            if (sayText.empty() || speaker->GetFormID() != saySpeakerID) return subtitle;
            if (SecondsBetween(sayTime, std::chrono::steady_clock::now()) > SAY_TEXT_SECONDS) return subtitle;
            shown = sayText;
            return shown.c_str();
        } catch (...) {
            return subtitle;
        }
    }

    struct SubtitleHook {
        static void Thunk(RE::SubtitleManager* manager, RE::TESObjectREFR* speaker, const char* subtitle, bool alwaysDisplay) {
            std::string shown;
            original(manager, speaker, PickShownSubtitle(speaker, subtitle, shown), alwaysDisplay);
            // What goes to the script stays the line as the game had it, so that the companion does not hear itself
            OnSubtitle(speaker, subtitle);
        }
        static inline REL::Relocation<decltype(Thunk)> original;
    };

    bool IsHearingSwitchedOff() {
        std::ifstream file(SETTINGS_FILE);
        std::string line;
        while (std::getline(file, line)) {
            std::string compact;
            for (const char letter : line) {
                if (!std::isspace(static_cast<unsigned char>(letter))) {
                    compact += static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
                }
            }
            if (compact == SETTING_HEARING_OFF) return true;
        }
        return false;
    }

    // Where the call at this address goes, or 0 if there is no call to a fixed address
    std::uintptr_t GetDirectCallTarget(std::uintptr_t address) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(address);
        if (bytes[0] != DIRECT_CALL_BYTE) return 0;
        std::int32_t distance = 0;
        std::memcpy(&distance, bytes + 1, sizeof(distance));
        return address + DIRECT_CALL_LENGTH + distance;
    }

    // All places in the code of the game that call ShowSubtitle (or jump to it) at a fixed address.
    // The two places mantella-vanilla-dialogue knows only see the lines of dialogues. What characters say in passing
    // reaches ShowSubtitle from elsewhere, so the code of the game is searched for every call that goes there.
    // A call is five bytes: E8 (or E9 for a jump) and the distance to the target. Five bytes that only look like
    // such a call by chance would have to hit this one address among four billion, so that does not happen in practice
    std::vector<std::uintptr_t> FindCallsTo(std::uintptr_t target) {
        std::vector<std::uintptr_t> calls;
        const auto code = REL::Module::get().segment(REL::Segment::textx);
        const std::uintptr_t start = code.address();
        const std::size_t size = code.size();
        if (start == 0 || size < DIRECT_CALL_LENGTH) return calls;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(start);
        for (std::size_t place = 0; place + DIRECT_CALL_LENGTH <= size; ++place) {
            if (bytes[place] != DIRECT_CALL_BYTE && bytes[place] != DIRECT_JUMP_BYTE) continue;
            std::int32_t distance = 0;
            std::memcpy(&distance, bytes + place + 1, sizeof(distance));
            if (start + place + DIRECT_CALL_LENGTH + distance != target) continue;
            const std::uintptr_t site = start + place;
            if (site >= target && site < target + SUBTITLE_FUNCTION_REACH) continue;
            calls.push_back(site);
        }
        return calls;
    }

    // The two calls of ShowSubtitle that are known for Skyrim SE and AE, if both really are calls that go there.
    // Otherwise (another version of the game, or another mod was there first) none
    std::vector<std::uintptr_t> GetKnownSubtitleCalls(std::uintptr_t showSubtitle) {
        const std::ptrdiff_t actorOffset = REL::Module::IsAE() ? ACTOR_SOUND_CALL_OFFSET_AE : ACTOR_SOUND_CALL_OFFSET_SE;
        REL::Relocation<std::uintptr_t> referenceSite(RELOCATION_ID(REFERENCE_SOUND_ID_SE, REFERENCE_SOUND_ID_AE), REFERENCE_SOUND_CALL_OFFSET);
        REL::Relocation<std::uintptr_t> actorSite(RELOCATION_ID(ACTOR_SOUND_ID_SE, ACTOR_SOUND_ID_AE), actorOffset);
        const std::vector<std::uintptr_t> sites{ referenceSite.address(), actorSite.address() };
        for (const auto site : sites) {
            const auto target = GetDirectCallTarget(site);
            if (target != showSubtitle) {
                logger::error("No hearing: the call at {:X} goes to {:X} instead of ShowSubtitle at {:X}.", site, target, showSubtitle);
                return {};
            }
        }
        return sites;
    }

    // Redirects the calls of ShowSubtitle through SubtitleHook::Thunk: every call found in the code of the game,
    // or only the two known ones if that search finds nothing believable. If neither works, nothing is changed
    // and the plugin only does without hearing
    bool InstallSubtitleHook() {
        if (IsHearingSwitchedOff()) {
            logger::info("Hearing is switched off in {}.", SETTINGS_FILE);
            return false;
        }
        REL::Relocation<std::uintptr_t> showSubtitle(RELOCATION_ID(SHOW_SUBTITLE_ID_SE, SHOW_SUBTITLE_ID_AE));
        auto sites = FindCallsTo(showSubtitle.address());
        if (sites.empty() || sites.size() > MAX_SUBTITLE_CALLS) {
            logger::warn("Hearing: the search found {} calls of ShowSubtitle, which is not believable. Using the two known ones.", sites.size());
            sites = GetKnownSubtitleCalls(showSubtitle.address());
            if (sites.empty()) return false;
        }

        auto& trampoline = SKSE::GetTrampoline();
        const std::size_t neededBytes = sites.size() * TRAMPOLINE_BYTES_PER_CALL;
        // Older versions of SKSE (as the one for Skyrim VR) hand out no memory for this, so the plugin gets its own
        if (trampoline.empty()) trampoline.create(neededBytes + TRAMPOLINE_SPARE_BYTES);
        if (trampoline.free_size() < neededBytes) {
            logger::error("No hearing: {} bytes are needed to redirect {} calls, only {} are free. Nothing was changed.", neededBytes,
                          sites.size(), trampoline.free_size());
            return false;
        }
        for (const auto site : sites) {
            const bool isJump = *reinterpret_cast<const std::uint8_t*>(site) == DIRECT_JUMP_BYTE;
            // Every one of them went to ShowSubtitle, so what comes back is the same address each time
            if (isJump) {
                SubtitleHook::original = trampoline.write_branch<DIRECT_CALL_LENGTH>(site, SubtitleHook::Thunk);
            } else {
                SubtitleHook::original = trampoline.write_call<DIRECT_CALL_LENGTH>(site, SubtitleHook::Thunk);
            }
            logger::info("Hearing: redirected the {} at {:X} (offset {:X}).", isJump ? "jump" : "call", site, site - REL::Module::get().base());
        }
        logger::info("Hearing: hooked into {} calls of ShowSubtitle.", sites.size());
        return true;
    }

    void OnMessage(SKSE::MessagingInterface::Message* message) {
        if (!message) return;
        // What was said before a save is loaded or a new game starts belongs to another game
        if (message->type == SKSE::MessagingInterface::kPreLoadGame || message->type == SKSE::MessagingInterface::kNewGame) {
            ForgetHeardLines();
        }
    }

    // ----------------------------------------- functions for the Papyrus script

    std::int32_t GetInterfaceVersion(RE::StaticFunctionTag*) { return INTERFACE_VERSION; }

    // The name of the voice of a character as the files of the game know it: the editor ID of its voice type
    // (eg FemaleEvenToned). The game looks for the sound files of what a character says in a folder of that name,
    // so the companion server needs it to put a spoken line where the game finds it. Empty if there is none
    RE::BSFixedString GetVoiceTypeName(RE::StaticFunctionTag*, RE::Actor* actor) {
        const auto* base = actor ? actor->GetActorBase() : nullptr;
        const auto* voice = base ? base->voiceType : nullptr;
        const char* name = voice ? voice->GetFormEditorID() : nullptr;
        return name ? RE::BSFixedString(name) : RE::BSFixedString();
    }

    // The door on the other side of a door that leads into another area (a building, a cave, out of town),
    // or None if this is no such door
    RE::TESObjectREFR* GetDoorDestination(RE::StaticFunctionTag*, RE::TESObjectREFR* door) {
        const auto* teleport = door ? door->extraList.GetByType<RE::ExtraTeleport>() : nullptr;
        const auto* link = teleport ? teleport->teleportData : nullptr;
        if (!link) return nullptr;
        return link->linkedDoor.get().get();
    }

    // Where someone stands after going through such a door, measured from the door on the other side
    // (GetDoorDestination): how far to the east, to the north and upwards in game units, and the direction
    // faced in degrees (0 = north, 90 = east). Empty if this is no such door
    std::vector<float> GetDoorArrival(RE::StaticFunctionTag*, RE::TESObjectREFR* door) {
        const auto* teleport = door ? door->extraList.GetByType<RE::ExtraTeleport>() : nullptr;
        const auto* link = teleport ? teleport->teleportData : nullptr;
        const auto otherDoor = link ? link->linkedDoor.get() : RE::NiPointer<RE::TESObjectREFR>();
        if (!otherDoor) return {};
        const auto offset = link->position - otherDoor->GetPosition();
        return { offset.x, offset.y, offset.z, link->rotation.z * DEGREES_PER_RADIAN };
    }

    // The characters in the building the character "origin" is in while the player is somewhere else
    // (see IsInUnloadedInterior), nearest first, at most maxCharacters. These are only the ones the game keeps
    // in memory for a place that is not loaded: as a rule the named characters who live there, not bandits and animals.
    // One text per character, its parts separated by tabs: ref ID as eight hex digits, distance to origin in meters,
    // angle from where origin faces in degrees (negative = left), dead (1 or 0), name.
    // Empty if origin is not in such a place
    std::vector<std::string> GetCharactersInCell(RE::StaticFunctionTag*, RE::Actor* origin, std::int32_t maxCharacters) {
        std::vector<std::string> lines;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !IsInUnloadedInterior(origin, player)) return lines;
        const std::size_t limit = maxCharacters > 0 ? static_cast<std::size_t>(maxCharacters) : DEFAULT_MAX_CHARACTERS;

        struct Character {
            float distance = 0.0f;
            std::string line;
        };
        std::vector<Character> characters;
        const auto own = origin->GetPosition();
        GetCellOf(origin)->ForEachReference([&](RE::TESObjectREFR* reference) {
            if (!reference || reference == origin || reference == player || reference->IsDisabled() || reference->IsMarkedForDeletion()) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            const auto* actor = reference->As<RE::Actor>();
            if (!actor) return RE::BSContainer::ForEachResult::kContinue;
            std::string name = CleanText(ViewOf(reference->GetDisplayFullName()));
            if (name.empty()) return RE::BSContainer::ForEachResult::kContinue;

            Character character;
            character.distance = own.GetDistance(reference->GetPosition());
            const auto view = GetViewFrom(origin, reference->GetPosition());
            character.line = std::format("{:08X}", reference->GetFormID());
            character.line += FIELD_SEPARATOR;
            character.line += std::to_string(view.distanceMeters);
            character.line += FIELD_SEPARATOR;
            character.line += std::to_string(view.angleDegrees);
            character.line += FIELD_SEPARATOR;
            character.line += actor->IsDead() ? "1" : "0";
            character.line += FIELD_SEPARATOR;
            character.line += name;
            characters.push_back(std::move(character));
            return RE::BSContainer::ForEachResult::kContinue;
        });
        std::sort(characters.begin(), characters.end(), [](const Character& left, const Character& right) { return left.distance < right.distance; });
        for (auto& character : characters) {
            if (lines.size() >= limit) break;
            lines.push_back(std::move(character.line));
        }
        logger::debug("GetCharactersInCell: {} characters sent", lines.size());
        return lines;
    }

    // Whether the plugin could hook into the subtitles of the game (see InstallSubtitleHook)
    bool IsHearing(RE::StaticFunctionTag*) { return isHearing; }

    // The lines that were heard since the last call, oldest first, at most maxLines (then the newest ones).
    // One text per line, its parts separated by tabs:
    // how many seconds ago, ref ID of the speaker as eight hex digits, name of the speaker, what was said.
    // A line the player picked in a dialogue menu has the player as its speaker
    std::vector<std::string> TakeHeardLines(RE::StaticFunctionTag*, std::int32_t maxLines) {
        const std::size_t limit = maxLines > 0 ? static_cast<std::size_t>(maxLines) : DEFAULT_MAX_HEARD_LINES;
        const auto now = std::chrono::steady_clock::now();

        std::deque<HeardLine> taken;
        {
            const std::lock_guard<std::mutex> guard(heardLock);
            taken.swap(heardLines);
        }
        while (taken.size() > limit) taken.pop_front();

        std::vector<std::string> lines;
        for (const auto& heard : taken) {
            const auto age = SecondsBetween(heard.time, now);
            if (age > MAX_HEARD_AGE_SECONDS) continue;
            std::string line = std::to_string(age);
            line += FIELD_SEPARATOR;
            line += std::format("{:08X}", heard.speakerID);
            line += FIELD_SEPARATOR;
            line += heard.speakerName;
            line += FIELD_SEPARATOR;
            line += heard.text;
            lines.push_back(std::move(line));
        }
        return lines;
    }

    // One text per quest, the parts separated by tabs:
    // tracked in the journal (1 or 0), type of the quest as the game numbers it (6 = miscellaneous), name of the quest,
    // then the text of each open objective.
    // The companion server takes these lines apart again (src/companion/game_state.py in the Mantella fork)
    std::vector<std::string> GetOpenQuests(RE::StaticFunctionTag*, std::int32_t maxQuests) {
        const auto quests = CollectOpenQuests();
        const std::size_t limit = maxQuests > 0 ? static_cast<std::size_t>(maxQuests) : DEFAULT_MAX_QUESTS;

        std::vector<std::string> lines;
        for (const auto& entry : quests) {
            if (lines.size() >= limit) break;
            std::string line = entry.quest->IsActive() ? "1" : "0";
            line += FIELD_SEPARATOR;
            line += std::to_string(static_cast<std::int32_t>(entry.quest->GetType()));
            line += FIELD_SEPARATOR;
            line += CleanText(ViewOf(entry.quest->GetName()));
            for (const auto& objective : entry.objectives) {
                line += FIELD_SEPARATOR;
                line += objective;
            }
            lines.push_back(std::move(line));
        }
        logger::debug("GetOpenQuests: {} open quests found, {} sent", quests.size(), lines.size());
        return lines;
    }

    // From whom the lists for the script are seen: the character the script names if it stands in the loaded area
    // or inside a building the player is not in. Otherwise (none named, or one somewhere far away outdoors) the player
    RE::Actor* GetViewer(RE::Actor* origin, RE::PlayerCharacter* player) {
        if (!origin || origin->IsDisabled()) return player;
        if (origin->Is3DLoaded() || IsInUnloadedInterior(origin, player)) return origin;
        return player;
    }

    // Whether the plugin can ask the game about the conditions of quest targets. Without that, GetQuestTargets
    // hands out every target of an objective, also those the game itself would not show at the moment
    bool canCheckTargetConditions = false;

    // Where the targets of the tracked quests are, at most maxTargets. One text per target, its parts separated by tabs:
    // place of the quest in the list GetOpenQuests(maxQuests) returns (from 0), place of the objective in that quest (from 0),
    // where the target is (0 = in the area of the viewer, 1 = behind a door of this area, 2 = somewhere else),
    // distance to the viewer in meters and angle from where the viewer faces in degrees (negative = left) - for 1 these are
    // the distance and angle of that door, for 2 both are 0 -, name of the target (can be empty),
    // for 1 and 2 the name of the place behind the door or of the place the target is in (can be empty),
    // and the ref ID to walk to as eight hex digits: the target for 0, that door for 1, 00000000 for 2.
    // The viewer is origin if that character stands in the loaded area, otherwise the player (see GetViewer)
    std::vector<std::string> GetQuestTargets(RE::StaticFunctionTag*, std::int32_t maxQuests, std::int32_t maxTargets, RE::Actor* origin) {
        std::vector<std::string> lines;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return lines;
        auto* viewer = GetViewer(origin, player);
        const auto quests = CollectOpenQuests();
        const std::size_t questLimit = maxQuests > 0 ? static_cast<std::size_t>(maxQuests) : DEFAULT_MAX_QUESTS;
        const std::size_t targetLimit = maxTargets > 0 ? static_cast<std::size_t>(maxTargets) : DEFAULT_MAX_QUEST_TARGETS;

        for (std::size_t questIndex = 0; questIndex < quests.size() && questIndex < questLimit; ++questIndex) {
            const auto& entry = quests[questIndex];
            // The compass of the game only shows the targets of tracked quests, and so does this list
            if (!entry.quest->IsActive()) continue;
            for (std::size_t objectiveIndex = 0; objectiveIndex < entry.objectiveForms.size(); ++objectiveIndex) {
                const auto targets = CollectTargets(entry.quest, entry.objectiveForms[objectiveIndex], player, viewer, canCheckTargetConditions);
                for (const auto& target : targets) {
                    if (lines.size() >= targetLimit) return lines;
                    std::string line = std::to_string(questIndex);
                    line += FIELD_SEPARATOR;
                    line += std::to_string(objectiveIndex);
                    line += FIELD_SEPARATOR;
                    line += std::to_string(target.kind);
                    line += FIELD_SEPARATOR;
                    line += std::to_string(target.view.distanceMeters);
                    line += FIELD_SEPARATOR;
                    line += std::to_string(target.view.angleDegrees);
                    line += FIELD_SEPARATOR;
                    line += target.name;
                    line += FIELD_SEPARATOR;
                    line += target.place;
                    line += FIELD_SEPARATOR;
                    line += std::format("{:08X}", target.referenceID);
                    lines.push_back(std::move(line));
                }
            }
        }
        logger::debug("GetQuestTargets: {} targets sent", lines.size());
        return lines;
    }

    // The places around the viewer (origin, or the player - see GetViewer), map markers first, then doors, each nearest first:
    // at most maxMarkers map markers within radiusMeters (none indoors), and at most maxDoors doors in the loaded area
    // within radiusMeters that lead into another area. One text per place, its parts separated by tabs:
    // kind (0 = map marker, 1 = door), ref ID of the marker or the door as eight hex digits,
    // distance to the viewer in meters, angle from where the viewer faces in degrees
    // (negative = left), for a map marker the kind of place as the game numbers it (13 = farm, 21 = stable ...; 0 for a door),
    // for a map marker 1 if the player has been there (0 otherwise, and for a door),
    // name of the place, for a door the name of the place it leads to
    std::vector<std::string> GetNearbyPlaces(RE::StaticFunctionTag*, std::int32_t radiusMeters, std::int32_t maxMarkers, std::int32_t maxDoors,
                                             RE::Actor* origin) {
        std::vector<std::string> lines;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return lines;
        auto* viewer = GetViewer(origin, player);
        const float radius = static_cast<float>(radiusMeters > 0 ? radiusMeters : DEFAULT_PLACE_RADIUS_METERS) * GAME_UNITS_PER_METER;
        // A limit of 0 switches that kind of place off, a negative one means: the usual number
        const std::size_t markerLimit = maxMarkers >= 0 ? static_cast<std::size_t>(maxMarkers) : DEFAULT_MAX_MAP_MARKERS;
        const std::size_t doorLimit = maxDoors >= 0 ? static_cast<std::size_t>(maxDoors) : DEFAULT_MAX_DOORS;

        std::vector<Place> places;
        if (markerLimit > 0) places = CollectMapMarkers(player, viewer, radius, markerLimit);
        if (doorLimit > 0) {
            auto doors = CollectDoors(viewer, radius, doorLimit);
            for (auto& door : doors) places.push_back(std::move(door));
        }
        for (const auto& place : places) {
            std::string line = std::to_string(place.kind);
            line += FIELD_SEPARATOR;
            line += std::format("{:08X}", place.referenceID);
            line += FIELD_SEPARATOR;
            line += std::to_string(place.view.distanceMeters);
            line += FIELD_SEPARATOR;
            line += std::to_string(place.view.angleDegrees);
            line += FIELD_SEPARATOR;
            line += std::to_string(place.type);
            line += FIELD_SEPARATOR;
            line += std::to_string(place.flags);
            line += FIELD_SEPARATOR;
            line += place.name;
            lines.push_back(std::move(line));
        }
        logger::debug("GetNearbyPlaces: {} places sent", lines.size());
        return lines;
    }

    // The things close to the viewer (origin, or the player - see GetViewer): furniture first, then containers,
    // then items that lie around, each nearest first, at most maxPerKind of each kind within radiusMeters.
    // One text per thing, its parts separated by tabs:
    // kind (0 = furniture, 1 = container, 2 = item), ref ID as eight hex digits, distance to the viewer in meters,
    // angle from where the viewer faces in degrees (negative = left),
    // for furniture 1 if a character can sit on it, for a container 1 if it is locked (0 otherwise), name
    std::vector<std::string> GetNearbyThings(RE::StaticFunctionTag*, std::int32_t radiusMeters, std::int32_t maxPerKind, RE::Actor* origin) {
        std::vector<std::string> lines;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return lines;
        auto* viewer = GetViewer(origin, player);
        const float radius = static_cast<float>(radiusMeters > 0 ? radiusMeters : DEFAULT_THING_RADIUS_METERS) * GAME_UNITS_PER_METER;
        const std::size_t limit = maxPerKind > 0 ? static_cast<std::size_t>(maxPerKind) : DEFAULT_MAX_THINGS_PER_KIND;

        for (const auto& thing : CollectThings(viewer, radius, limit)) lines.push_back(GetThingLine(thing));
        logger::debug("GetNearbyThings: {} things sent", lines.size());
        return lines;
    }

    // The traps close to the viewer (origin, or the player - see GetViewer), nearest first, at most maxTraps within
    // radiusMeters. One text per trap, built like the lines of GetNearbyThings: kind (always 3), ref ID, distance,
    // angle, 0, and as the name the kind of trap as the game files it (tripwire, pressureplate, beartrap, macetrap ...)
    std::vector<std::string> GetNearbyTraps(RE::StaticFunctionTag*, std::int32_t radiusMeters, std::int32_t maxTraps, RE::Actor* origin) {
        std::vector<std::string> lines;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return lines;
        auto* viewer = GetViewer(origin, player);
        const float radius = static_cast<float>(radiusMeters > 0 ? radiusMeters : DEFAULT_TRAP_RADIUS_METERS) * GAME_UNITS_PER_METER;
        const std::size_t limit = maxTraps > 0 ? static_cast<std::size_t>(maxTraps) : DEFAULT_MAX_TRAPS;
        for (const auto& trap : CollectTraps(viewer, radius, limit)) lines.push_back(GetThingLine(trap));
        return lines;
    }

    // The characters in the loaded area around the viewer (origin, or the player - see GetViewer) within radiusMeters,
    // at most maxCharacters: first the living that are hostile to the player, then the other living, then the dead,
    // each nearest first. The player and the viewer are left out. People and animals alike.
    // For the list "Nearby" outdoors: the script itself only reaches the characters of the one cell the viewer stands in,
    // so archers on a tower or game a hundred meters away were missing. Empty if the viewer is not in the loaded area
    std::vector<RE::Actor*> GetCharactersAround(RE::StaticFunctionTag*, RE::Actor* origin, std::int32_t radiusMeters, std::int32_t maxCharacters) {
        std::vector<RE::Actor*> characters;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || radiusMeters <= 0 || maxCharacters <= 0) return characters;
        auto* viewer = GetViewer(origin, player);
        if (!viewer->Is3DLoaded()) return characters;

        struct SeenCharacter {
            RE::Actor* actor = nullptr;
            float distance = 0.0f;
            std::int32_t rank = 0;
        };
        constexpr std::int32_t RANK_HOSTILE = 0;
        constexpr std::int32_t RANK_LIVING = 1;
        constexpr std::int32_t RANK_DEAD = 2;
        // Without the function of the game that tells hostility, the living simply come nearest first
        const bool canTellHostile = HasGameAddress(HOSTILE_TO_ACTOR_ID);
        std::vector<SeenCharacter> seen;
        const auto own = viewer->GetPosition();
        const float radius = static_cast<float>(radiusMeters) * GAME_UNITS_PER_METER;
        ForEachReferenceAround(viewer, radius, [&](RE::TESObjectREFR* reference) {
            if (!reference || reference == viewer || reference == player || reference->IsDisabled() || reference->IsMarkedForDeletion()) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            auto* actor = reference->As<RE::Actor>();
            // A character without a body is not in the world right now (eg it waits to be placed)
            if (!actor || !actor->Is3DLoaded()) return RE::BSContainer::ForEachResult::kContinue;
            SeenCharacter character;
            character.actor = actor;
            character.distance = own.GetDistance(actor->GetPosition());
            if (actor->IsDead()) {
                character.rank = RANK_DEAD;
            } else if (canTellHostile && actor->IsHostileToActor(player)) {
                character.rank = RANK_HOSTILE;
            } else {
                character.rank = RANK_LIVING;
            }
            seen.push_back(character);
            return RE::BSContainer::ForEachResult::kContinue;
        });
        std::sort(seen.begin(), seen.end(), [](const SeenCharacter& left, const SeenCharacter& right) {
            if (left.rank != right.rank) return left.rank < right.rank;
            return left.distance < right.distance;
        });
        const std::size_t limit = static_cast<std::size_t>(maxCharacters);
        for (const auto& character : seen) {
            if (characters.size() >= limit) break;
            characters.push_back(character.actor);
        }
        logger::debug("GetCharactersAround: {} of {} characters within {} meters sent", characters.size(), seen.size(), radiusMeters);
        return characters;
    }

    // What in the loaded area around the viewer (origin, or the player - see GetViewer) has a name that holds every word
    // of the text, nearest first, at most maxResults. Capitals do not matter. One text per match, built like the lines
    // of GetNearbyThings: kind (0 = a character, 1 = a door, 2 = a map marker, 3 = a thing), ref ID, distance in meters,
    // angle in degrees (negative = left), for a character 1 if it is dead (0 otherwise), name (for a door: of the place it leads to)
    std::vector<std::string> FindByName(RE::StaticFunctionTag*, RE::BSFixedString text, std::int32_t maxResults, RE::Actor* origin) {
        std::vector<std::string> lines;
        auto* player = RE::PlayerCharacter::GetSingleton();
        const auto words = GetSearchWords(ViewOf(text.c_str()));
        if (!player || words.empty()) return lines;
        auto* viewer = GetViewer(origin, player);
        const std::size_t limit = maxResults > 0 ? static_cast<std::size_t>(maxResults) : DEFAULT_MAX_FOUND;
        for (const auto& match : FindAround(player, viewer, words, limit)) lines.push_back(GetThingLine(match));
        logger::debug("FindByName: {} matches sent", lines.size());
        return lines;
    }

    // The reference with this ID (given in its upper and lower 16 bits, as the companion server sends IDs), or nullptr
    RE::TESObjectREFR* FindReference(std::int32_t highHalf, std::int32_t lowHalf) {
        const RE::FormID wantedID = (static_cast<RE::FormID>(highHalf & 0xFFFF) << 16) | static_cast<RE::FormID>(lowHalf & 0xFFFF);
        auto* form = wantedID != 0 ? RE::TESForm::LookupByID(wantedID) : nullptr;
        return form ? form->AsReference() : nullptr;
    }

    // The name of the ammunition if the reference with this ID is an arrow that was shot and has come to rest,
    // otherwise an empty text. Such an arrow is no item for the game, so scripts cannot take it like one
    std::string GetRestingArrowName(RE::StaticFunctionTag*, std::int32_t highHalf, std::int32_t lowHalf) {
        const auto* ammo = GetRestingArrowAmmo(FindReference(highHalf, lowHalf));
        return ammo ? CleanText(ViewOf(ammo->GetName())) : std::string();
    }

    // Gives the taker the ammunition of an arrow that has come to rest and takes the arrow out of the world.
    // Returns the name of the ammunition, or an empty text if there is no such arrow (any more).
    // The game does the giving and taking a moment later, on its own thread
    std::string TakeRestingArrow(RE::StaticFunctionTag*, RE::Actor* taker, std::int32_t highHalf, std::int32_t lowHalf) {
        auto* arrow = FindReference(highHalf, lowHalf);
        auto* ammo = GetRestingArrowAmmo(arrow);
        const auto* tasks = SKSE::GetTaskInterface();
        if (!taker || !ammo || !tasks || !HasGameAddress(DISABLE_REFERENCE_ID)) return std::string();
        const RE::FormID arrowID = arrow->GetFormID();
        const RE::FormID takerID = taker->GetFormID();
        tasks->AddTask([arrowID, takerID]() {
            auto* takerNow = RE::TESForm::LookupByID<RE::Actor>(takerID);
            auto* arrowForm = RE::TESForm::LookupByID(arrowID);
            auto* arrowNow = arrowForm ? arrowForm->AsReference() : nullptr;
            // Someone else may have taken the arrow in the meantime
            auto* ammoNow = GetRestingArrowAmmo(arrowNow);
            if (!takerNow || !ammoNow) return;
            takerNow->AddObjectToContainer(ammoNow, nullptr, 1, nullptr);
            arrowNow->Disable();
        });
        return CleanText(ViewOf(ammo->GetName()));
    }

    // say: tells what the speaker really says with its next line, so that the subtitle of the own line of the game mod
    // shows that instead of "...". An empty text ends it. Returns whether subtitles can be changed at all
    bool SetSayText(RE::StaticFunctionTag*, RE::Actor* speaker, RE::BSFixedString text) {
        const std::lock_guard<std::mutex> guard(sayLock);
        saySpeakerID = speaker ? speaker->GetFormID() : 0;
        sayText = speaker ? CleanText(ViewOf(text.c_str()), MAX_HEARD_TEXT_LENGTH) : std::string();
        sayTime = std::chrono::steady_clock::now();
        return isHearing;
    }

    // What is around the character within rangeMeters, by rays (see CastRay). SURROUNDING_DIRECTIONS directions,
    // the first straight ahead of the character, the others clockwise from there, three numbers each:
    // what the ray met (0 = nothing, 1 = a wall or another upright obstacle, 2 = ground that rises),
    // how far away in meters (0 if nothing), and after how many meters the ground drops away (-1 = it does not,
    // as far as the ray got). An empty list if the game cannot tell (the character is not in the loaded area)
    std::vector<std::int32_t> GetSurroundings(RE::StaticFunctionTag*, RE::Actor* origin, std::int32_t rangeMeters) {
        std::vector<std::int32_t> values;
        if (!origin || !origin->Is3DLoaded()) return values;
        if (!HasGameAddress(CELL_PHYSICS_ID) || !HasGameAddress(PHYSICS_LOCK_ID) || !HasGameAddress(PHYSICS_UNLOCK_ID)) return values;
        const auto* cell = origin->GetParentCell();
        auto* physics = cell ? cell->GetbhkWorld() : nullptr;
        if (!physics) return values;
        const float scale = HasGameAddress(PHYSICS_SCALE_ID) ? RE::bhkWorld::GetWorldScale() : DEFAULT_PHYSICS_SCALE;
        const std::int32_t meters = rangeMeters > 0 ? std::min(rangeMeters, MAX_SURROUNDINGS_METERS) : DEFAULT_SURROUNDINGS_METERS;
        const float range = static_cast<float>(meters) * GAME_UNITS_PER_METER;
        const RE::NiPoint3 feet = origin->GetPosition();
        const RE::NiPoint3 chest(feet.x, feet.y, feet.z + RAY_HEIGHT_UNITS);
        const float facing = origin->GetAngleZ();

        const RE::BSReadLockGuard guard(physics->worldLock);
        for (std::int32_t index = 0; index < SURROUNDING_DIRECTIONS; ++index) {
            // The game counts directions clockwise from north, and north is where y grows
            const float heading = facing + static_cast<float>(index) * 2.0f * std::numbers::pi_v<float> / static_cast<float>(SURROUNDING_DIRECTIONS);
            const RE::NiPoint3 step(std::sin(heading), std::cos(heading), 0.0f);
            const RayHit ahead = CastRay(physics, scale, chest + step * RAY_START_UNITS, chest + step * range);
            std::int32_t obstacle = OBSTACLE_NONE;
            float reach = range;
            if (ahead.isHit) {
                obstacle = ahead.normalUp >= WALKABLE_NORMAL_UP ? OBSTACLE_SLOPE : OBSTACLE_WALL;
                reach = RAY_START_UNITS + ahead.distance;
            }
            // Along the free stretch: does the ground fall away somewhere?
            std::int32_t dropMeters = NO_DROP;
            float groundBefore = feet.z;
            for (float along = DROP_STEP_UNITS; along < reach; along += DROP_STEP_UNITS) {
                const RE::NiPoint3 top = chest + step * along;
                const RayHit below = CastRay(physics, scale, top, RE::NiPoint3(top.x, top.y, top.z - DROP_PROBE_UNITS));
                const float ground = below.isHit ? top.z - below.distance : top.z - DROP_PROBE_UNITS;
                if (!below.isHit || groundBefore - ground > DROP_HEIGHT_UNITS) {
                    dropMeters = ToWholeMeters(along);
                    break;
                }
                groundBefore = ground;
            }
            values.push_back(obstacle);
            values.push_back(obstacle == OBSTACLE_NONE ? 0 : ToWholeMeters(reach));
            values.push_back(dropMeters);
        }
        return values;
    }

    bool Bind(RE::BSScript::IVirtualMachine* vm) {
        vm->RegisterFunction("GetInterfaceVersion", SCRIPT_NAME, GetInterfaceVersion);
        vm->RegisterFunction("GetOpenQuests", SCRIPT_NAME, GetOpenQuests);
        vm->RegisterFunction("IsHearing", SCRIPT_NAME, IsHearing);
        vm->RegisterFunction("TakeHeardLines", SCRIPT_NAME, TakeHeardLines);
        vm->RegisterFunction("GetQuestTargets", SCRIPT_NAME, GetQuestTargets);
        vm->RegisterFunction("GetNearbyPlaces", SCRIPT_NAME, GetNearbyPlaces);
        vm->RegisterFunction("GetNearbyThings", SCRIPT_NAME, GetNearbyThings);
        vm->RegisterFunction("GetVoiceTypeName", SCRIPT_NAME, GetVoiceTypeName);
        vm->RegisterFunction("GetDoorDestination", SCRIPT_NAME, GetDoorDestination);
        vm->RegisterFunction("GetDoorArrival", SCRIPT_NAME, GetDoorArrival);
        vm->RegisterFunction("GetCharactersInCell", SCRIPT_NAME, GetCharactersInCell);
        vm->RegisterFunction("GetNearbyTraps", SCRIPT_NAME, GetNearbyTraps);
        vm->RegisterFunction("GetRestingArrowName", SCRIPT_NAME, GetRestingArrowName);
        vm->RegisterFunction("TakeRestingArrow", SCRIPT_NAME, TakeRestingArrow);
        vm->RegisterFunction("SetSayText", SCRIPT_NAME, SetSayText);
        vm->RegisterFunction("GetSurroundings", SCRIPT_NAME, GetSurroundings);
        vm->RegisterFunction("FindByName", SCRIPT_NAME, FindByName);
        vm->RegisterFunction("GetCharactersAround", SCRIPT_NAME, GetCharactersAround);
        return true;
    }

}  // namespace Companion

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    SetupLog();
    SKSE::GetPapyrusInterface()->Register(Companion::Bind);
    logger::info("Loaded. Functions for the script {} registered, interface version {}.", Companion::SCRIPT_NAME,
                 Companion::INTERFACE_VERSION);
    Companion::isHearing = Companion::InstallSubtitleHook();
    Companion::canCheckTargetConditions = Companion::HasGameAddress(Companion::CONDITION_IS_TRUE_ID);
    if (!Companion::canCheckTargetConditions) {
        logger::warn("The Address Library has no ID {}: quest targets are handed out without checking their conditions.",
                     Companion::CONDITION_IS_TRUE_ID);
    }
    if (!Companion::HasGameAddress(Companion::WORLD_SINGLETON_ID)) {
        logger::warn("The Address Library has no ID {}: no doors among the places and no things around.", Companion::WORLD_SINGLETON_ID);
    }
    for (const auto wantedID : {Companion::CELL_PHYSICS_ID, Companion::PHYSICS_LOCK_ID, Companion::PHYSICS_UNLOCK_ID}) {
        if (!Companion::HasGameAddress(wantedID)) logger::warn("The Address Library has no ID {}: no rays to tell the surroundings.", wantedID);
    }
    if (!Companion::HasGameAddress(Companion::DISABLE_REFERENCE_ID)) {
        logger::warn("The Address Library has no ID {}: arrows that were shot cannot be picked up.", Companion::DISABLE_REFERENCE_ID);
    }
    if (!Companion::HasGameAddress(Companion::HOSTILE_TO_ACTOR_ID)) {
        logger::warn("The Address Library has no ID {}: the characters around come nearest first, not hostile ones first.", Companion::HOSTILE_TO_ACTOR_ID);
    }
    if (const auto* messaging = SKSE::GetMessagingInterface()) {
        messaging->RegisterListener("SKSE", Companion::OnMessage);
    }
    return true;
}
