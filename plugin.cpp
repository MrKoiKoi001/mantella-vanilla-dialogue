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
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <numbers>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "PCH.h"
#include "logger.h"

// Windows.h (comes in with logger.h) renames GetObject to GetObjectA. That breaks
// RE::BGSDefaultObjectManager::GetObject, which the PCH read under its real name
#ifdef GetObject
#undef GetObject
#endif

namespace Companion {

    // Goes up whenever the functions for the script change. The script passes it on to the companion server
    constexpr std::int32_t INTERFACE_VERSION = 41;
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
    // A character can lie down and sleep on it (see the command sleep)
    constexpr std::int32_t FURNITURE_IS_BED = 2;
    constexpr std::int32_t CONTAINER_IS_LOCKED = 1;
    // The flag of a container that is not locked: this times (the number of different items in it + 1),
    // so that an empty container (2) can be told from one of an older plugin that does not count (0)
    constexpr std::int32_t CONTAINER_COUNT_STEP = 2;
    constexpr std::int32_t LOOT_UNKNOWN = -1;
    // Inventories (see GetInventoryLines): what to list, the flags of an item, and the numbers for the kinds of armor.
    // The same numbers as in KI_MCP_Gefaehrte_Loop.psc of the game mod and game_state.py of the companion server
    constexpr std::int32_t INVENTORY_FAVORITES = 1;
    constexpr std::int32_t INVENTORY_EQUIPPED = 2;
    constexpr std::size_t DEFAULT_MAX_INVENTORY_ITEMS = 120;
    constexpr std::int32_t ITEM_IS_EQUIPPED = 1;
    constexpr std::int32_t ITEM_IS_FAVORITE = 2;
    constexpr std::int32_t ITEM_KIND_STEP = 16;
    constexpr std::int32_t ITEM_KIND_SHIELD = 20;
    constexpr std::int32_t ITEM_KIND_BODY = 21;
    constexpr std::int32_t ITEM_KIND_HEAD = 22;
    constexpr std::int32_t ITEM_KIND_HANDS = 23;
    constexpr std::int32_t ITEM_KIND_FEET = 24;
    constexpr std::int32_t ITEM_KIND_AMULET = 25;
    constexpr std::int32_t ITEM_KIND_RING = 26;
    constexpr std::int32_t ITEM_KIND_OTHER_ARMOR = 27;
    // What GetHandCastState returns for a character that cannot cast
    constexpr std::int32_t CAST_STATE_UNKNOWN = -1;
    // What GetAttackActionResult returns: the action has not been carried out yet, the game refused it, the game
    // took it
    constexpr std::int32_t ACTION_RESULT_PENDING = -1;
    constexpr std::int32_t ACTION_RESULT_REFUSED = 0;
    constexpr std::int32_t ACTION_RESULT_TAKEN = 1;
    // What GetAttackState returns for a character whose state cannot be read
    constexpr std::int32_t ATTACK_STATE_UNKNOWN = -1;
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
    // A look in one direction (GetSurroundingsToward) may reach further. The game only has the physics of the area
    // it has loaded around the player (outdoors about 120 meters), so a ray finds nothing beyond that
    constexpr std::int32_t MAX_LOOK_METERS = 200;
    // The rays of such a look fan out over the angle that is asked for: always an odd number, so that one of them goes
    // straight into the direction, about this many degrees apart and at most MAX_LOOK_RAYS
    constexpr float LOOK_RAY_STEP_DEGREES = 15.0f;
    constexpr std::int32_t MIN_LOOK_RAYS = 3;
    constexpr std::int32_t MAX_LOOK_RAYS = 9;
    // The ground at a spot (GetGroundHeight): a ray goes down from this far above the feet of the character to this far
    // below them. Outdoors, if that finds nothing because the ground rises, a second ray comes down from much higher up
    constexpr float GROUND_PROBE_UP_UNITS = 150.0f;
    constexpr float GROUND_PROBE_DOWN_UNITS = 700.0f;
    constexpr float GROUND_PROBE_HIGH_UNITS = 1000.0f;
    constexpr float NO_GROUND_HEIGHT = -1000000.0f;
    // The rays start at chest height, outside the body of the character
    constexpr float RAY_HEIGHT_UNITS = 90.0f;
    // Sight: the rays start at the eyes of the viewer and go to three heights of the thing (a door stands on its
    // position). What a ray meets this close to the thing counts as the thing itself: a door is solid, and so is its frame
    constexpr float SIGHT_EYE_UNITS = 110.0f;
    constexpr std::array<float, 3> SIGHT_TARGET_HEIGHTS = {30.0f, 90.0f, 150.0f};
    constexpr float SIGHT_TARGET_UNITS = 150.0f;
    // GetNearbyPlaces: the places are not filtered by what the viewer can see
    constexpr std::int32_t NO_SIGHT_FILTER = -1;
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
    // RE::InventoryEntryData::GetValue (what an item is worth) and the settings of the game, for trading
    constexpr std::uint64_t ITEM_VALUE_ID = 15757;
    constexpr std::uint64_t GAME_SETTINGS_ID = 514622;
    // RE::TESObjectREFR::IsCrimeToActivate, for what a companion must not collect (see FindLooseItems)
    constexpr std::uint64_t CRIME_TO_ACTIVATE_ID = 19400;
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
    std::vector<OpenQuest> CollectOpenQuests(std::size_t maxObjectives = MAX_OBJECTIVES_PER_QUEST) {
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
            if (openQuest.objectives.size() < maxObjectives) {
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
        // How far the point lies above where the game has the viewer, in game units (negative = below)
        float upUnits = 0.0f;
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
        view.upUnits = up;
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

    // Why a character or a thing is not really there for the viewer, as the end of a sentence, or an empty text if it
    // is there. The game keeps characters it does not show: disabled ones, ones without a body, ones whose body it
    // hides. In a test a character nobody could see stood in the list of the characters around, and the companion
    // was sent to it. For a thing only "disabled" and "being deleted" count: a map marker never has a body
    std::string GetAbsence(RE::TESObjectREFR* reference, const RE::TESObjectREFR* viewer) {
        if (!reference) return "does not exist";
        // The player is always there. Seen through its own eyes the game does not draw its body, which looked like
        // a hidden character here and kept the companion from speaking to the player
        if (reference->IsPlayerRef()) return "";
        if (reference->IsDisabled()) return "is disabled: the game has taken it out of the world";
        if (reference->IsMarkedForDeletion()) return "is being deleted by the game";
        auto* actor = reference->As<RE::Actor>();
        if (!actor) return "";
        if (!actor->Is3DLoaded()) return "has no body in the world right now";
        const auto* body = actor->Get3D();
        if (body && body->GetAppCulled()) return "is hidden: the game does not draw it";
        const auto* cell = actor->GetParentCell();
        if (!cell) return "is in no place of the world right now";
        if (!cell->IsAttached()) return "is in a place the game has not loaded";
        const auto* viewerCell = viewer ? viewer->GetParentCell() : nullptr;
        if (viewerCell && viewerCell != cell) {
            if (viewerCell->IsInteriorCell() || cell->IsInteriorCell()) return "is in another place than the player";
            if (GetRootWorldOf(viewerCell) != GetRootWorldOf(cell)) return "is in another part of the world than the player";
        }
        if (const auto* values = actor->AsActorValueOwner()) {
            if (values->GetActorValue(RE::ActorValue::kInvisibility) > 0.0f) return "is invisible";
        }
        return "";
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

    // How many different items a container holds or a dead character carries that can be taken: the ones the loot
    // menu of the game would show. Lists the game has not rolled out yet, things that are not for the player
    // (the claws of an animal) and things without a name do not count. LOOT_UNKNOWN without a holder
    std::int32_t CountLoot(RE::TESObjectREFR* holder) {
        if (!holder) return LOOT_UNKNOWN;
        std::int32_t kinds = 0;
        for (const auto& [object, entry] : holder->GetInventory()) {
            if (!object || entry.first <= 0) continue;
            if (object->Is(RE::FormType::LeveledItem) || !object->GetPlayable()) continue;
            const char* name = object->GetName();
            if (!name || name[0] == '\0') continue;
            ++kinds;
        }
        return kinds;
    }

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
                if (furniture && furniture->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSleep)) {
                    thing.flags = FURNITURE_IS_BED;
                } else if (furniture && furniture->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSit)) {
                    thing.flags = FURNITURE_IS_SEAT;
                }
            } else if (kind == THING_IS_CONTAINER) {
                const auto* extra = reference->extraList.GetByType<RE::ExtraLock>();
                if (extra && extra->lock && extra->lock->IsLocked()) {
                    thing.flags = CONTAINER_IS_LOCKED;
                } else {
                    // Nobody can look into a locked container. An open one tells whether it is worth opening
                    thing.flags = CONTAINER_COUNT_STEP * (CountLoot(reference) + 1);
                }
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

    constexpr float NO_WATER_ON_RAY = -1.0f;

    struct RayHit {
        bool isHit = false;
        // From the start of the ray, in game units
        float distance = 0.0f;
        // How much the surface that was hit faces upwards: 1 = flat ground, 0 = an upright wall
        float normalUp = 0.0f;
        // The ray met nothing solid, but it did not get to its end either: it went through as many things that are
        // no obstacle as it may. So "nothing was hit" does not mean that nothing is there
        bool isCutShort = false;
        // How far from the start the ray first met something in the layer the game has for water, on its way to what
        // stopped it. NO_WATER_ON_RAY if it met none. Whether water has a body in that layer at all only the game shows
        float waterDistance = NO_WATER_ON_RAY;
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
    RayHit CastRay(RE::bhkWorld* physics, float scale, const RE::NiPoint3& from, const RE::NiPoint3& to, std::int32_t maxPasses = MAX_RAY_PASSES) {
        RayHit result;
        const RE::NiPoint3 whole = to - from;
        const float length = whole.Length();
        if (length <= 0.0f) return result;
        const RE::NiPoint3 direction = whole / length;
        float covered = 0.0f;
        for (std::int32_t pass = 0; pass < maxPasses && covered < length; ++pass) {
            const RE::NiPoint3 start = from + direction * covered;
            RE::bhkPickData pick;
            pick.rayInput.from = RE::hkVector4(start.x * scale, start.y * scale, start.z * scale, 0.0f);
            pick.rayInput.to = RE::hkVector4(to.x * scale, to.y * scale, to.z * scale, 0.0f);
            physics->PickObject(pick);
            if (!pick.rayOutput.HasHit()) return result;
            const float hitDistance = covered + pick.rayOutput.hitFraction * (length - covered);
            const RE::COL_LAYER hitLayer = pick.rayOutput.rootCollidable->GetCollisionLayer();
            if (hitLayer == RE::COL_LAYER::kWater && result.waterDistance < 0.0f) result.waterDistance = hitDistance;
            if (IsSolidLayer(hitLayer)) {
                float normal[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                _mm_storeu_ps(normal, pick.rayOutput.normal.quad);
                result.isHit = true;
                result.distance = hitDistance;
                result.normalUp = normal[2];
                return result;
            }
            covered = hitDistance + RAY_PASS_STEP_UNITS;
        }
        result.isCutShort = covered < length;
        return result;
    }

    std::int32_t ToWholeMeters(float gameUnits) { return static_cast<std::int32_t>(std::lround(gameUnits / GAME_UNITS_PER_METER)); }

    // ---------------------------------------------------------------- heights
    // How much higher or lower than the viewer the characters and things stand that the lists of a state name:
    // a guard up on a wall and one on the ground are both "15 m ahead" otherwise. The lists themselves stay as they
    // are. While they are built, each of them keeps the height of what it names here (KeepHeight), and the script
    // fetches all of it once at the end of a state (TakeHeights). Only a difference of HEIGHT_MIN_METERS or more is kept

    constexpr std::int32_t HEIGHT_MIN_METERS = 2;
    constexpr std::size_t MAX_KEPT_HEIGHTS = 512;

    struct KeptHeight {
        RE::FormID viewerID = 0;
        RE::FormID referenceID = 0;
        std::int32_t meters = 0;
    };

    std::mutex heightLock;
    std::vector<KeptHeight> keptHeights;

    // upUnits: how far the reference lies above where the game has the viewer (see View)
    void KeepHeight(RE::Actor* viewer, RE::FormID referenceID, float upUnits) {
        if (!viewer || referenceID == 0) return;
        // The height counts from the feet. In the saddle those are the hooves of the horse, as for the picture of GetView
        RE::NiPointer<RE::Actor> mount;
        if (viewer->GetMount(mount) && mount) upUnits += viewer->GetPosition().z - mount->GetPosition().z;
        const std::int32_t meters = ToWholeMeters(upUnits);
        const std::lock_guard<std::mutex> guard(heightLock);
        std::erase_if(keptHeights, [&](const KeptHeight& kept) { return kept.referenceID == referenceID; });
        if (meters > -HEIGHT_MIN_METERS && meters < HEIGHT_MIN_METERS) return;
        // Nobody fetched them for a long time (a script that is older than this plugin)
        if (keptHeights.size() >= MAX_KEPT_HEIGHTS) keptHeights.clear();
        KeptHeight kept;
        kept.viewerID = viewer->GetFormID();
        kept.referenceID = referenceID;
        kept.meters = meters;
        keptHeights.push_back(kept);
    }

    // The heights kept since the last call, for what was measured from this viewer: two numbers each, the ref ID and
    // how many whole meters it stands above the feet of the viewer (negative = below). What is missing stands
    // less than HEIGHT_MIN_METERS higher or lower. Forgets all of them
    std::vector<std::int32_t> TakeHeights(RE::StaticFunctionTag*, RE::Actor* viewer) {
        std::vector<std::int32_t> numbers;
        const std::lock_guard<std::mutex> guard(heightLock);
        if (viewer) {
            const RE::FormID viewerID = viewer->GetFormID();
            for (const auto& kept : keptHeights) {
                if (kept.viewerID != viewerID) continue;
                numbers.push_back(static_cast<std::int32_t>(kept.referenceID));
                numbers.push_back(kept.meters);
            }
        }
        keptHeights.clear();
        return numbers;
    }

    // Whether nothing solid of the world lies between the eyes of a viewer and a thing at this position.
    // The game itself can only tell that for the player and between characters (Actor.HasLOS).
    // Like the game the rays go to three heights, and one that gets through is enough.
    // Only call this while the physics are locked for reading
    bool IsInSight(RE::bhkWorld* physics, float scale, const RE::NiPoint3& eyes, const RE::NiPoint3& position) {
        for (const float height : SIGHT_TARGET_HEIGHTS) {
            const RE::NiPoint3 point(position.x, position.y, position.z + height);
            const float length = (point - eyes).Length();
            if (length <= SIGHT_TARGET_UNITS) return true;
            const RayHit hit = CastRay(physics, scale, eyes, point);
            if (!hit.isHit || hit.distance >= length - SIGHT_TARGET_UNITS) return true;
        }
        return false;
    }

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

    // Mute: the character whose own lines of the game are not to be heard (0 = nobody), told by the script
    // (see SetMutedSpeaker), and how many lines of that character have come by. That number tells a line that was
    // cut short apart from the one that followed it
    std::atomic<RE::FormID> mutedSpeakerID{ 0 };
    std::atomic<std::uint32_t> mutedSpeakerLineCount{ 0 };

    // Whether this line is one the game lets the muted character say on its own: a sentence with words
    // (grunts and cries carry no subtitle), not the own line of the game mod (say), and not an answer in a
    // conversation the player is having in the dialogue menu
    bool IsLineToMute(RE::TESObjectREFR* speaker, const char* subtitle) noexcept {
        try {
            const RE::FormID wantedID = mutedSpeakerID.load();
            if (wantedID == 0 || !speaker || !subtitle || speaker->GetFormID() != wantedID) return false;
            const auto text = ViewOf(subtitle);
            const auto first = text.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos) return false;
            const auto last = text.find_last_not_of(" \t\r\n");
            if (text.substr(first, last - first + 1) == OWN_LINE_TEXT) return false;
            auto* menus = RE::UI::GetSingleton();
            if (menus && menus->IsMenuOpen(RE::DialogueMenu::MENU_NAME)) return false;
            return true;
        } catch (...) {
            return false;
        }
    }

    // Ends the line the muted character has just begun. Not from inside the function of the game that is starting
    // the line, but with the next frame, so that the game has finished setting the line up. If the character has
    // begun another line by then (its own line of the game mod, for one), nothing is ended
    void EndLineSoon(RE::TESObjectREFR* speaker, std::uint32_t lineNumber) {
        const auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) return;
        const auto handle = speaker->GetHandle();
        tasks->AddTask([handle, lineNumber]() {
            if (mutedSpeakerLineCount.load() != lineNumber) return;
            const auto reference = handle.get();
            if (reference) reference->StopCurrentDialogue();
        });
    }

    struct SubtitleHook {
        static void Thunk(RE::SubtitleManager* manager, RE::TESObjectREFR* speaker, const char* subtitle, bool alwaysDisplay) {
            if (speaker && speaker->GetFormID() == mutedSpeakerID.load()) {
                const std::uint32_t lineNumber = ++mutedSpeakerLineCount;
                if (IsLineToMute(speaker, subtitle)) {
                    // No subtitle, no sound, and nothing for the script to hear
                    EndLineSoon(speaker, lineNumber);
                    return;
                }
            }
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

    // ----------------------------------------------------------------- events
    // What happens around the player and the companion, kept with the time it happened, for the AI that plays the
    // companion. The game tells its events to whoever registered for them; the plugin keeps one short text per event.
    // Some things have no event of the game (the player begins to fight or to sneak): for those the script calls
    // TickEvents about once a second, and the plugin compares with what it saw the time before. The same call ends
    // what is counted together: the hits the player or the companion takes, and the things the player takes.
    // The script fetches the kept texts when the companion server asks for the state (TakeEvents). The urgent ones
    // (a fight begins, someone dies) already go out with its next request to the server (TakeUrgentEvents).
    // The companion is called "you" in the texts, because the AI that reads them plays it.

    constexpr std::size_t MAX_STORED_EVENTS = 40;
    constexpr std::int64_t MAX_EVENT_AGE_SECONDS = 600;
    constexpr std::size_t MAX_EVENT_TEXT_LENGTH = 260;
    // The same text again within this time is no new event (a door used twice, a spell cast again and again)
    constexpr std::int64_t SAME_EVENT_SECONDS = 10;
    // Characters in a fight lose their target and find it again. That is one fight
    constexpr std::int64_t SAME_FIGHT_SECONDS = 30;
    constexpr std::size_t MAX_REMEMBERED_EVENTS = 30;
    // A death or a fight further away from the player than this is not kept
    constexpr float EVENT_RADIUS_UNITS = 40.0f * GAME_UNITS_PER_METER;
    // The hits someone takes become one event. It ends after this long without a hit, or when it has lasted HIT_LONGEST_SECONDS
    constexpr std::int64_t HIT_QUIET_SECONDS = 4;
    constexpr std::int64_t HIT_LONGEST_SECONDS = 12;
    // The things the player takes become one event per container. It ends after this long without another thing
    constexpr std::int64_t TAKE_QUIET_SECONDS = 2;
    constexpr std::size_t MAX_NAMED_ITEMS = 6;
    constexpr std::size_t MAX_NAMED_ATTACKERS = 3;
    // Someone who is left with less health than this after being hit is urgent news
    constexpr float LOW_HEALTH_PERCENT = 30.0f;
    constexpr float FULL_PERCENT = 100.0f;
    // Right after a save was loaded the game reports everything the player wears as just put on. Nothing is kept that long
    constexpr std::int64_t QUIET_AFTER_LOAD_SECONDS = 5;
    constexpr float MINUTES_PER_HOUR = 60.0f;
    // Whose hits and health are watched
    constexpr std::size_t WATCHED_PLAYER = 0;
    constexpr std::size_t WATCHED_COMPANION = 1;
    constexpr std::size_t WATCHED_COUNT = 2;
    // The ref ID every game gives the player
    constexpr RE::FormID PLAYER_REFERENCE_ID = 0x14;
    constexpr std::string_view DIALOGUE_MENU_NAME = "Dialogue Menu";
    constexpr std::string_view LOCKPICKING_MENU_NAME = "Lockpicking Menu";

    struct GameEvent {
        std::string text;
        std::chrono::steady_clock::time_point time;
        // The time of day in the game, in minutes since midnight
        std::int32_t gameMinutes = 0;
        bool isUrgent = false;
    };

    struct NameCount {
        std::string name;
        std::int32_t count = 0;
    };

    struct Health {
        float now = 0.0f;
        float full = 0.0f;
        bool isKnown = false;
    };

    // The hits one character takes in a row
    struct HitRun {
        bool isActive = false;
        std::int32_t hitCount = 0;
        Health before;
        std::chrono::steady_clock::time_point firstTime;
        std::chrono::steady_clock::time_point lastTime;
        std::vector<NameCount> attackers;
    };

    // The things the player takes out of one container in a row, or hands to the companion
    struct TakeRun {
        bool isActive = false;
        RE::FormID sourceID = 0;
        bool isGift = false;
        std::string sourceName;
        std::chrono::steady_clock::time_point lastTime;
        std::vector<NameCount> items;
    };

    std::mutex eventLock;
    std::deque<GameEvent> gameEvents;
    // The events that were kept last, so that the same one is not kept twice. gameEvents is emptied when it is fetched
    std::deque<GameEvent> rememberedEvents;
    std::array<HitRun, WATCHED_COUNT> hitRuns;
    // The health of the player and of the companion as TickEvents saw it last, while nobody was hitting them
    std::array<Health, WATCHED_COUNT> sampledHealth;
    TakeRun takeRun;
    bool hasPlayerSample = false;
    bool wasPlayerSneaking = false;
    bool wasPlayerFighting = false;
    std::string lastPlayerPlace;
    std::string talkPartnerName;
    bool hasQuietEnd = false;
    std::chrono::steady_clock::time_point quietEnd;
    // Whether nothing is kept at the moment: a save is being loaded, or was loaded a moment ago
    std::atomic<bool> areEventsQuiet{ false };
    // The ref ID of the companion, as the script tells it with every TickEvents (0 = there is none)
    std::atomic<RE::FormID> eventCompanionID{ 0 };
    // While the script itself moves things between the companion and the player (give, take), that is no news
    std::atomic<bool> skipsTransfers{ false };
    // The horse of the companion and the horse of the player, as the script tells them (see SetWatchedHorses, 0 = none):
    // a hit on one of them and its death are news wherever it stands
    std::atomic<RE::FormID> eventOwnHorseID{ 0 };
    std::atomic<RE::FormID> eventPlayerHorseID{ 0 };
    // A hit on one of these horses is told once per fight of the horse: while its hits come at most this far apart,
    // it is the same fight, whoever hits. horseHitTimes: when each horse was hit last. Guarded by eventLock
    constexpr std::int64_t HORSE_FIGHT_GAP_SECONDS = 30;
    constexpr std::size_t MAX_KEPT_HORSE_HITS = 8;
    std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> horseHitTimes;
    // Whether the companion is to follow the player right now, as the script tells with every TickWatch
    std::atomic<bool> isCompanionFollowing{ false };
    // A save was loaded: what CheckWatch remembers belongs to another game
    std::atomic<bool> isWatchForgotten{ false };

    // The journal as TickEvents saw it last: the quests with an objective that is shown and open (see CheckQuests)
    struct QuestSample {
        const RE::TESQuest* quest = nullptr;
        std::string name;
        // Whether the player has picked it in the journal as a quest to follow
        bool isActive = false;
        std::vector<const RE::BGSQuestObjective*> objectives;
        std::vector<std::string> texts;
    };
    std::vector<QuestSample> questSamples;
    bool hasQuestSample = false;
    // The hostile characters the companion has spotted, with the time, and the traps (see SpotDangers)
    std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> spottedCharacters;
    // The hostile characters the companion has heard or felt close by without seeing them, with the time and
    // which of the two it was (SENSE_HEARD or SENSE_CLOSE)
    std::unordered_map<RE::FormID, std::pair<std::chrono::steady_clock::time_point, std::int32_t>> sensedCharacters;
    std::unordered_set<RE::FormID> spottedTraps;
    bool hasSlowCheck = false;
    std::chrono::steady_clock::time_point lastSlowCheck;

    std::int32_t GetGameMinutes() {
        const auto* calendar = RE::Calendar::GetSingleton();
        return calendar ? static_cast<std::int32_t>(calendar->GetHour() * MINUTES_PER_HOUR) : 0;
    }

    // Only call this while eventLock is held
    void PushEventLocked(std::string text, bool isUrgent, std::chrono::steady_clock::time_point now, std::int32_t gameMinutes,
                         std::int64_t sameSeconds = SAME_EVENT_SECONDS) {
        if (text.empty() || areEventsQuiet.load()) return;
        while (!rememberedEvents.empty() && SecondsBetween(rememberedEvents.front().time, now) >= SAME_FIGHT_SECONDS) rememberedEvents.pop_front();
        for (const auto& known : rememberedEvents) {
            if (known.text == text && SecondsBetween(known.time, now) < sameSeconds) return;
        }
        GameEvent kept;
        kept.text = std::move(text);
        kept.time = now;
        kept.gameMinutes = gameMinutes;
        kept.isUrgent = isUrgent;
        rememberedEvents.push_back(kept);
        while (rememberedEvents.size() > MAX_REMEMBERED_EVENTS) rememberedEvents.pop_front();
        gameEvents.push_back(std::move(kept));
        while (gameEvents.size() > MAX_STORED_EVENTS) gameEvents.pop_front();
    }

    void KeepEvent(std::string_view rawText, bool isUrgent = false, std::int64_t sameSeconds = SAME_EVENT_SECONDS) {
        std::string text = CleanText(rawText, MAX_EVENT_TEXT_LENGTH);
        const auto now = std::chrono::steady_clock::now();
        const std::int32_t gameMinutes = GetGameMinutes();
        const std::lock_guard<std::mutex> guard(eventLock);
        PushEventLocked(std::move(text), isUrgent, now, gameMinutes, sameSeconds);
    }

    bool IsEventCompanion(const RE::TESObjectREFR* reference) {
        const RE::FormID companionID = eventCompanionID.load();
        return reference && companionID != 0 && reference->GetFormID() == companionID;
    }

    bool IsWatched(const RE::TESObjectREFR* reference) { return reference && (reference->IsPlayerRef() || IsEventCompanion(reference)); }

    bool IsOwnHorse(const RE::TESObjectREFR* reference) {
        const RE::FormID horseID = eventOwnHorseID.load();
        return reference && horseID != 0 && reference->GetFormID() == horseID;
    }

    bool IsPlayersHorse(const RE::TESObjectREFR* reference) {
        const RE::FormID horseID = eventPlayerHorseID.load();
        return reference && horseID != 0 && reference->GetFormID() == horseID;
    }

    // How the texts call a character or a thing. isFirst: the name begins a sentence
    std::string GetEventName(RE::TESObjectREFR* reference, bool isFirst) {
        if (reference) {
            if (reference->IsPlayerRef()) return isFirst ? "The player" : "the player";
            if (IsEventCompanion(reference)) return isFirst ? "You" : "you";
            if (IsOwnHorse(reference)) return isFirst ? "Your horse" : "your horse";
            if (IsPlayersHorse(reference)) return isFirst ? "The player's horse" : "the player's horse";
            std::string name = CleanText(ViewOf(reference->GetDisplayFullName()));
            if (!name.empty()) return name;
        }
        return isFirst ? "Someone" : "someone";
    }

    bool IsNearPlayer(const RE::TESObjectREFR* reference) {
        const auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !reference) return false;
        return player->GetPosition().GetDistance(reference->GetPosition()) <= EVENT_RADIUS_UNITS;
    }

    Health ReadHealth(RE::Actor* actor) {
        Health health;
        const auto* values = actor ? actor->AsActorValueOwner() : nullptr;
        if (!values) return health;
        health.now = values->GetActorValue(RE::ActorValue::kHealth);
        health.full = values->GetPermanentActorValue(RE::ActorValue::kHealth);
        health.isKnown = true;
        return health;
    }

    void AddNameCount(std::vector<NameCount>& list, std::string_view name, std::int32_t count) {
        for (auto& entry : list) {
            if (entry.name == name) {
                entry.count += count;
                return;
            }
        }
        NameCount entry;
        entry.name = std::string(name);
        entry.count = count;
        list.push_back(std::move(entry));
    }

    // "Wolf (2), Bandit and 3 more": the names with their numbers where they are above one, at most limit of them
    std::string ListNameCounts(const std::vector<NameCount>& list, std::size_t limit) {
        std::string text;
        std::size_t shown = 0;
        for (const auto& entry : list) {
            if (shown == limit) break;
            if (shown > 0) text += ", ";
            text += entry.name;
            if (entry.count > 1) text += std::format(" ({})", entry.count);
            ++shown;
        }
        if (list.size() > shown) text += std::format(" and {} more", list.size() - shown);
        return text;
    }

    // Ends the hits the watched character took in a row and keeps them as one event. Only call this while eventLock is held.
    // after: the health of that character now
    void EndHitRunLocked(std::size_t watched, const Health& after, std::chrono::steady_clock::time_point now, std::int32_t gameMinutes) {
        HitRun& run = hitRuns[watched];
        if (!run.isActive) return;
        run.isActive = false;
        std::string text = watched == WATCHED_PLAYER ? "The player" : "You";
        text += run.hitCount == 1 ? " took a hit" : std::format(" took {} hits", run.hitCount);
        if (!run.attackers.empty()) text += " from " + ListNameCounts(run.attackers, MAX_NAMED_ATTACKERS);
        bool isUrgent = false;
        if (after.isKnown && after.full > 0.0f) {
            const float percent = std::clamp(after.now / after.full * FULL_PERCENT, 0.0f, FULL_PERCENT);
            const float lost = run.before.isKnown ? run.before.now - after.now : 0.0f;
            if (lost >= 1.0f) {
                text += std::format(" and lost about {} health ({}% left)", FormatNumber(lost), FormatNumber(percent));
            } else {
                text += std::format(" ({}% health left)", FormatNumber(percent));
            }
            isUrgent = percent < LOW_HEALTH_PERCENT;
        }
        text += ".";
        // Hits are counted, so two events of them never say the same thing twice
        PushEventLocked(std::move(text), isUrgent, now, gameMinutes, 0);
    }

    // Ends the things the player took in a row and keeps them as one event. Only call this while eventLock is held
    void EndTakeRunLocked(std::chrono::steady_clock::time_point now, std::int32_t gameMinutes) {
        if (!takeRun.isActive) return;
        takeRun.isActive = false;
        if (takeRun.items.empty()) return;
        std::string text;
        if (takeRun.isGift) {
            text = "The player gave you: ";
        } else if (takeRun.sourceName.empty()) {
            text = "The player got: ";
        } else {
            text = "The player took from " + takeRun.sourceName + ": ";
        }
        text += ListNameCounts(takeRun.items, MAX_NAMED_ITEMS) + ".";
        PushEventLocked(std::move(text), false, now, gameMinutes, 0);
    }

    // Ends what is counted together once it has been quiet for long enough (isAll: in any case).
    // The health of the two watched characters is read before the lock is taken, because that asks the game
    void EndRuns(bool isAll) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        const RE::FormID companionID = eventCompanionID.load();
        auto* companion = companionID != 0 ? RE::TESForm::LookupByID<RE::Actor>(companionID) : nullptr;
        std::array<Health, WATCHED_COUNT> health;
        health[WATCHED_PLAYER] = ReadHealth(player);
        health[WATCHED_COMPANION] = ReadHealth(companion);
        const auto now = std::chrono::steady_clock::now();
        const std::int32_t gameMinutes = GetGameMinutes();

        const std::lock_guard<std::mutex> guard(eventLock);
        if (areEventsQuiet.load() && hasQuietEnd && now >= quietEnd) {
            areEventsQuiet.store(false);
            hasQuietEnd = false;
        }
        for (std::size_t watched = 0; watched < WATCHED_COUNT; ++watched) {
            HitRun& run = hitRuns[watched];
            if (run.isActive) {
                const bool isOver = SecondsBetween(run.lastTime, now) >= HIT_QUIET_SECONDS || SecondsBetween(run.firstTime, now) >= HIT_LONGEST_SECONDS;
                if (isAll || isOver) EndHitRunLocked(watched, health[watched], now, gameMinutes);
            }
            if (!run.isActive) sampledHealth[watched] = health[watched];
        }
        if (takeRun.isActive && (isAll || SecondsBetween(takeRun.lastTime, now) >= TAKE_QUIET_SECONDS)) EndTakeRunLocked(now, gameMinutes);
    }

    void OnCombatEvent(const RE::TESCombatEvent& event) {
        auto* fighter = event.actor.get();
        auto* target = event.targetActor.get();
        if (!fighter) return;
        if (event.newState == RE::ACTOR_COMBAT_STATE::kCombat) {
            if (IsEventCompanion(fighter)) {
                KeepEvent("You started fighting " + GetEventName(target, false) + ".", false, SAME_FIGHT_SECONDS);
            } else if (IsWatched(target)) {
                KeepEvent(GetEventName(fighter, true) + " started fighting " + GetEventName(target, false) + ".", true, SAME_FIGHT_SECONDS);
            }
        } else if (event.newState == RE::ACTOR_COMBAT_STATE::kNone && IsEventCompanion(fighter)) {
            KeepEvent("You stopped fighting.", false, SAME_FIGHT_SECONDS);
        }
    }

    void OnDeathEvent(const RE::TESDeathEvent& event) {
        auto* victim = event.actorDying.get();
        auto* killer = event.actorKiller.get();
        if (!victim) return;
        if (!IsWatched(victim) && !IsWatched(killer) && !IsNearPlayer(victim) && !IsOwnHorse(victim) && !IsPlayersHorse(victim)) return;
        // The game tells a death twice (dying, dead). The second one has the same text and is dropped
        if (killer && killer != victim) {
            KeepEvent(GetEventName(killer, true) + " killed " + GetEventName(victim, false) + ".", true);
        } else {
            KeepEvent(GetEventName(victim, true) + " died.", true);
        }
    }

    void OnHitEvent(const RE::TESHitEvent& event) {
        auto* target = event.target.get();
        auto* attacker = event.cause.get();
        if ((IsOwnHorse(target) || IsPlayersHorse(target)) && attacker != target) {
            // One sentence per fight of the horse, however many hits it takes and from however many attackers
            // (see HORSE_FIGHT_GAP_SECONDS). In a test a horse between two wolves was told again and again
            const auto hitTime = std::chrono::steady_clock::now();
            bool isNewFight = false;
            {
                const std::lock_guard<std::mutex> guard(eventLock);
                const auto known = horseHitTimes.find(target->GetFormID());
                isNewFight = known == horseHitTimes.end() || SecondsBetween(known->second, hitTime) >= HORSE_FIGHT_GAP_SECONDS;
                if (horseHitTimes.size() > MAX_KEPT_HORSE_HITS) horseHitTimes.clear();
                horseHitTimes[target->GetFormID()] = hitTime;
            }
            if (isNewFight) {
                KeepEvent(GetEventName(target, true) + " is being attacked" + (attacker ? " by " + GetEventName(attacker, false) : std::string()) + ".",
                          true, SAME_FIGHT_SECONDS);
            }
            return;
        }
        if (!IsWatched(target) || attacker == target) return;
        const std::size_t watched = target->IsPlayerRef() ? WATCHED_PLAYER : WATCHED_COMPANION;
        const std::string attackerName = attacker ? GetEventName(attacker, false) : std::string();
        const Health health = ReadHealth(target->As<RE::Actor>());
        const auto now = std::chrono::steady_clock::now();

        const std::lock_guard<std::mutex> guard(eventLock);
        if (areEventsQuiet.load()) return;
        HitRun& run = hitRuns[watched];
        if (!run.isActive) {
            run = HitRun();
            run.isActive = true;
            run.firstTime = now;
            // What TickEvents saw a moment ago is the health before this hit. Without it: the health of this moment
            run.before = sampledHealth[watched].isKnown ? sampledHealth[watched] : health;
        }
        ++run.hitCount;
        run.lastTime = now;
        if (!attackerName.empty()) AddNameCount(run.attackers, attackerName, 1);
    }

    void OnLocationEvent(const RE::TESActorLocationChangeEvent& event) {
        const auto* mover = event.actor.get();
        if (!mover || !mover->IsPlayerRef()) return;
        const std::string newName = event.newLoc ? CleanText(ViewOf(event.newLoc->GetFullName())) : std::string();
        const std::string oldName = event.oldLoc ? CleanText(ViewOf(event.oldLoc->GetFullName())) : std::string();
        std::string text;
        {
            const std::lock_guard<std::mutex> guard(eventLock);
            if (!newName.empty()) {
                if (newName == lastPlayerPlace) return;
                lastPlayerPlace = newName;
                text = "The player entered " + newName + ".";
            } else if (!oldName.empty()) {
                lastPlayerPlace.clear();
                text = "The player left " + oldName + ".";
            }
        }
        if (!text.empty()) KeepEvent(text);
    }

    void OnActivateEvent(const RE::TESActivateEvent& event) {
        auto* thing = event.objectActivated.get();
        const auto* user = event.actionRef.get();
        if (!thing || !user || !user->IsPlayerRef()) return;
        if (const auto* character = thing->As<RE::Actor>()) {
            // Speaking to someone shows as the dialogue menu, getting on a horse shows in the state
            if (character->IsDead() && !IsEventCompanion(thing)) KeepEvent("The player searched the body of " + GetEventName(thing, false) + ".");
            return;
        }
        const auto* base = thing->GetBaseObject();
        if (!base) return;
        const std::string name = CleanText(ViewOf(thing->GetDisplayFullName()));
        switch (base->GetFormType()) {
            case RE::FormType::Door: {
                const std::string place = GetNameBehindDoor(thing);
                if (!place.empty()) {
                    KeepEvent("The player used the door to " + place + ".");
                } else if (!name.empty()) {
                    KeepEvent("The player used the door '" + name + "'.");
                }
                break;
            }
            case RE::FormType::Container:
                if (name.empty()) break;
                KeepEvent(thing->IsLocked() ? "The player tried the locked " + name + "." : "The player opened " + name + ".");
                break;
            case RE::FormType::Activator:
            case RE::FormType::TalkingActivator:
            case RE::FormType::Furniture:
                if (!name.empty()) KeepEvent("The player used " + name + ".");
                break;
            default:
                // Things that are picked up show when they change hands, a book shows when it is read
                break;
        }
    }

    void OnBookReadEvent(const RE::TESBookReadEvent& event) {
        const auto* book = RE::TESForm::LookupByID<RE::TESObjectBOOK>(event.baseFormID);
        if (!book) return;
        const std::string title = CleanText(ViewOf(book->GetName()));
        if (title.empty()) return;
        const char* kind = "the book";
        if (book->TeachesSpell()) {
            kind = "the spell tome";
        } else if (book->IsNoteScroll()) {
            kind = "the note";
        }
        KeepEvent(std::format("The player read {} '{}' (ID {:08X}).", kind, title, book->GetFormID()));
    }

    void OnContainerEvent(const RE::TESContainerChangedEvent& event) {
        if (skipsTransfers.load() || event.itemCount <= 0) return;
        const RE::FormID companionID = eventCompanionID.load();
        const bool isGift = companionID != 0 && event.newContainer == companionID && event.oldContainer == PLAYER_REFERENCE_ID;
        if (!isGift && event.newContainer != PLAYER_REFERENCE_ID) return;
        const auto* item = RE::TESForm::LookupByID(event.baseObj);
        const std::string itemName = item ? CleanText(ViewOf(item->GetName())) : std::string();
        if (itemName.empty()) return;
        const RE::FormID sourceID = isGift ? 0 : event.oldContainer;
        std::string sourceName;
        if (sourceID != 0) {
            auto* sourceForm = RE::TESForm::LookupByID(sourceID);
            auto* source = sourceForm ? sourceForm->AsReference() : nullptr;
            if (source) {
                const auto* holder = source->As<RE::Actor>();
                sourceName = GetEventName(source, false);
                if (holder && holder->IsDead() && !IsEventCompanion(source)) sourceName = "the body of " + sourceName;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        const std::int32_t gameMinutes = GetGameMinutes();

        const std::lock_guard<std::mutex> guard(eventLock);
        if (areEventsQuiet.load()) return;
        if (takeRun.isActive && (takeRun.sourceID != sourceID || takeRun.isGift != isGift)) EndTakeRunLocked(now, gameMinutes);
        if (!takeRun.isActive) {
            takeRun = TakeRun();
            takeRun.isActive = true;
            takeRun.sourceID = sourceID;
            takeRun.isGift = isGift;
            takeRun.sourceName = sourceName;
        }
        takeRun.lastTime = now;
        AddNameCount(takeRun.items, itemName, event.itemCount);
    }

    void OnEquipEvent(const RE::TESEquipEvent& event) {
        const auto* wearer = event.actor.get();
        if (!event.equipped || !wearer || !wearer->IsPlayerRef()) return;
        const auto* item = RE::TESForm::LookupByID(event.baseObject);
        if (!item) return;
        const std::string name = CleanText(ViewOf(item->GetName()));
        if (name.empty()) return;
        switch (item->GetFormType()) {
            case RE::FormType::Weapon:
            case RE::FormType::Armor:
            case RE::FormType::Light:
                KeepEvent("The player equipped " + name + ".");
                break;
            case RE::FormType::Scroll:
                KeepEvent("The player took the scroll " + name + " in hand.");
                break;
            case RE::FormType::AlchemyItem: {
                const auto* potion = item->As<RE::AlchemyItem>();
                if (potion && potion->IsPoison()) {
                    KeepEvent("The player used the poison " + name + ".");
                } else if (potion && potion->IsFood()) {
                    KeepEvent("The player ate or drank " + name + ".");
                } else {
                    KeepEvent("The player drank " + name + ".");
                }
                break;
            }
            default:
                break;
        }
    }

    void OnSpellCastEvent(const RE::TESSpellCastEvent& event) {
        const auto* caster = event.object.get();
        if (!caster || !caster->IsPlayerRef()) return;
        const auto* spell = RE::TESForm::LookupByID(event.spell);
        // Potions, enchantments and scrolls come here as well. Only real spells count
        if (!spell || !spell->Is(RE::FormType::Spell)) return;
        const std::string name = CleanText(ViewOf(spell->GetName()));
        if (!name.empty()) KeepEvent("The player cast " + name + ".");
    }

    void OnBleedoutEvent(const RE::TESEnterBleedoutEvent& event) {
        auto* fallen = event.actor.get();
        if (!fallen || fallen->IsPlayerRef()) return;
        const auto* character = fallen->As<RE::Actor>();
        if (IsEventCompanion(fallen)) {
            KeepEvent("You went down and are bleeding out.", true);
        } else if (character && character->IsPlayerTeammate()) {
            KeepEvent(GetEventName(fallen, true) + " went down and is bleeding out.", true);
        }
    }

    // Which menus were open, for the log of the companion server: a game that stands still in a menu asks the server
    // nothing, and afterwards nobody could tell why it was silent. The script fetches the lines with its next request
    // (TakeMenuLines). What the game shows all the time or with every other menu is left out
    constexpr std::array<std::string_view, 8> LASTING_MENU_NAMES = {"HUD Menu",  "Cursor Menu",        "Fader Menu",   "LoadWaitSpinner",
                                                                    "Mist Menu", "WSActivateRollover", "SafeZoneMenu", "Kinect Menu"};
    constexpr std::size_t MAX_KEPT_MENUS = 20;

    struct MenuVisit {
        std::string name;
        std::int64_t milliseconds = 0;
    };

    std::mutex menuLock;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> openMenus;
    std::deque<MenuVisit> menuVisits;

    // Remembers when a menu opens, and how long it was open once it closes
    void NoteMenu(std::string_view menuName, bool isOpening) {
        if (menuName.empty()) return;
        if (std::find(LASTING_MENU_NAMES.begin(), LASTING_MENU_NAMES.end(), menuName) != LASTING_MENU_NAMES.end()) return;
        const auto now = std::chrono::steady_clock::now();
        const std::string name(menuName);
        const std::lock_guard<std::mutex> guard(menuLock);
        if (isOpening) {
            openMenus.try_emplace(name, now);
            return;
        }
        const auto opened = openMenus.find(name);
        if (opened == openMenus.end()) return;
        MenuVisit visit;
        visit.name = name;
        visit.milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now - opened->second).count();
        openMenus.erase(opened);
        menuVisits.push_back(std::move(visit));
        while (menuVisits.size() > MAX_KEPT_MENUS) menuVisits.pop_front();
    }

    void OnMenuEvent(const RE::MenuOpenCloseEvent& event) {
        const auto menuName = ViewOf(event.menuName.c_str());
        NoteMenu(menuName, event.opening);
        if (menuName == LOCKPICKING_MENU_NAME) {
            if (event.opening) KeepEvent("The player started picking a lock.");
            return;
        }
        if (menuName != DIALOGUE_MENU_NAME) return;
        std::string partnerName;
        if (event.opening) {
            const auto* topics = RE::MenuTopicManager::GetSingleton();
            const auto partner = topics ? topics->speaker.get() : RE::NiPointer<RE::TESObjectREFR>();
            if (partner) partnerName = GetEventName(partner.get(), false);
        }
        std::string text;
        {
            const std::lock_guard<std::mutex> guard(eventLock);
            if (event.opening) {
                talkPartnerName = partnerName;
                text = partnerName.empty() ? "The player started a conversation." : "The player started talking to " + partnerName + ".";
            } else {
                text = talkPartnerName.empty() ? "The player ended the conversation." : "The player stopped talking to " + talkPartnerName + ".";
                talkPartnerName.clear();
            }
        }
        KeepEvent(text, false, 0);
    }

    // One listener per kind of event. Whatever goes wrong in a handler must not reach the game
    template <class Event, void (*Handler)(const Event&)>
    class EventListener final : public RE::BSTEventSink<Event> {
    public:
        RE::BSEventNotifyControl ProcessEvent(const Event* event, RE::BSTEventSource<Event>*) override {
            try {
                if (event && !areEventsQuiet.load()) Handler(*event);
            } catch (...) {
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    EventListener<RE::TESCombatEvent, OnCombatEvent> combatListener;
    EventListener<RE::TESDeathEvent, OnDeathEvent> deathListener;
    EventListener<RE::TESHitEvent, OnHitEvent> hitListener;
    EventListener<RE::TESActorLocationChangeEvent, OnLocationEvent> locationListener;
    EventListener<RE::TESActivateEvent, OnActivateEvent> activateListener;
    EventListener<RE::TESBookReadEvent, OnBookReadEvent> bookReadListener;
    EventListener<RE::TESContainerChangedEvent, OnContainerEvent> containerListener;
    EventListener<RE::TESEquipEvent, OnEquipEvent> equipListener;
    EventListener<RE::TESSpellCastEvent, OnSpellCastEvent> spellCastListener;
    EventListener<RE::TESEnterBleedoutEvent, OnBleedoutEvent> bleedoutListener;
    EventListener<RE::MenuOpenCloseEvent, OnMenuEvent> menuListener;
    bool areListenersInstalled = false;

    // Registers for the events of the game. They only exist once the game has loaded its data
    void InstallEventListeners() {
        if (areListenersInstalled) return;
        auto* events = RE::ScriptEventSourceHolder::GetSingleton();
        if (!events) {
            logger::warn("Events: the game has no source of events, so nothing is kept of what happens.");
            return;
        }
        events->AddEventSink<RE::TESCombatEvent>(&combatListener);
        events->AddEventSink<RE::TESDeathEvent>(&deathListener);
        events->AddEventSink<RE::TESHitEvent>(&hitListener);
        events->AddEventSink<RE::TESActorLocationChangeEvent>(&locationListener);
        events->AddEventSink<RE::TESActivateEvent>(&activateListener);
        events->AddEventSink<RE::TESBookReadEvent>(&bookReadListener);
        events->AddEventSink<RE::TESContainerChangedEvent>(&containerListener);
        events->AddEventSink<RE::TESEquipEvent>(&equipListener);
        events->AddEventSink<RE::TESSpellCastEvent>(&spellCastListener);
        events->AddEventSink<RE::TESEnterBleedoutEvent>(&bleedoutListener);
        if (auto* menus = RE::UI::GetSingleton()) menus->AddEventSink<RE::MenuOpenCloseEvent>(&menuListener);
        areListenersInstalled = true;
        logger::info("Events: listening to what happens in the game.");
    }

    // What happened before a save is loaded or a new game starts belongs to another game
    void ForgetEvents() {
        areEventsQuiet.store(true);
        const std::lock_guard<std::mutex> guard(eventLock);
        gameEvents.clear();
        rememberedEvents.clear();
        for (auto& run : hitRuns) run = HitRun();
        for (auto& health : sampledHealth) health = Health();
        takeRun = TakeRun();
        hasPlayerSample = false;
        lastPlayerPlace.clear();
        talkPartnerName.clear();
        hasQuietEnd = false;
        questSamples.clear();
        hasQuestSample = false;
        spottedCharacters.clear();
        sensedCharacters.clear();
        spottedTraps.clear();
        horseHitTimes.clear();
        hasSlowCheck = false;
        isWatchForgotten.store(true);
    }

    // The game is running again: a moment later events are kept again (see QUIET_AFTER_LOAD_SECONDS)
    void AllowEventsSoon() {
        const std::lock_guard<std::mutex> guard(eventLock);
        quietEnd = std::chrono::steady_clock::now() + std::chrono::seconds(QUIET_AFTER_LOAD_SECONDS);
        hasQuietEnd = true;
    }

    // ------------------------------------------------------- quests in the events
    // The game has events for quests that start, stop and reach a stage, but they come for every quest that runs
    // unseen in the background. So the plugin compares the journal of the player with how it looked a moment ago:
    // only what the player is shown gets into the events, and the text of the objective is at hand

    constexpr std::int64_t SLOW_CHECK_SECONDS = 2;
    constexpr std::size_t MAX_WATCHED_OBJECTIVES = 40;
    constexpr std::string_view OBJECTIVE_SEPARATOR = "; ";

    // The newest state of an objective in the journal (see GetJournalEntries)
    RE::QUEST_OBJECTIVE_STATE GetObjectiveState(const RE::BGSQuestObjective* objective) {
        auto state = RE::QUEST_OBJECTIVE_STATE::kDormant;
        const auto* entries = GetJournalEntries();
        if (!entries) return state;
        for (const auto& entry : *entries) {
            if (entry.Objective == objective) state = entry.InstanceState;
        }
        return state;
    }

    void AddObjectiveText(std::string& list, std::string_view text) {
        if (!list.empty()) list += OBJECTIVE_SEPARATOR;
        list += text;
    }

    // Keeps an event for every quest that is new in the journal, has a new objective, lost one or is over
    void CheckQuests() {
        std::vector<QuestSample> now;
        for (auto& open : CollectOpenQuests(MAX_WATCHED_OBJECTIVES)) {
            QuestSample sample;
            sample.quest = open.quest;
            sample.name = CleanText(ViewOf(open.quest->GetName()));
            sample.isActive = open.quest->IsActive();
            sample.objectives = std::move(open.objectiveForms);
            sample.texts = std::move(open.objectives);
            now.push_back(std::move(sample));
        }
        std::vector<std::string> texts;
        {
            const std::lock_guard<std::mutex> guard(eventLock);
            if (hasQuestSample) {
                for (const auto& sample : now) {
                    const QuestSample* before = nullptr;
                    for (const auto& known : questSamples) {
                        if (known.quest == sample.quest) before = &known;
                    }
                    std::string added;
                    for (std::size_t index = 0; index < sample.objectives.size() && index < sample.texts.size(); ++index) {
                        if (!before || std::find(before->objectives.begin(), before->objectives.end(), sample.objectives[index]) == before->objectives.end()) {
                            AddObjectiveText(added, sample.texts[index]);
                        }
                    }
                    if (!before) {
                        if (added.empty()) continue;
                        texts.push_back(sample.name.empty() ? "New objective: " + added + "." : "New quest: " + sample.name + ". Objective: " + added + ".");
                        continue;
                    }
                    // The player picked another quest to follow in the journal
                    if (sample.isActive && !before->isActive && !sample.name.empty()) texts.push_back("The player now follows the quest " + sample.name + ".");
                    std::string done;
                    std::string failed;
                    for (std::size_t index = 0; index < before->objectives.size() && index < before->texts.size(); ++index) {
                        if (std::find(sample.objectives.begin(), sample.objectives.end(), before->objectives[index]) != sample.objectives.end()) continue;
                        const auto state = GetObjectiveState(before->objectives[index]);
                        if (state == RE::QUEST_OBJECTIVE_STATE::kCompleted || state == RE::QUEST_OBJECTIVE_STATE::kCompletedDisplayed) {
                            AddObjectiveText(done, before->texts[index]);
                        } else if (state == RE::QUEST_OBJECTIVE_STATE::kFailed || state == RE::QUEST_OBJECTIVE_STATE::kFailedDisplayed) {
                            AddObjectiveText(failed, before->texts[index]);
                        }
                    }
                    if (added.empty() && done.empty() && failed.empty()) continue;
                    std::string text = sample.name.empty() ? "" : "Quest " + sample.name + ".";
                    if (!done.empty()) text += " Objective done: " + done + ".";
                    if (!failed.empty()) text += " Objective failed: " + failed + ".";
                    if (!added.empty()) text += " New objective: " + added + ".";
                    texts.push_back(std::move(text));
                }
                for (const auto& before : questSamples) {
                    bool isStillOpen = false;
                    for (const auto& sample : now) {
                        if (sample.quest == before.quest) isStillOpen = true;
                    }
                    if (isStillOpen || !before.quest) continue;
                    if (before.quest->data.flags.all(RE::QuestFlag::kFailed)) {
                        texts.push_back(before.name.empty() ? "An objective failed." : "Quest " + before.name + " failed.");
                        continue;
                    }
                    if (before.quest->IsCompleted() && !before.name.empty()) {
                        texts.push_back("Quest " + before.name + " is completed.");
                        continue;
                    }
                    // The quest goes on without an objective that is shown, or it has no name of its own
                    std::string done;
                    for (std::size_t index = 0; index < before.objectives.size() && index < before.texts.size(); ++index) {
                        const auto state = GetObjectiveState(before.objectives[index]);
                        if (state == RE::QUEST_OBJECTIVE_STATE::kCompleted || state == RE::QUEST_OBJECTIVE_STATE::kCompletedDisplayed) {
                            AddObjectiveText(done, before.texts[index]);
                        }
                    }
                    if (done.empty()) continue;
                    texts.push_back((before.name.empty() ? "" : "Quest " + before.name + ". ") + "Objective done: " + done + ".");
                }
            }
            questSamples = std::move(now);
            hasQuestSample = true;
        }
        for (const auto& text : texts) KeepEvent(text, false, 0);
    }

    // ---------------------------------------------------------------- dangers
    // What the companion spots by itself: hostile characters and traps. Only what lies where it faces and what
    // nothing solid stands in front of: a companion that tells of a trap behind a wall would know more than it can.
    // Each one becomes an urgent event, so the AI hears of it with the answer to its next command

    // ------------------------------------------------------------ senses
    // What a viewer notices of a character: it sees it (in front, with nothing solid in between), it has it close by
    // (within CLOSE_UNITS all around, whatever is in between and however quiet the other is), or it hears it. How far
    // a character can be heard goes by how loud it is, and behind something solid only half as far. The numbers are
    // what the player asked for after a test in which an enemy could walk up to the companion from behind unnoticed
    constexpr std::int32_t SENSE_NONE = 0;
    constexpr std::int32_t SENSE_SEEN = 1;
    constexpr std::int32_t SENSE_CLOSE = 2;
    constexpr std::int32_t SENSE_HEARD = 3;
    constexpr float CLOSE_UNITS = 2.0f * GAME_UNITS_PER_METER;
    constexpr float HEAR_SNEAKING_UNITS = 3.0f * GAME_UNITS_PER_METER;
    constexpr float HEAR_STILL_UNITS = 5.0f * GAME_UNITS_PER_METER;
    constexpr float HEAR_WALKING_UNITS = 12.0f * GAME_UNITS_PER_METER;
    // Running, fighting, with a weapon drawn or casting a spell
    constexpr float HEAR_LOUD_UNITS = 30.0f * GAME_UNITS_PER_METER;
    constexpr float HEAR_BEHIND_WALL_PART = 0.5f;

    // How far away a living character can be heard at the moment, with nothing solid in between
    float GetHearingUnits(RE::Actor* actor) {
        if (actor->IsSneaking()) return HEAR_SNEAKING_UNITS;
        const auto* state = actor->AsActorState();
        const bool isMoving = actor->IsMoving();
        const bool isArmed = state && state->IsWeaponDrawn();
        const bool isFast = isMoving && state && (state->IsRunning() || state->IsSprinting());
        if (actor->IsInCombat() || isArmed || isFast || actor->WhoIsCasting() != 0) return HEAR_LOUD_UNITS;
        return isMoving ? HEAR_WALKING_UNITS : HEAR_STILL_UNITS;
    }

    // How far a viewer sees at the moment, by the weather, the time of day and the light (see UpdateSight further
    // down, which works it out and keeps it here). It holds for the viewer it was worked out for, and only for a few
    // seconds: the script asks for it anew with every state that is about seeing. Everything else sees without a limit
    constexpr float NO_SIGHT_LIMIT_UNITS = 1.0e9f;
    constexpr std::int64_t SIGHT_VALID_SECONDS = 10;
    // Places of the map are towers, farms and mills: big things that show from further away than a character does
    constexpr float SIGHT_PLACE_TIMES = 3.0f;
    std::mutex sightLock;
    RE::FormID keptSightViewerID = 0;
    float keptSightUnits = NO_SIGHT_LIMIT_UNITS;
    std::chrono::steady_clock::time_point keptSightTime;

    float GetSightUnitsOf(const RE::TESObjectREFR* viewer) {
        if (!viewer) return NO_SIGHT_LIMIT_UNITS;
        const std::lock_guard<std::mutex> guard(sightLock);
        if (keptSightViewerID != viewer->GetFormID()) return NO_SIGHT_LIMIT_UNITS;
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - keptSightTime).count();
        return age <= SIGHT_VALID_SECONDS ? keptSightUnits : NO_SIGHT_LIMIT_UNITS;
    }

    // Which sense tells a viewer of a character, or SENSE_NONE if it notices nothing of it. view and distanceUnits:
    // where the character is, seen from the viewer. halfAngleDegrees: how far to the left and right the viewer sees.
    // The dead make no sound. Only call this while the physics are locked for reading
    std::int32_t GetSenseOf(RE::bhkWorld* physics, float scale, const RE::NiPoint3& eyes, RE::Actor* actor, const View& view, float distanceUnits,
                            std::int32_t halfAngleDegrees, float sightUnits = NO_SIGHT_LIMIT_UNITS) {
        const bool isInFront = std::abs(view.angleDegrees) <= halfAngleDegrees;
        // Further away than anything can be heard, and not in front: no ray is needed to tell
        if (!isInFront && distanceUnits > HEAR_LOUD_UNITS) return SENSE_NONE;
        const bool isInSight = IsInSight(physics, scale, eyes, actor->GetPosition());
        // Further away than the viewer sees in this weather and light, a character can still be heard
        if (isInFront && isInSight && distanceUnits <= sightUnits) return SENSE_SEEN;
        if (distanceUnits <= CLOSE_UNITS) return SENSE_CLOSE;
        if (actor->IsDead()) return SENSE_NONE;
        const float reach = GetHearingUnits(actor) * (isInSight ? 1.0f : HEAR_BEHIND_WALL_PART);
        return distanceUnits <= reach ? SENSE_HEARD : SENSE_NONE;
    }

    constexpr float SPOT_RADIUS_UNITS = 40.0f * GAME_UNITS_PER_METER;
    constexpr std::int32_t SPOT_HALF_ANGLE_DEGREES = 95;
    constexpr std::int32_t SPOT_AHEAD_DEGREES = 25;
    constexpr std::int32_t SPOT_BEHIND_DEGREES = 135;
    constexpr std::int64_t SPOT_AGAIN_SECONDS = 120;
    constexpr std::size_t MAX_NAMED_SPOTS = 3;
    constexpr std::size_t MAX_REMEMBERED_SPOTS = 300;
    // The game counts prey as hostile (deer, elk, wild goats, hares, foxes, chickens, cows), but it always runs
    // away: its actor value "Confidence" is 0 (cowardly). Whoever has less than this is no danger to tell of. If
    // it attacks after all, the fight has its events
    constexpr float SPOT_LEAST_CONFIDENCE = 1.0f;

    struct Spot {
        RE::FormID referenceID = 0;
        RE::NiPoint3 position;
        View view;
        std::string name;
        // For characters: who it is, how far away in game units, and which sense tells of it (see SENSE_...)
        RE::Actor* actor = nullptr;
        float distanceUnits = 0.0f;
        std::int32_t sense = SENSE_NONE;
    };

    // "25 m ahead of you", "8 m to your left"
    std::string DescribeSpot(const View& view) {
        std::string where = std::format("{} m ", view.distanceMeters);
        if (std::abs(view.angleDegrees) <= SPOT_AHEAD_DEGREES) {
            where += "ahead of you";
        } else {
            where += view.angleDegrees < 0 ? "to your left" : "to your right";
        }
        return where;
    }

    // "behind you", "to your left": where something is that is heard or close by and not seen
    std::string DescribeSide(const View& view) {
        const std::int32_t offDegrees = std::abs(view.angleDegrees);
        if (offDegrees <= SPOT_AHEAD_DEGREES) return "ahead of you";
        if (offDegrees >= SPOT_BEHIND_DEGREES) return "behind you";
        return view.angleDegrees < 0 ? "to your left" : "to your right";
    }

    // Tells the companion of hostile characters it comes to see, to hear or to have close by, and of traps it sees
    void SpotDangers(RE::Actor* companion) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!companion || !player || companion->IsDead() || !companion->Is3DLoaded()) return;
        // Without the physics of the game the walls cannot be checked: then nothing is told at all
        const auto* cell = companion->GetParentCell();
        const bool canCastRays = HasGameAddress(CELL_PHYSICS_ID) && HasGameAddress(PHYSICS_LOCK_ID) && HasGameAddress(PHYSICS_UNLOCK_ID);
        auto* physics = cell && canCastRays ? cell->GetbhkWorld() : nullptr;
        if (!physics) return;

        std::vector<Spot> hostiles;
        std::vector<Spot> traps;
        const RE::NiPoint3 feet = companion->GetPosition();
        // A fight has events of its own, and who fights knows the enemy is there
        const bool isFighting = player->IsInCombat() || companion->IsInCombat();
        if (!isFighting && HasGameAddress(HOSTILE_TO_ACTOR_ID)) {
            ForEachReferenceAround(companion, SPOT_RADIUS_UNITS, [&](RE::TESObjectREFR* reference) {
                if (!reference || reference == companion || reference == player || reference->IsDisabled() || reference->IsMarkedForDeletion()) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                auto* actor = reference->As<RE::Actor>();
                if (!actor || !actor->Is3DLoaded() || actor->IsDead() || actor->IsPlayerTeammate()) return RE::BSContainer::ForEachResult::kContinue;
                if (!actor->IsHostileToActor(player) && !actor->IsHostileToActor(companion)) return RE::BSContainer::ForEachResult::kContinue;
                const auto* actorValues = actor->AsActorValueOwner();
                if (actorValues && actorValues->GetActorValue(RE::ActorValue::kConfidence) < SPOT_LEAST_CONFIDENCE) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                Spot spot;
                spot.referenceID = actor->GetFormID();
                spot.position = actor->GetPosition();
                spot.view = GetViewFrom(companion, spot.position);
                spot.name = CleanText(ViewOf(actor->GetDisplayFullName()));
                spot.actor = actor;
                spot.distanceUnits = feet.GetDistance(spot.position);
                if (!spot.name.empty()) hostiles.push_back(std::move(spot));
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }
        const float trapRadius = static_cast<float>(DEFAULT_TRAP_RADIUS_METERS) * GAME_UNITS_PER_METER;
        for (const auto& trap : CollectTraps(companion, trapRadius, DEFAULT_MAX_TRAPS)) {
            auto* form = RE::TESForm::LookupByID(trap.referenceID);
            auto* reference = form ? form->AsReference() : nullptr;
            if (!reference || std::abs(trap.view.angleDegrees) > SPOT_HALF_ANGLE_DEGREES) continue;
            Spot spot;
            spot.referenceID = trap.referenceID;
            spot.position = reference->GetPosition();
            spot.view = trap.view;
            spot.name = trap.name;
            traps.push_back(std::move(spot));
        }
        if (hostiles.empty() && traps.empty()) return;

        // The traps that were told already are left out before the walls are checked, which is the costly part
        const auto now = std::chrono::steady_clock::now();
        {
            const std::lock_guard<std::mutex> guard(eventLock);
            std::erase_if(traps, [&](const Spot& spot) { return spottedTraps.contains(spot.referenceID); });
        }
        if (hostiles.empty() && traps.empty()) return;
        {
            const float scale = HasGameAddress(PHYSICS_SCALE_ID) ? RE::bhkWorld::GetWorldScale() : DEFAULT_PHYSICS_SCALE;
            const RE::NiPoint3 eyes(feet.x, feet.y, feet.z + SIGHT_EYE_UNITS);
            const RE::BSReadLockGuard guard(physics->worldLock);
            for (auto& spot : hostiles) {
                spot.sense = GetSenseOf(physics, scale, eyes, spot.actor, spot.view, spot.distanceUnits, SPOT_HALF_ANGLE_DEGREES);
            }
            std::erase_if(traps, [&](const Spot& spot) { return !IsInSight(physics, scale, eyes, spot.position); });
        }

        // What is new: whom the companion sees, whom it has close by and whom it hears. Everyone is told once and
        // then not again for a while, unless a sound has come within reach
        std::vector<Spot> characters;
        std::vector<Spot> closeOnes;
        std::vector<Spot> heardOnes;
        {
            const std::lock_guard<std::mutex> guard(eventLock);
            if (spottedCharacters.size() > MAX_REMEMBERED_SPOTS) spottedCharacters.clear();
            if (sensedCharacters.size() > MAX_REMEMBERED_SPOTS) sensedCharacters.clear();
            if (spottedTraps.size() > MAX_REMEMBERED_SPOTS) spottedTraps.clear();
            for (const auto& spot : hostiles) {
                const auto seenBefore = spottedCharacters.find(spot.referenceID);
                const bool wasSeenLately = seenBefore != spottedCharacters.end() && SecondsBetween(seenBefore->second, now) < SPOT_AGAIN_SECONDS;
                if (spot.sense == SENSE_SEEN) {
                    if (wasSeenLately) continue;
                    spottedCharacters[spot.referenceID] = now;
                    characters.push_back(spot);
                } else if (spot.sense == SENSE_CLOSE || spot.sense == SENSE_HEARD) {
                    const auto sensedBefore = sensedCharacters.find(spot.referenceID);
                    const bool wasSensedLately =
                        sensedBefore != sensedCharacters.end() && SecondsBetween(sensedBefore->second.first, now) < SPOT_AGAIN_SECONDS;
                    const bool hasComeClose = wasSensedLately && sensedBefore->second.second == SENSE_HEARD && spot.sense == SENSE_CLOSE;
                    if (wasSensedLately && !hasComeClose) continue;
                    // Whom the companion has just seen it knows of: a sound from there is nothing new. Within reach it is
                    if (spot.sense == SENSE_HEARD && wasSeenLately) continue;
                    sensedCharacters[spot.referenceID] = std::make_pair(now, spot.sense);
                    if (spot.sense == SENSE_CLOSE) {
                        closeOnes.push_back(spot);
                    } else {
                        heardOnes.push_back(spot);
                    }
                }
            }
            for (const auto& spot : traps) spottedTraps.insert(spot.referenceID);
        }
        std::sort(characters.begin(), characters.end(), [](const Spot& left, const Spot& right) { return left.view.distanceMeters < right.view.distanceMeters; });
        if (characters.size() == 1) {
            KeepEvent("You see a hostile " + characters.front().name + " " + DescribeSpot(characters.front().view) + ".", true, 0);
        } else if (!characters.empty()) {
            std::string text = "You see hostile characters: ";
            std::size_t shown = 0;
            for (const auto& spot : characters) {
                if (shown == MAX_NAMED_SPOTS) break;
                if (shown > 0) text += ", ";
                text += spot.name + " (" + DescribeSpot(spot.view) + ")";
                ++shown;
            }
            if (characters.size() > shown) text += std::format(" and {} more", characters.size() - shown);
            KeepEvent(text + ".", true, 0);
        }
        // Who is within reach is known by name. A sound is only a sound: the nearest one is told
        for (const auto& spot : closeOnes) KeepEvent("A hostile " + spot.name + " is right " + DescribeSide(spot.view) + ", within reach.", true, 0);
        if (!heardOnes.empty()) {
            std::sort(heardOnes.begin(), heardOnes.end(), [](const Spot& left, const Spot& right) { return left.distanceUnits < right.distanceUnits; });
            const auto& nearest = heardOnes.front();
            std::string text = "You hear something " + DescribeSide(nearest.view) + std::format(", about {} m away", nearest.view.distanceMeters);
            if (heardOnes.size() > 1) text += std::format(" (and {} more sounds around you)", heardOnes.size() - 1);
            KeepEvent(text + ".", true, 0);
        }
        // Traps of one kind come in one sentence, with where the nearest one is: a room full of oil on the floor came
        // as eight sentences at a time
        std::sort(traps.begin(), traps.end(), [](const Spot& left, const Spot& right) { return left.view.distanceMeters < right.view.distanceMeters; });
        std::vector<std::string> toldTrapNames;
        for (const auto& spot : traps) {
            if (std::find(toldTrapNames.begin(), toldTrapNames.end(), spot.name) != toldTrapNames.end()) continue;
            toldTrapNames.push_back(spot.name);
            const auto sameCount = std::count_if(traps.begin(), traps.end(), [&](const Spot& other) { return other.name == spot.name; });
            if (sameCount == 1) {
                KeepEvent("You see a trap (" + spot.name + ") " + DescribeSpot(spot.view) + ".", true, 0);
            } else {
                KeepEvent(std::format("You see {} traps ({}), the nearest {}.", sameCount, spot.name, DescribeSpot(spot.view)), true, 0);
            }
        }
    }

    // ------------------------------------------------------------------ watch
    // What an AI that waits for something to happen should hear of, and no event of the game tells: the companion
    // falls behind the player or gets stuck, its arrows or potions run low, the player gets a bounty or a level.
    // Looked at with the slow checks. Each of these becomes an event like any other

    constexpr float BEHIND_FOOT_START_UNITS = 40.0f * GAME_UNITS_PER_METER;
    constexpr float BEHIND_FOOT_END_UNITS = 10.0f * GAME_UNITS_PER_METER;
    constexpr float BEHIND_RIDE_START_UNITS = 80.0f * GAME_UNITS_PER_METER;
    constexpr float BEHIND_RIDE_END_UNITS = 20.0f * GAME_UNITS_PER_METER;
    // After the companion was told that it fell behind, it is not told again for this long
    constexpr std::int64_t BEHIND_QUIET_SECONDS = 60;
    // A companion that is to follow but is not in the loaded area for this many checks in a row was left behind
    constexpr std::int32_t BEHIND_UNLOADED_CHECKS = 5;
    // The horse left behind: the player rides, the companion is to follow and walks, and its own horse is further
    // away from it than this (or not in the loaded area), for this many checks in a row. Told once, and again only
    // after that was over in between and after HORSE_AWAY_QUIET_SECONDS
    constexpr float HORSE_AWAY_UNITS = 40.0f * GAME_UNITS_PER_METER;
    constexpr std::int32_t HORSE_AWAY_CHECKS = 5;
    constexpr std::int64_t HORSE_AWAY_QUIET_SECONDS = 120;
    // Stuck: the animation says the companion moves (see STUCK_SPEED_VARIABLE), but it got less far than this
    // between two checks, STUCK_CHECKS times in a row
    constexpr float STUCK_UNITS = 50.0f;
    constexpr std::int32_t STUCK_CHECKS = 2;
    constexpr std::int64_t STUCK_QUIET_SECONDS = 60;
    constexpr const char* STUCK_SPEED_VARIABLE = "Speed";
    constexpr float STUCK_SPEED = 1.0f;
    constexpr std::int32_t AMMO_FIRST_WARNING = 20;
    constexpr std::int32_t AMMO_SECOND_WARNING = 5;
    // In a fight with less than this left and no potion for it, the companion is in need
    constexpr float NEED_PERCENT = 50.0f;
    constexpr std::int32_t AMMO_IS_NONE = 0;
    constexpr std::int32_t AMMO_IS_ARROW = 1;
    constexpr std::int32_t AMMO_IS_BOLT = 2;

    struct WatchNews {
        std::string text;
        bool isUrgent = false;
    };

    struct WatchState {
        RE::FormID companionID = 0;
        // Falling behind
        bool wasFollowing = false;
        bool isBehind = false;
        bool hasBehindNews = false;
        std::chrono::steady_clock::time_point behindNewsTime;
        std::int32_t unloadedChecks = 0;
        // Told already that it is not where the player is, for this stay outside the loaded area
        bool isLeftBehind = false;
        // The horse left behind
        std::int32_t horseAwayChecks = 0;
        bool isHorseAwayTold = false;
        bool hasHorseAwayNews = false;
        std::chrono::steady_clock::time_point horseAwayNewsTime;
        // Getting stuck
        bool hasSpot = false;
        bool wasMoving = false;
        RE::NiPoint3 spot;
        std::int32_t stuckChecks = 0;
        bool hasStuckNews = false;
        std::chrono::steady_clock::time_point stuckNewsTime;
        // Arrows and potions
        bool hasSupplies = false;
        std::int32_t ammoKind = AMMO_IS_NONE;
        std::int32_t ammoCount = 0;
        std::int32_t healPotions = 0;
        std::int32_t magickaPotions = 0;
        bool isHealthNeedTold = false;
        bool isMagickaNeedTold = false;
        // The player
        bool hasLevel = false;
        std::uint16_t playerLevel = 0;
        bool hasBounties = false;
        std::unordered_map<RE::FormID, float> bounties;
    };

    std::mutex watchLock;
    WatchState watchState;

    // What a potion gives back, as the actor value of its costliest effect: kHealth for a healing potion, kMagicka for a
    // magicka potion. kNone for everything else (poison, food, a potion that fortifies or resists)
    RE::ActorValue GetRestoredValue(RE::AlchemyItem* potion) {
        if (!potion || potion->IsPoison() || potion->IsFood()) return RE::ActorValue::kNone;
        const RE::Effect* strongest = nullptr;
        for (const auto* effect : potion->effects) {
            if (effect && effect->baseEffect && (!strongest || effect->cost > strongest->cost)) strongest = effect;
        }
        if (!strongest) return RE::ActorValue::kNone;
        const auto* base = strongest->baseEffect;
        if (base->GetArchetype() != RE::EffectSetting::Archetype::kValueModifier || base->IsDetrimental() || base->IsHostile()) return RE::ActorValue::kNone;
        return base->data.primaryAV;
    }

    float GetPercentOf(RE::Actor* actor, RE::ActorValue value) {
        const auto* values = actor ? actor->AsActorValueOwner() : nullptr;
        if (!values) return FULL_PERCENT;
        const float full = values->GetPermanentActorValue(value);
        return full > 0.0f ? std::clamp(values->GetActorValue(value) / full * FULL_PERCENT, 0.0f, FULL_PERCENT) : FULL_PERCENT;
    }

    // Only call this while watchLock is held. isFollowing: the companion is to follow the player (see TickWatch)
    void CheckProgressLocked(RE::Actor* companion, RE::PlayerCharacter* player, bool isFollowing, std::chrono::steady_clock::time_point now,
                             std::vector<WatchNews>& news) {
        WatchState& state = watchState;
        const bool isLoaded = companion->Is3DLoaded();
        const bool isFighting = companion->IsInCombat();
        const bool isRiding = companion->IsOnMount();
        const auto* values = companion->AsActorValueOwner();
        const bool isWaiting = values && values->GetActorValue(RE::ActorValue::kWaitingForPlayer) > 0.0f;
        const bool canTellBehind = !state.hasBehindNews || SecondsBetween(state.behindNewsTime, now) >= BEHIND_QUIET_SECONDS;

        // Falling behind: only a companion that is to follow the player can
        if (!isFollowing || isWaiting || isFighting) {
            state.wasFollowing = false;
            state.unloadedChecks = 0;
            state.isLeftBehind = false;
        } else if (!isLoaded) {
            // Not where the player is: the player went through a door or far ahead. That is news of its own, told
            // once for every time it happens, also when the distance was told a moment ago. (In a test the companion
            // was told that it was 44 m behind, the player went through a door ten seconds later, and nothing came)
            state.wasFollowing = true;
            state.isBehind = true;
            if (!state.isLeftBehind && ++state.unloadedChecks >= BEHIND_UNLOADED_CHECKS) {
                state.isLeftBehind = true;
                state.hasBehindNews = true;
                state.behindNewsTime = now;
                news.push_back(
                    {"You have fallen behind: the player went on without you (through a door, or far ahead), and you are no longer where the player is.",
                     false});
            }
        } else {
            state.unloadedChecks = 0;
            state.isLeftBehind = false;
            const float distance = companion->GetPosition().GetDistance(player->GetPosition());
            const float startUnits = isRiding ? BEHIND_RIDE_START_UNITS : BEHIND_FOOT_START_UNITS;
            const float endUnits = isRiding ? BEHIND_RIDE_END_UNITS : BEHIND_FOOT_END_UNITS;
            if (!state.wasFollowing) {
                // It begins to follow (after a wait, a fight, a walk): where it stands now is no news
                state.wasFollowing = true;
                state.isBehind = distance > startUnits;
            } else if (state.isBehind) {
                if (distance < endUnits) state.isBehind = false;
            } else if (distance > startUnits) {
                state.isBehind = true;
                if (canTellBehind) {
                    state.hasBehindNews = true;
                    state.behindNewsTime = now;
                    news.push_back({std::format("You have fallen behind: the player is about {} m away from you.", ToWholeMeters(distance)), false});
                }
            }
        }

        // The horse left behind: the player rides, and the companion walks after it without its horse
        const RE::FormID ownHorseID = eventOwnHorseID.load();
        auto* ownHorse = ownHorseID != 0 ? RE::TESForm::LookupByID<RE::Actor>(ownHorseID) : nullptr;
        const bool isHorseNear = ownHorse && ownHorse->Is3DLoaded() && ownHorse->GetPosition().GetDistance(companion->GetPosition()) <= HORSE_AWAY_UNITS;
        const bool isHorseAway =
            isFollowing && !isWaiting && !isFighting && isLoaded && !isRiding && player->IsOnMount() && ownHorse && !ownHorse->IsDead() && !isHorseNear;
        if (!isHorseAway) {
            state.horseAwayChecks = 0;
            state.isHorseAwayTold = false;
        } else if (!state.isHorseAwayTold && ++state.horseAwayChecks >= HORSE_AWAY_CHECKS) {
            state.isHorseAwayTold = true;
            if (!state.hasHorseAwayNews || SecondsBetween(state.horseAwayNewsTime, now) >= HORSE_AWAY_QUIET_SECONDS) {
                state.hasHorseAwayNews = true;
                state.horseAwayNewsTime = now;
                if (ownHorse->Is3DLoaded()) {
                    const float horseDistance = ownHorse->GetPosition().GetDistance(companion->GetPosition());
                    news.push_back(
                        {std::format("The player rides and you are on foot: your horse is about {} m away from you.", ToWholeMeters(horseDistance)), false});
                } else {
                    news.push_back({"The player rides and you are on foot: your horse is not where you are.", false});
                }
            }
        }

        // Getting stuck: walking without getting anywhere
        if (!isLoaded || isRiding || isFighting) {
            state.hasSpot = false;
            state.wasMoving = false;
            state.stuckChecks = 0;
            return;
        }
        float speed = 0.0f;
        const bool isMoving = companion->GetGraphVariableFloat(STUCK_SPEED_VARIABLE, speed) && speed >= STUCK_SPEED;
        const RE::NiPoint3 spot = companion->GetPosition();
        if (state.hasSpot && state.wasMoving && isMoving) {
            const float east = spot.x - state.spot.x;
            const float north = spot.y - state.spot.y;
            if (std::sqrt(east * east + north * north) < STUCK_UNITS) {
                ++state.stuckChecks;
            } else {
                state.stuckChecks = 0;
            }
        } else {
            state.stuckChecks = 0;
        }
        state.hasSpot = true;
        state.wasMoving = isMoving;
        state.spot = spot;
        if (state.stuckChecks == STUCK_CHECKS && (!state.hasStuckNews || SecondsBetween(state.stuckNewsTime, now) >= STUCK_QUIET_SECONDS)) {
            state.hasStuckNews = true;
            state.stuckNewsTime = now;
            news.push_back({"You are stuck: you have been walking for some seconds without getting anywhere.", false});
        }
    }

    // Only call this while watchLock is held
    void CheckSuppliesLocked(RE::Actor* companion, std::vector<WatchNews>& news) {
        WatchState& state = watchState;
        if (!companion->Is3DLoaded()) return;
        std::int32_t arrows = 0;
        std::int32_t bolts = 0;
        std::int32_t healPotions = 0;
        std::int32_t magickaPotions = 0;
        for (const auto& [object, entry] : companion->GetInventory()) {
            if (!object || entry.first <= 0) continue;
            if (auto* ammo = object->As<RE::TESAmmo>()) {
                (ammo->IsBolt() ? bolts : arrows) += entry.first;
            } else if (auto* potion = object->As<RE::AlchemyItem>()) {
                const RE::ActorValue restored = GetRestoredValue(potion);
                if (restored == RE::ActorValue::kHealth) healPotions += entry.first;
                if (restored == RE::ActorValue::kMagicka) magickaPotions += entry.first;
            }
        }
        // The ammunition that counts is the kind the companion has on (arrows or bolts). With none on, the kind it had last
        std::int32_t ammoKind = state.ammoKind;
        if (auto* current = companion->GetCurrentAmmo()) ammoKind = current->IsBolt() ? AMMO_IS_BOLT : AMMO_IS_ARROW;
        const std::int32_t ammoCount = ammoKind == AMMO_IS_BOLT ? bolts : (ammoKind == AMMO_IS_ARROW ? arrows : 0);
        const RE::TESForm* rightHand = companion->GetEquippedObject(false);
        const RE::TESForm* leftHand = companion->GetEquippedObject(true);
        const bool usesSpells = (rightHand && rightHand->Is(RE::FormType::Spell)) || (leftHand && leftHand->Is(RE::FormType::Spell));

        if (state.hasSupplies) {
            const std::string ammoName = ammoKind == AMMO_IS_BOLT ? "bolts" : "arrows";
            if (ammoKind != AMMO_IS_NONE && ammoKind == state.ammoKind && ammoCount < state.ammoCount) {
                if (ammoCount <= 0) {
                    news.push_back({"You are out of " + ammoName + ".", true});
                } else if ((state.ammoCount > AMMO_SECOND_WARNING && ammoCount <= AMMO_SECOND_WARNING) ||
                           (state.ammoCount > AMMO_FIRST_WARNING && ammoCount <= AMMO_FIRST_WARNING)) {
                    news.push_back({std::format("You are low on {}: {} left.", ammoName, ammoCount), false});
                }
            }
            if (healPotions < state.healPotions) {
                if (healPotions == 0) {
                    news.push_back({"You are out of healing potions.", false});
                } else if (healPotions == 1) {
                    news.push_back({"You are low on healing potions: 1 left.", false});
                }
            }
            if (usesSpells && magickaPotions < state.magickaPotions) {
                if (magickaPotions == 0) {
                    news.push_back({"You are out of magicka potions.", false});
                } else if (magickaPotions == 1) {
                    news.push_back({"You are low on magicka potions: 1 left.", false});
                }
            }
        }
        state.hasSupplies = true;
        state.ammoKind = ammoKind;
        state.ammoCount = ammoCount;
        state.healPotions = healPotions;
        state.magickaPotions = magickaPotions;

        // In need: hurt in a fight with nothing to drink against it. Told once, and again after it was better in between
        const bool isFighting = companion->IsInCombat();
        const bool isHealthNeeded = isFighting && healPotions == 0 && GetPercentOf(companion, RE::ActorValue::kHealth) < NEED_PERCENT;
        if (isHealthNeeded && !state.isHealthNeedTold) news.push_back({"You are below half your health in a fight and have no healing potion.", true});
        state.isHealthNeedTold = isHealthNeeded;
        const bool isMagickaNeeded = isFighting && usesSpells && magickaPotions == 0 && GetPercentOf(companion, RE::ActorValue::kMagicka) < NEED_PERCENT;
        if (isMagickaNeeded && !state.isMagickaNeedTold) news.push_back({"You are below half your magicka in a fight and have no magicka potion.", true});
        state.isMagickaNeedTold = isMagickaNeeded;
    }

    // Only call this while watchLock is held: a level the player reached, and a bounty the player got
    void CheckPlayerLocked(RE::PlayerCharacter* player, std::vector<WatchNews>& news) {
        WatchState& state = watchState;
        const std::uint16_t level = player->GetLevel();
        if (state.hasLevel && level > state.playerLevel) news.push_back({std::format("The player reached level {}.", level), false});
        state.hasLevel = true;
        state.playerLevel = level;

        std::unordered_map<RE::FormID, float> bounties;
        for (const auto& entry : player->GetCrimeValue().crimeGoldMap) {
            const auto* faction = entry.first;
            if (!faction) continue;
            const float bounty = entry.second.violentCur + entry.second.nonViolentCur;
            bounties[faction->GetFormID()] = bounty;
            if (!state.hasBounties) continue;
            const auto known = state.bounties.find(faction->GetFormID());
            const float before = known != state.bounties.end() ? known->second : 0.0f;
            if (bounty < before + 1.0f) continue;
            const std::string factionName = CleanText(ViewOf(faction->GetFullName()));
            news.push_back({std::format("The player got a bounty of {} gold{} ({} in all there).", FormatNumber(bounty - before),
                                        factionName.empty() ? std::string() : " with " + factionName, FormatNumber(bounty)),
                            false});
        }
        state.hasBounties = true;
        state.bounties = std::move(bounties);
    }

    // See "watch" above. Called with the slow checks
    void CheckWatch(RE::Actor* companion) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        const auto now = std::chrono::steady_clock::now();
        std::vector<WatchNews> news;
        {
            const std::lock_guard<std::mutex> guard(watchLock);
            const RE::FormID companionID = companion && !companion->IsDead() ? companion->GetFormID() : 0;
            if (isWatchForgotten.exchange(false) || watchState.companionID != companionID) {
                // Another game or another companion: nothing of what was seen before counts
                watchState = WatchState();
                watchState.companionID = companionID;
            }
            if (companionID != 0) {
                CheckProgressLocked(companion, player, isCompanionFollowing.load(), now, news);
                CheckSuppliesLocked(companion, news);
            }
            CheckPlayerLocked(player, news);
        }
        for (const auto& entry : news) KeepEvent(entry.text, entry.isUrgent, 0);
    }

    // What TickEvents does only every few seconds, because it costs more than a look at the player
    void RunSlowChecks(RE::Actor* companion) {
        if (areEventsQuiet.load()) return;
        const auto now = std::chrono::steady_clock::now();
        {
            const std::lock_guard<std::mutex> guard(eventLock);
            if (hasSlowCheck && SecondsBetween(lastSlowCheck, now) < SLOW_CHECK_SECONDS) return;
            hasSlowCheck = true;
            lastSlowCheck = now;
        }
        CheckQuests();
        SpotDangers(companion);
        CheckWatch(companion);
    }

    // ------------------------------------------------------------------ books
    // The text of a book or a note, for a companion that reads it. The game keeps it as the description of the book,
    // with its own marks in it: [pagebreak], <font ...>, <p align=...>, <br>, pictures. They are taken out, what is
    // left is cut into paragraphs, and the paragraphs into pages of about BOOK_PAGE_BYTES.
    // A note can name a character or a place of a quest with <Alias=Name>. The game fills that in when it shows
    // the note; here it stays as [Name], because the plugin does not know which quest the note belongs to

    constexpr std::size_t BOOK_PAGE_BYTES = 3000;
    // A page goes to the script in pieces: one long text is more than the way through the script is known to take
    constexpr std::size_t BOOK_PIECE_BYTES = 400;
    constexpr std::int32_t BOOK_IS_BOOK = 0;
    constexpr std::int32_t BOOK_IS_NOTE = 1;
    constexpr std::int32_t BOOK_IS_SPELL_TOME = 2;
    // How a piece joins what came before it: it begins a paragraph, it goes on after a blank, or it goes on right away
    constexpr std::int32_t PIECE_BEGINS_PARAGRAPH = 1;
    constexpr std::int32_t PIECE_FOLLOWS_BLANK = 0;
    constexpr std::int32_t PIECE_FOLLOWS_DIRECTLY = 2;
    constexpr std::string_view PAGE_BREAK_MARK = "[pagebreak]";

    struct BookPiece {
        std::int32_t join = PIECE_BEGINS_PARAGRAPH;
        std::string text;
    };

    // Adds what was collected as a paragraph, with single blanks and none at its ends
    void EndParagraph(std::string& collected, std::vector<std::string>& paragraphs) {
        std::string paragraph;
        bool isBlankPending = false;
        for (const char letter : collected) {
            if (static_cast<unsigned char>(letter) <= 0x20) {
                isBlankPending = !paragraph.empty();
                continue;
            }
            if (isBlankPending) paragraph += ' ';
            isBlankPending = false;
            paragraph += letter;
        }
        collected.clear();
        if (!paragraph.empty()) paragraphs.push_back(std::move(paragraph));
    }

    // The paragraphs of a book text without the marks of the game
    std::vector<std::string> ReadParagraphs(std::string_view raw) {
        const std::string text = IsValidUtf(raw) ? std::string(raw) : FromWindowsText(raw);
        std::vector<std::string> paragraphs;
        std::string collected;
        std::size_t index = 0;
        while (index < text.size()) {
            const char letter = text[index];
            if (letter == '\n') {
                EndParagraph(collected, paragraphs);
                ++index;
                continue;
            }
            if (letter == '[' && StartsWithIgnoreCase(std::string_view(text).substr(index), PAGE_BREAK_MARK)) {
                EndParagraph(collected, paragraphs);
                index += PAGE_BREAK_MARK.size();
                continue;
            }
            if (letter == '<') {
                const std::size_t end = text.find('>', index);
                if (end != std::string::npos) {
                    const std::string_view tag = std::string_view(text).substr(index + 1, end - index - 1);
                    const bool isBreak = StartsWithIgnoreCase(tag, "br") || StartsWithIgnoreCase(tag, "/p") || EqualsIgnoreCase(tag, "p") ||
                                         StartsWithIgnoreCase(tag, "p ");
                    const std::size_t equals = tag.find('=');
                    if (isBreak) {
                        EndParagraph(collected, paragraphs);
                    } else if ((StartsWithIgnoreCase(tag, "alias") || StartsWithIgnoreCase(tag, "global")) && equals != std::string_view::npos) {
                        collected += '[';
                        collected += tag.substr(equals + 1);
                        collected += ']';
                    } else {
                        // Fonts, pictures and the like. A blank keeps the words on both sides apart
                        collected += ' ';
                    }
                    index = end + 1;
                    continue;
                }
            }
            collected += letter;
            ++index;
        }
        EndParagraph(collected, paragraphs);
        return paragraphs;
    }

    // Cuts a paragraph into pieces of at most BOOK_PIECE_BYTES, at a blank where there is one
    void AddPieces(const std::string& paragraph, std::vector<BookPiece>& pieces) {
        std::size_t start = 0;
        std::int32_t join = PIECE_BEGINS_PARAGRAPH;
        while (start < paragraph.size()) {
            std::size_t end = paragraph.size();
            std::int32_t nextJoin = PIECE_FOLLOWS_BLANK;
            std::size_t nextStart = end;
            if (end - start > BOOK_PIECE_BYTES) {
                end = start + BOOK_PIECE_BYTES;
                const std::size_t blank = paragraph.rfind(' ', end);
                if (blank != std::string::npos && blank > start) {
                    end = blank;
                    nextStart = blank + 1;
                } else {
                    // No blank to cut at. Do not cut a letter of several bytes in the middle
                    while (end > start + 1 && IsFollowByte(static_cast<unsigned char>(paragraph[end]))) --end;
                    nextStart = end;
                    nextJoin = PIECE_FOLLOWS_DIRECTLY;
                }
            }
            BookPiece piece;
            piece.join = join;
            piece.text = paragraph.substr(start, end - start);
            pieces.push_back(std::move(piece));
            join = nextJoin;
            start = nextStart;
        }
    }

    // The pieces of a book text put on pages of about BOOK_PAGE_BYTES
    std::vector<std::vector<BookPiece>> ReadBookPages(std::string_view raw) {
        std::vector<BookPiece> pieces;
        for (const auto& paragraph : ReadParagraphs(raw)) AddPieces(paragraph, pieces);
        std::vector<std::vector<BookPiece>> pages;
        std::size_t pageBytes = 0;
        for (auto& piece : pieces) {
            if (pages.empty() || (pageBytes > 0 && pageBytes + piece.text.size() > BOOK_PAGE_BYTES)) {
                pages.emplace_back();
                pageBytes = 0;
            }
            pageBytes += piece.text.size();
            pages.back().push_back(std::move(piece));
        }
        return pages;
    }

    void OnMessage(SKSE::MessagingInterface::Message* message) {
        if (!message) return;
        // What was said before a save is loaded or a new game starts belongs to another game
        if (message->type == SKSE::MessagingInterface::kPreLoadGame || message->type == SKSE::MessagingInterface::kNewGame) {
            ForgetHeardLines();
            ForgetEvents();
        }
        // Loading is over, or the new game runs: from a moment later on events are kept again
        if (message->type == SKSE::MessagingInterface::kPostLoadGame || message->type == SKSE::MessagingInterface::kNewGame) {
            AllowEventsSoon();
        }
        if (message->type == SKSE::MessagingInterface::kDataLoaded) {
            InstallEventListeners();
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

    // The lines of GetNearbyPlaces and GetVisiblePlaces. halfAngleDegrees: NO_SIGHT_FILTER = every place around,
    // otherwise only the places that lie within this many degrees to the left and right of where the viewer faces
    // and have nothing solid between them and the eyes of the viewer
    std::vector<std::string> GetPlaceLines(std::int32_t radiusMeters, std::int32_t maxMarkers, std::int32_t maxDoors, RE::Actor* origin,
                                           std::int32_t halfAngleDegrees) {
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
        if (halfAngleDegrees > NO_SIGHT_FILTER) {
            std::vector<Place> ahead;
            const float placeSightMeters = GetSightUnitsOf(viewer) * SIGHT_PLACE_TIMES / GAME_UNITS_PER_METER;
            for (auto& place : places) {
                if (std::abs(place.view.angleDegrees) <= halfAngleDegrees && static_cast<float>(place.view.distanceMeters) <= placeSightMeters) {
                    ahead.push_back(std::move(place));
                }
            }
            places = std::move(ahead);
            // Without the physics of the game the walls cannot be checked: then it stays with the direction
            const auto* cell = viewer->Is3DLoaded() ? viewer->GetParentCell() : nullptr;
            const bool canCastRays = HasGameAddress(CELL_PHYSICS_ID) && HasGameAddress(PHYSICS_LOCK_ID) && HasGameAddress(PHYSICS_UNLOCK_ID);
            auto* physics = cell && canCastRays ? cell->GetbhkWorld() : nullptr;
            if (physics) {
                const float scale = HasGameAddress(PHYSICS_SCALE_ID) ? RE::bhkWorld::GetWorldScale() : DEFAULT_PHYSICS_SCALE;
                const RE::NiPoint3 feet = viewer->GetPosition();
                const RE::NiPoint3 eyes(feet.x, feet.y, feet.z + SIGHT_EYE_UNITS);
                std::vector<Place> seen;
                const RE::BSReadLockGuard guard(physics->worldLock);
                for (auto& place : places) {
                    auto* form = place.referenceID != 0 ? RE::TESForm::LookupByID(place.referenceID) : nullptr;
                    auto* reference = form ? form->AsReference() : nullptr;
                    // A place the game cannot find any more is not held back
                    if (!reference || IsInSight(physics, scale, eyes, reference->GetPosition())) seen.push_back(std::move(place));
                }
                places = std::move(seen);
            }
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
        logger::debug("GetPlaceLines: {} places sent", lines.size());
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
        return GetPlaceLines(radiusMeters, maxMarkers, maxDoors, origin, NO_SIGHT_FILTER);
    }

    // Like GetNearbyPlaces, but only the places the viewer can see: within halfAngleDegrees to the left and right of
    // where it faces, and with no wall between its eyes and the place (rays through the physics of the game).
    // Without this a companion "saw" doors through walls
    std::vector<std::string> GetVisiblePlaces(RE::StaticFunctionTag*, std::int32_t radiusMeters, std::int32_t maxMarkers, std::int32_t maxDoors,
                                              RE::Actor* origin, std::int32_t halfAngleDegrees) {
        return GetPlaceLines(radiusMeters, maxMarkers, maxDoors, origin, std::max(halfAngleDegrees, 0));
    }

    // The things close to the viewer (origin, or the player - see GetViewer): furniture first, then containers,
    // then items that lie around, each nearest first, at most maxPerKind of each kind within radiusMeters.
    // One text per thing, its parts separated by tabs:
    // kind (0 = furniture, 1 = container, 2 = item), ref ID as eight hex digits, distance to the viewer in meters,
    // angle from where the viewer faces in degrees (negative = left),
    // for furniture 1 if a character can sit on it and 2 if it can sleep on it, for a container 1 if it is locked, otherwise 2 x (the number of different items in it + 1), name
    std::vector<std::string> GetNearbyThings(RE::StaticFunctionTag*, std::int32_t radiusMeters, std::int32_t maxPerKind, RE::Actor* origin) {
        std::vector<std::string> lines;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return lines;
        auto* viewer = GetViewer(origin, player);
        const float radius = static_cast<float>(radiusMeters > 0 ? radiusMeters : DEFAULT_THING_RADIUS_METERS) * GAME_UNITS_PER_METER;
        const std::size_t limit = maxPerKind > 0 ? static_cast<std::size_t>(maxPerKind) : DEFAULT_MAX_THINGS_PER_KIND;

        for (const auto& thing : CollectThings(viewer, radius, limit)) {
            KeepHeight(viewer, thing.referenceID, thing.view.upUnits);
            lines.push_back(GetThingLine(thing));
        }
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
        for (const auto& trap : CollectTraps(viewer, radius, limit)) {
            KeepHeight(viewer, trap.referenceID, trap.view.upUnits);
            lines.push_back(GetThingLine(trap));
        }
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
            // Nor is one the game hides or keeps somewhere else (see GetAbsence)
            if (!GetAbsence(actor, viewer).empty()) return RE::BSContainer::ForEachResult::kContinue;
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
        for (const auto& match : FindAround(player, viewer, words, limit)) {
            KeepHeight(viewer, match.referenceID, match.view.upUnits);
            lines.push_back(GetThingLine(match));
        }
        logger::debug("FindByName: {} matches sent", lines.size());
        return lines;
    }

    // What kind of ammunition an item is: 0 = none, 1 = an arrow, 2 = a bolt for a crossbow. The script cannot tell
    // arrows from bolts itself. For the favorites of the companion, which only fit the weapon of their kind
    std::int32_t GetAmmoKind(RE::StaticFunctionTag*, RE::TESForm* item) {
        auto* ammo = item ? item->As<RE::TESAmmo>() : nullptr;
        if (!ammo) return 0;
        return ammo->IsBolt() ? 2 : 1;
    }

    // Whether this is a piece of furniture a character can lie down and sleep on. The script cannot tell that itself
    bool IsBed(RE::StaticFunctionTag*, RE::TESObjectREFR* reference) {
        const auto* base = reference ? reference->GetBaseObject() : nullptr;
        const auto* furniture = base ? base->As<RE::TESFurniture>() : nullptr;
        return furniture && furniture->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSleep);
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

    // Mute: from now on the sentences the game lets this character say on its own are cut off as they begin, and their
    // subtitles are not shown (None = nobody is muted any more). Grunts and cries without a subtitle stay, so do the
    // own lines of the game mod (say) and what the character answers in the dialogue menu. Returns whether the plugin
    // can do that at all (it has to listen to the subtitles for it, see IsHearing)
    bool SetMutedSpeaker(RE::StaticFunctionTag*, RE::Actor* speaker) {
        const RE::FormID wantedID = speaker ? speaker->GetFormID() : 0;
        if (mutedSpeakerID.exchange(wantedID) != wantedID) {
            logger::info("Muted speaker: {:08X}", wantedID);
        }
        return isHearing;
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

    // One direction of the surroundings: a ray from the chest of the character into the direction "heading" (radians,
    // clockwise from north) as far as "range" (game units), and rays down along the free stretch. Adds three numbers to
    // "values": what the ray met, how far away in meters (0 if nothing), after how many meters the ground drops away
    // (NO_DROP = it does not). Only call this while the physics are locked for reading
    void ProbeDirection(RE::bhkWorld* physics, float scale, const RE::NiPoint3& chest, float feetHeight, float heading, float range,
                        std::vector<std::int32_t>& values) {
        // The game counts directions clockwise from north, and north is where y grows
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
        float groundBefore = feetHeight;
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
            ProbeDirection(physics, scale, chest, feet.z, heading, range, values);
        }
        return values;
    }

    // Like GetSurroundings, but in a direction the script names and as far as it names: the rays fan out over
    // angleDegrees around headingDegrees (a point of the compass in degrees, 0 = north, 90 = east), from the left edge
    // of that angle to the right one, the middle ray straight into the direction. An angle of 0 is one ray, a full
    // circle (360) is SURROUNDING_DIRECTIONS rays, the first into the direction and the others clockwise from there.
    // rangeMeters: 0 = the usual range, at most MAX_LOOK_METERS. Three numbers per ray, as GetSurroundings gives them
    std::vector<std::int32_t> GetSurroundingsToward(RE::StaticFunctionTag*, RE::Actor* origin, std::int32_t rangeMeters,
                                                    std::int32_t headingDegrees, std::int32_t angleDegrees) {
        std::vector<std::int32_t> values;
        if (!origin || !origin->Is3DLoaded()) return values;
        if (!HasGameAddress(CELL_PHYSICS_ID) || !HasGameAddress(PHYSICS_LOCK_ID) || !HasGameAddress(PHYSICS_UNLOCK_ID)) return values;
        const auto* cell = origin->GetParentCell();
        auto* physics = cell ? cell->GetbhkWorld() : nullptr;
        if (!physics) return values;
        const float scale = HasGameAddress(PHYSICS_SCALE_ID) ? RE::bhkWorld::GetWorldScale() : DEFAULT_PHYSICS_SCALE;
        const std::int32_t meters = rangeMeters > 0 ? std::min(rangeMeters, MAX_LOOK_METERS) : DEFAULT_SURROUNDINGS_METERS;
        const float range = static_cast<float>(meters) * GAME_UNITS_PER_METER;
        const RE::NiPoint3 feet = origin->GetPosition();
        const RE::NiPoint3 chest(feet.x, feet.y, feet.z + RAY_HEIGHT_UNITS);
        const float middle = static_cast<float>(headingDegrees) / DEGREES_PER_RADIAN;
        const float angle = static_cast<float>(std::clamp(angleDegrees, 0, static_cast<std::int32_t>(FULL_CIRCLE_DEGREES)));

        // Where the first ray goes, how far the next one is turned to the right, and how many there are
        float firstHeading = middle;
        float turn = 0.0f;
        std::int32_t rayCount = 1;
        if (angle >= FULL_CIRCLE_DEGREES) {
            rayCount = SURROUNDING_DIRECTIONS;
            turn = 2.0f * std::numbers::pi_v<float> / static_cast<float>(SURROUNDING_DIRECTIONS);
        } else if (angle > 0.0f) {
            const auto raysPerSide = static_cast<std::int32_t>(std::ceil(angle / 2.0f / LOOK_RAY_STEP_DEGREES));
            rayCount = std::clamp(2 * raysPerSide + 1, MIN_LOOK_RAYS, MAX_LOOK_RAYS);
            turn = angle / DEGREES_PER_RADIAN / static_cast<float>(rayCount - 1);
            firstHeading = middle - angle / DEGREES_PER_RADIAN / 2.0f;
        }

        const RE::BSReadLockGuard guard(physics->worldLock);
        for (std::int32_t index = 0; index < rayCount; ++index) {
            ProbeDirection(physics, scale, chest, feet.z, firstHeading + static_cast<float>(index) * turn, range, values);
        }
        return values;
    }

    // How high the ground is at the spot x, y (game units) close to the character "origin", for putting something there
    // that a character is to walk to. A spot at the height of the character itself hangs in the air or sticks in the
    // ground wherever the ground is not level. NO_GROUND_HEIGHT if the rays find no ground there
    float GetGroundHeight(RE::StaticFunctionTag*, RE::Actor* origin, float x, float y) {
        if (!origin || !origin->Is3DLoaded()) return NO_GROUND_HEIGHT;
        if (!HasGameAddress(CELL_PHYSICS_ID) || !HasGameAddress(PHYSICS_LOCK_ID) || !HasGameAddress(PHYSICS_UNLOCK_ID)) return NO_GROUND_HEIGHT;
        const auto* cell = origin->GetParentCell();
        auto* physics = cell ? cell->GetbhkWorld() : nullptr;
        if (!physics) return NO_GROUND_HEIGHT;
        const float scale = HasGameAddress(PHYSICS_SCALE_ID) ? RE::bhkWorld::GetWorldScale() : DEFAULT_PHYSICS_SCALE;
        const float feetHeight = origin->GetPosition().z;
        const RE::NiPoint3 top(x, y, feetHeight + GROUND_PROBE_UP_UNITS);

        const RE::BSReadLockGuard guard(physics->worldLock);
        const RayHit below = CastRay(physics, scale, top, RE::NiPoint3(x, y, feetHeight - GROUND_PROBE_DOWN_UNITS));
        if (below.isHit) return top.z - below.distance;
        // Nothing below: outdoors the ground may rise there, so that the first ray began inside the hill.
        // Indoors a ray from higher up would only find the ceiling or the floor above
        if (cell->IsInteriorCell()) return NO_GROUND_HEIGHT;
        const RE::NiPoint3 high(x, y, feetHeight + GROUND_PROBE_HIGH_UNITS);
        const RayHit above = CastRay(physics, scale, high, top);
        return above.isHit ? high.z - above.distance : NO_GROUND_HEIGHT;
    }

    // The horse (or other mount) a character sits on, or nothing if it rides nothing.
    // Scripts can only ask whether a character rides, not what
    RE::Actor* GetMount(RE::StaticFunctionTag*, RE::Actor* rider) {
        if (!rider) return nullptr;
        RE::NiPointer<RE::Actor> mount;
        if (!rider->GetMount(mount)) return nullptr;
        return mount.get();
    }

    // How many different items a dead character carries or a container holds that can be taken (see CountLoot),
    // so that a companion does not have to open every body to find out that it is empty. -1 = cannot tell
    std::int32_t GetLootCount(RE::StaticFunctionTag*, RE::TESObjectREFR* holder) { return CountLoot(holder); }

    // The five functions below exist from interface version 17 on

    // Called by the script about once a second, with the companion of the moment (None = there is none): keeps
    // what has no event of the game (the player begins or stops sneaking or fighting), and ends the hits and the
    // things taken that are counted together (see "events" above)
    void TickEvents(RE::StaticFunctionTag*, RE::Actor* companion) {
        eventCompanionID.store(companion ? companion->GetFormID() : 0);
        EndRuns(false);
        const auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        const bool isSneaking = player->IsSneaking();
        const bool isFighting = player->IsInCombat();
        std::string sneakText;
        std::string fightText;
        {
            const std::lock_guard<std::mutex> guard(eventLock);
            if (hasPlayerSample) {
                if (isSneaking != wasPlayerSneaking) sneakText = isSneaking ? "The player started sneaking." : "The player stopped sneaking.";
                if (isFighting != wasPlayerFighting) fightText = isFighting ? "The player is in a fight now." : "The fight of the player is over.";
            }
            hasPlayerSample = true;
            wasPlayerSneaking = isSneaking;
            wasPlayerFighting = isFighting;
        }
        if (!sneakText.empty()) KeepEvent(sneakText, false, 0);
        if (!fightText.empty()) KeepEvent(fightText, isFighting, SAME_FIGHT_SECONDS);
        // From interface version 20 on: quests that begin, go on and end, and what the companion spots
        RunSlowChecks(companion);
    }

    // Like TickEvents, from interface version 38 on. isFollowing: whether the companion is to follow the player right
    // now (not sent somewhere, not seated, not holding a spot), which only the script knows. With it the plugin can
    // tell when the companion falls behind (see CheckWatch)
    void TickWatch(RE::StaticFunctionTag* tag, RE::Actor* companion, bool isFollowing) {
        isCompanionFollowing.store(isFollowing);
        TickEvents(tag, companion);
    }

    // Which horse is the companion's and which is the player's (None = none): a hit on one of them and its death
    // become events wherever the horse stands
    void SetWatchedHorses(RE::StaticFunctionTag*, RE::Actor* ownHorse, RE::Actor* playersHorse) {
        eventOwnHorseID.store(ownHorse ? ownHorse->GetFormID() : 0);
        eventPlayerHorseID.store(playersHorse ? playersHorse->GetFormID() : 0);
    }

    // One text per event, its parts separated by tabs: how many seconds ago, the time of day in the game in minutes
    // since midnight, urgent (1 or 0), what happened
    std::vector<std::string> GetEventLines(const std::deque<GameEvent>& events) {
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::string> lines;
        for (const auto& kept : events) {
            const auto age = SecondsBetween(kept.time, now);
            if (age > MAX_EVENT_AGE_SECONDS) continue;
            std::string line = std::to_string(age);
            line += FIELD_SEPARATOR;
            line += std::to_string(kept.gameMinutes);
            line += FIELD_SEPARATOR;
            line += kept.isUrgent ? "1" : "0";
            line += FIELD_SEPARATOR;
            line += kept.text;
            lines.push_back(std::move(line));
        }
        return lines;
    }

    // What happened since the last call, oldest first, at most maxLines (then the newest ones). The plugin forgets
    // an event once it has handed it out. What is still being counted together is ended first, so that nothing is missing
    std::vector<std::string> TakeEvents(RE::StaticFunctionTag*, std::int32_t maxLines) {
        EndRuns(true);
        const std::size_t limit = maxLines > 0 ? static_cast<std::size_t>(maxLines) : MAX_STORED_EVENTS;
        std::deque<GameEvent> taken;
        {
            const std::lock_guard<std::mutex> guard(eventLock);
            taken.swap(gameEvents);
        }
        while (taken.size() > limit) taken.pop_front();
        return GetEventLines(taken);
    }

    // Only the urgent events since the last call (a fight begins, someone dies or goes down), built like the lines
    // of TakeEvents. They are forgotten as well, the others stay for TakeEvents
    std::vector<std::string> TakeUrgentEvents(RE::StaticFunctionTag*) {
        std::deque<GameEvent> taken;
        {
            const std::lock_guard<std::mutex> guard(eventLock);
            std::deque<GameEvent> others;
            for (auto& kept : gameEvents) {
                if (kept.isUrgent) {
                    taken.push_back(std::move(kept));
                } else {
                    others.push_back(std::move(kept));
                }
            }
            gameEvents.swap(others);
        }
        return GetEventLines(taken);
    }

    // The script moves things between the companion and the player itself (give, take). While this is set,
    // things that change hands are no event
    void SkipTransferEvents(RE::StaticFunctionTag*, bool isSkipping) { skipsTransfers.store(isSkipping); }

    // The text of a book or a note, one page of it (from 1; too high = the last one). book is the book itself or a
    // book that lies in the world. The first text tells about the book, its parts separated by tabs: the number of
    // this page, how many pages there are (0 = the book has no text), the kind (0 = book, 1 = note, 2 = spell tome),
    // the form ID of the book as eight hex digits, its title. Then one text per piece of the page: how it joins what
    // came before (1 = it begins a paragraph, 0 = it goes on after a blank, 2 = it goes on right away), a tab, the text.
    // Empty if this is no book. Reading it this way does nothing in the game: no skill goes up and no quest starts
    std::vector<std::string> GetBookText(RE::StaticFunctionTag*, RE::TESForm* form, std::int32_t page) {
        std::vector<std::string> lines;
        if (!form) return lines;
        if (auto* reference = form->AsReference()) form = reference->GetBaseObject();
        auto* book = form ? form->As<RE::TESObjectBOOK>() : nullptr;
        if (!book) return lines;
        RE::BSString description;
        book->GetDescription(description, book);
        const auto pages = ReadBookPages(ViewOf(description.c_str()));
        std::int32_t kind = BOOK_IS_BOOK;
        if (book->TeachesSpell()) {
            kind = BOOK_IS_SPELL_TOME;
        } else if (book->IsNoteScroll()) {
            kind = BOOK_IS_NOTE;
        }
        std::size_t pageNumber = page > 0 ? static_cast<std::size_t>(page) : 1;
        if (pageNumber > pages.size()) pageNumber = pages.size();
        std::string header = std::to_string(pageNumber);
        header += FIELD_SEPARATOR;
        header += std::to_string(pages.size());
        header += FIELD_SEPARATOR;
        header += std::to_string(kind);
        header += FIELD_SEPARATOR;
        header += std::format("{:08X}", book->GetFormID());
        header += FIELD_SEPARATOR;
        header += CleanText(ViewOf(book->GetName()));
        lines.push_back(std::move(header));
        if (pageNumber == 0) return lines;
        for (const auto& piece : pages[pageNumber - 1]) {
            std::string line = std::to_string(piece.join);
            line += FIELD_SEPARATOR;
            line += piece.text;
            lines.push_back(std::move(line));
        }
        return lines;
    }

    // The six functions below exist from interface version 18 on

    // Runs a command of the console of the game on a reference, as if it were typed there with that reference picked.
    // Only call this on the main thread of the game (a task of SKSE)
    void RunConsoleCommand(RE::TESObjectREFR* target, const std::string& command) {
        const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
        auto* script = factory ? factory->Create() : nullptr;
        if (!script) return;
        script->SetCommand(command);
        script->CompileAndRun(target);
        delete script;
    }

    // Sneaking: the game makes a follower ("teammate") sneak exactly when the player sneaks, and a script that
    // switches the sneaking of such a character is overruled within a moment. The console command SetForceSneak
    // forces a character that is no player to enter or to leave the sneaking state and to stay that way.
    // Returns whether the command could be handed to the game. It runs a moment later, on the main thread
    bool SetForceSneak(RE::StaticFunctionTag*, RE::Actor* actor, bool shouldSneak) {
        const auto* tasks = SKSE::GetTaskInterface();
        if (!actor || !tasks) return false;
        const auto handle = actor->GetHandle();
        tasks->AddTask([handle, shouldSneak]() {
            const auto sneaker = handle.get();
            if (sneaker) RunConsoleCommand(sneaker.get(), shouldSneak ? "SetForceSneak 1" : "SetForceSneak 0");
        });
        return true;
    }

    // What casts the spell a character holds in its right hand
    RE::MagicCaster* GetRightHandCaster(RE::Actor* actor) {
        return actor ? actor->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand) : nullptr;
    }

    // Makes a character do one of the actions of the game (as pressing or letting go of the attack button does for
    // the player). Returns what the game said to the action (false also if it could not be asked).
    // Only call this on the main thread of the game
    bool DoAction(RE::Actor* actor, RE::DEFAULT_OBJECT actionID) {
        const auto* defaults = RE::BGSDefaultObjectManager::GetSingleton();
        auto* action = defaults ? defaults->GetObject<RE::BGSAction>(actionID) : nullptr;
        if (!actor || !action) return false;
        std::unique_ptr<RE::TESActionData> data(RE::TESActionData::Create());
        if (!data) return false;
        data->source = RE::NiPointer<RE::TESObjectREFR>(actor);
        data->action = action;
        return data->Process();
    }

    // Casting by hand: a spell cast by a script (Spell.Cast) has no movement and comes out of the head of the
    // character. This lets the character cast the spell it holds in its right hand the way the game itself does it:
    // with the movement of the hand, out of the hand, and paid for with magicka. target is what the spell is aimed
    // at (None = nothing in particular). The spell has to be in the right hand and the hands have to be raised
    // (weapon drawn). Returns whether it could be handed to the game. It runs a moment later, on the main thread
    bool StartHandCast(RE::StaticFunctionTag*, RE::Actor* actor, RE::TESObjectREFR* target) {
        const auto* tasks = SKSE::GetTaskInterface();
        if (!tasks || !GetRightHandCaster(actor)) return false;
        const auto handle = actor->GetHandle();
        const auto targetHandle = target ? target->GetHandle() : RE::ObjectRefHandle();
        tasks->AddTask([handle, targetHandle]() {
            const auto casting = handle.get();
            if (!casting) return;
            if (auto* caster = GetRightHandCaster(casting.get())) caster->desiredTarget = targetHandle;
            DoAction(casting.get(), RE::DEFAULT_OBJECT::kActionRightAttack);
        });
        return true;
    }

    // Lets go of the spell: one that is charged flies off, one that is held (flames) stops
    bool EndHandCast(RE::StaticFunctionTag*, RE::Actor* actor) {
        const auto* tasks = SKSE::GetTaskInterface();
        if (!actor || !tasks) return false;
        const auto handle = actor->GetHandle();
        tasks->AddTask([handle]() {
            const auto casting = handle.get();
            if (casting) DoAction(casting.get(), RE::DEFAULT_OBJECT::kActionRightRelease);
        });
        return true;
    }

    // What the game said to the last action of StartAttack or EndAttack (see GetAttackActionResult)
    std::atomic<std::int32_t> attackActionResult{ ACTION_RESULT_PENDING };

    // Hands one of the two actions of the attack button to the game for a character and keeps what the game said to
    // it. Returns whether it could be handed over. It runs a moment later, on the main thread
    bool QueueAttackAction(RE::Actor* actor, RE::DEFAULT_OBJECT actionID) {
        const auto* tasks = SKSE::GetTaskInterface();
        if (!actor || !tasks) return false;
        const auto handle = actor->GetHandle();
        attackActionResult = ACTION_RESULT_PENDING;
        tasks->AddTask([handle, actionID]() {
            const auto attacker = handle.get();
            const bool isTaken = attacker && DoAction(attacker.get(), actionID);
            attackActionResult = isTaken ? ACTION_RESULT_TAKEN : ACTION_RESULT_REFUSED;
        });
        return true;
    }

    // Pressing the attack button for a character, whatever it holds: with a bow in its hands the game draws it.
    // Other than StartHandCast this asks for no spell in the hand, and what the game said to the action is kept
    bool StartAttack(RE::StaticFunctionTag*, RE::Actor* actor) {
        return QueueAttackAction(actor, RE::DEFAULT_OBJECT::kActionRightAttack);
    }

    // Letting go of the attack button: a drawn bow lets its arrow fly
    bool EndAttack(RE::StaticFunctionTag*, RE::Actor* actor) {
        return QueueAttackAction(actor, RE::DEFAULT_OBJECT::kActionRightRelease);
    }

    // What the game said to the last action of StartAttack or EndAttack: 1 = it took it, 0 = it refused it,
    // -1 = the action has not been carried out yet
    std::int32_t GetAttackActionResult(RE::StaticFunctionTag*) { return attackActionResult.load(); }

    // How far a character is with an attack, as the game numbers it: 0 = it does nothing, 1 to 6 = a blow,
    // 8 = it draws its bow, 9 = the arrow is on, 10 = the bow is drawn, 11 = it lets go, 12 = it has let go,
    // 13 and 14 = after the shot. -1 = cannot tell
    std::int32_t GetAttackState(RE::StaticFunctionTag*, RE::Actor* actor) {
        const auto* state = actor ? actor->AsActorState() : nullptr;
        return state ? static_cast<std::int32_t>(state->GetAttackState()) : ATTACK_STATE_UNKNOWN;
    }

    // How far the right hand of a character is with casting, as the game numbers it: 0 = it does nothing,
    // 5 = it charges the spell, 6 = it casts. -1 = cannot tell
    std::int32_t GetHandCastState(RE::StaticFunctionTag*, RE::Actor* actor) {
        const auto* caster = GetRightHandCaster(actor);
        return caster ? static_cast<std::int32_t>(caster->state.underlying()) : CAST_STATE_UNKNOWN;
    }

    // What kind of weapon or armor an item is, as the script numbers it (GetItemKind there): for a weapon its type
    // plus one, for armor a number by the part of the body it covers, 0 for everything else
    std::int32_t GetItemKind(RE::TESBoundObject* object) {
        if (const auto* weapon = object->As<RE::TESObjectWEAP>()) return static_cast<std::int32_t>(weapon->GetWeaponType()) + 1;
        const auto* armor = object->As<RE::TESObjectARMO>();
        if (!armor) return 0;
        using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
        if (armor->HasPartOf(Slot::kShield)) return ITEM_KIND_SHIELD;
        if (armor->HasPartOf(Slot::kBody)) return ITEM_KIND_BODY;
        if (armor->HasPartOf(Slot::kHead) || armor->HasPartOf(Slot::kHair) || armor->HasPartOf(Slot::kCirclet)) return ITEM_KIND_HEAD;
        if (armor->HasPartOf(Slot::kHands)) return ITEM_KIND_HANDS;
        if (armor->HasPartOf(Slot::kFeet)) return ITEM_KIND_FEET;
        if (armor->HasPartOf(Slot::kAmulet)) return ITEM_KIND_AMULET;
        if (armor->HasPartOf(Slot::kRing)) return ITEM_KIND_RING;
        return ITEM_KIND_OTHER_ARMOR;
    }

    // An inventory in one go. A script has to ask the game several questions per item, which takes seconds for a
    // full inventory and more than the server waits for the inventory of a player who carries a lot.
    // filter: 0 = everything, 1 = only the favorites, 2 = only what is worn and held. The favorites of the player are
    // what the game has among its favorites; for everybody else they are the items in "marked" (the companion marks
    // its own, see the command favorite). The first text is how many kinds of items passed the filter, then one text
    // per item, at most maxItems, its parts separated by tabs: form ID as eight hex digits, how many, form type as
    // SKSE numbers it, flags (1 = worn or held, plus 2 = favorite, plus 16 times the kind of weapon or armor), name
    std::vector<std::string> GetInventoryLines(RE::StaticFunctionTag*, RE::TESObjectREFR* owner, std::int32_t filter, std::int32_t maxItems,
                                               std::vector<RE::TESForm*> marked) {
        std::vector<std::string> lines;
        if (!owner) return lines;
        const std::size_t limit = maxItems > 0 ? static_cast<std::size_t>(maxItems) : DEFAULT_MAX_INVENTORY_ITEMS;
        const bool isPlayer = owner->IsPlayerRef();
        std::size_t total = 0;
        // The total comes first. It is only known at the end
        lines.emplace_back();
        for (const auto& [object, entry] : owner->GetInventory()) {
            if (!object || entry.first <= 0 || object->Is(RE::FormType::LeveledItem)) continue;
            const std::string name = CleanText(ViewOf(object->GetName()));
            if (name.empty()) continue;
            const auto* data = entry.second.get();
            const bool isEquipped = data && data->IsWorn();
            bool isFavorite = false;
            if (isPlayer) {
                isFavorite = data && data->IsFavorited();
            } else {
                isFavorite = std::find(marked.begin(), marked.end(), static_cast<RE::TESForm*>(object)) != marked.end();
            }
            if (filter == INVENTORY_FAVORITES && !isFavorite) continue;
            if (filter == INVENTORY_EQUIPPED && !isEquipped) continue;
            ++total;
            if (total > limit) continue;
            const std::int32_t flags = (isEquipped ? ITEM_IS_EQUIPPED : 0) + (isFavorite ? ITEM_IS_FAVORITE : 0) + ITEM_KIND_STEP * GetItemKind(object);
            lines.push_back(std::format("{:08X}\t{}\t{}\t{}\t{}", object->GetFormID(), entry.first, static_cast<std::int32_t>(object->GetFormType()), flags, name));
        }
        lines[0] = std::to_string(total);
        return lines;
    }

    // The function below exists from interface version 19 on

    // Whom a character looks at while it speaks. The game keeps several things a character can look at, and the
    // one of a dialogue comes before the one a script sets (Actor.SetLookAt). A line spoken with Actor.Say makes the
    // game put the player there, so a companion that spoke to somebody else kept its eyes off them. This puts
    // "target" there instead (None = nobody: what the script set counts again).
    // Returns whether it could be handed to the game. It runs a moment later, on the main thread
    bool SetTalkTarget(RE::StaticFunctionTag*, RE::Actor* speaker, RE::TESObjectREFR* target) {
        const auto* tasks = SKSE::GetTaskInterface();
        if (!speaker || !tasks) return false;
        const auto handle = speaker->GetHandle();
        const bool hasTarget = target != nullptr;
        const auto targetHandle = target ? target->GetHandle() : RE::ObjectRefHandle();
        tasks->AddTask([handle, hasTarget, targetHandle]() {
            const auto talking = handle.get();
            if (!talking) return;
            auto* process = talking->GetActorRuntimeData().currentProcess;
            auto* high = process ? process->high : nullptr;
            if (!high) return;
            if (!hasTarget) {
                high->ClearHeadtrackTarget(RE::HighProcessData::HEAD_TRACK_TYPE::kDialogue, false);
                return;
            }
            const auto lookedAt = targetHandle.get();
            if (lookedAt) high->SetHeadtrackTarget(RE::HighProcessData::HEAD_TRACK_TYPE::kDialogue, lookedAt.get());
        });
        return true;
    }

    // The functions below exist from interface version 20 on

    // ------------------------------------------------------------------- heal
    // The spells a character knows that give health back, strongest first. forOther: those that work on somebody
    // else (Healing Hands), otherwise those that work on the caster (Healing). For the companion that heals by itself
    std::vector<RE::SpellItem*> GetHealSpells(RE::StaticFunctionTag*, RE::Actor* caster, bool forOther) {
        struct HealSpell {
            RE::SpellItem* spell = nullptr;
            float strength = 0.0f;
        };
        std::vector<HealSpell> found;
        const auto consider = [&](RE::SpellItem* spell) {
            if (!spell || spell->GetSpellType() != RE::MagicSystem::SpellType::kSpell || ViewOf(spell->GetName()).empty()) return;
            for (const auto& known : found) {
                if (known.spell == spell) return;
            }
            const auto delivery = spell->GetDelivery();
            const bool worksOnOthers = delivery == RE::MagicSystem::Delivery::kAimed || delivery == RE::MagicSystem::Delivery::kTargetActor ||
                                       delivery == RE::MagicSystem::Delivery::kTouch;
            if (forOther ? !worksOnOthers : delivery != RE::MagicSystem::Delivery::kSelf) return;
            // What the spell is, is what its costliest effect does
            const RE::Effect* strongest = nullptr;
            for (const auto* effect : spell->effects) {
                if (effect && effect->baseEffect && (!strongest || effect->cost > strongest->cost)) strongest = effect;
            }
            if (!strongest) return;
            const auto* base = strongest->baseEffect;
            if (base->GetArchetype() != RE::EffectSetting::Archetype::kValueModifier || base->data.primaryAV != RE::ActorValue::kHealth) return;
            if (base->IsDetrimental() || base->IsHostile()) return;
            HealSpell heal;
            heal.spell = spell;
            heal.strength = strongest->effectItem.magnitude * static_cast<float>(std::max<std::uint32_t>(strongest->effectItem.duration, 1));
            found.push_back(heal);
        };
        if (caster) {
            for (auto* spell : caster->GetActorRuntimeData().addedSpells) consider(spell);
            const auto* base = caster->GetActorBase();
            const auto* known = base ? base->actorEffects : nullptr;
            if (known && known->spells) {
                for (std::uint32_t index = 0; index < known->numSpells; ++index) consider(known->spells[index]);
            }
        }
        std::sort(found.begin(), found.end(), [](const HealSpell& left, const HealSpell& right) { return left.strength > right.strength; });
        std::vector<RE::SpellItem*> spells;
        for (const auto& heal : found) spells.push_back(heal.spell);
        return spells;
    }

    // ------------------------------------------------------------------ trade
    // The companion buys from a merchant and sells to one without the barter menu of the game. A merchant is a
    // character in a faction that has a merchant's chest: what is in that chest is what the merchant sells, and its
    // gold is what the merchant pays with. Prices follow the rule of the game: the worth of the thing times (buying)
    // or divided by (selling) a factor that falls from fBarterMax to fBarterMin with the Speech skill of the one who
    // trades. Perks, potions and enchantments that better prices are left out

    constexpr std::int32_t MERCHANT_TRADES = 0;
    constexpr std::int32_t MERCHANT_IS_NONE = 1;
    constexpr std::int32_t MERCHANT_IS_CLOSED = 2;
    constexpr std::int32_t OFFER_IS_FINE = 0;
    constexpr std::int32_t OFFER_NOT_DEALT_IN = 3;
    constexpr std::int32_t OFFER_NONE_THERE = 4;
    constexpr std::int32_t OFFER_IS_WORTHLESS = 5;
    constexpr std::int32_t SIDE_MERCHANT_SELLS = 0;
    constexpr std::int32_t SIDE_COMPANION_SELLS = 1;
    constexpr RE::FormID GOLD_FORM_ID = 0xF;
    constexpr float DEFAULT_BARTER_MAX = 3.3f;
    constexpr float DEFAULT_BARTER_MIN = 2.0f;
    constexpr float FULL_SKILL = 100.0f;
    constexpr std::size_t DEFAULT_MAX_WARES = 60;

    const RE::TESFaction* FindVendorFaction(RE::Actor* merchant) {
        const auto* base = merchant ? merchant->GetActorBase() : nullptr;
        if (!base) return nullptr;
        for (const auto& membership : base->factions) {
            const auto* faction = membership.faction;
            if (faction && membership.rank >= 0 && faction->vendorData.merchantContainer) return faction;
        }
        return nullptr;
    }

    // Whether the merchant trades at this hour of the day. Hours that are the same mean: always
    bool IsTradingNow(const RE::TESFaction* faction) {
        const float start = static_cast<float>(faction->vendorData.vendorValues.startHour);
        const float end = static_cast<float>(faction->vendorData.vendorValues.endHour);
        const auto* calendar = RE::Calendar::GetSingleton();
        if (!calendar || start == end) return true;
        const float hour = calendar->GetHour();
        if (start < end) return hour >= start && hour < end;
        return hour >= start || hour < end;
    }

    std::int32_t GetMerchantStatus(RE::Actor* merchant, const RE::TESFaction* faction) {
        if (!merchant || !faction || merchant->IsDead()) return MERCHANT_IS_NONE;
        return IsTradingNow(faction) ? MERCHANT_TRADES : MERCHANT_IS_CLOSED;
    }

    // Whether the merchant buys and sells this kind of thing (the list of keywords of its faction)
    bool DealsIn(const RE::TESFaction* faction, const RE::TESBoundObject* object) {
        auto* list = faction->vendorData.vendorSellBuyList;
        if (!list) return true;
        const bool isListed = object->HasKeywordInList(list, false);
        return faction->vendorData.vendorValues.notBuySell ? !isListed : isListed;
    }

    std::int32_t GetBaseValue(const RE::TESBoundObject* object) {
        if (HasGameAddress(ITEM_VALUE_ID)) return std::max(object->GetGoldValue(), 0);
        const auto* valued = object->As<RE::TESValueForm>();
        return valued ? std::max(valued->value, 0) : 0;
    }

    float GetBarterSetting(const char* name, float usual) {
        if (!HasGameAddress(GAME_SETTINGS_ID)) return usual;
        auto* settings = RE::GameSettingCollection::GetSingleton();
        const auto* setting = settings ? settings->GetSetting(name) : nullptr;
        return setting ? setting->GetFloat() : usual;
    }

    // What one piece costs the trader (isBuying) or brings the trader
    std::int32_t GetTradePrice(const RE::TESBoundObject* object, RE::Actor* trader, bool isBuying) {
        const float value = static_cast<float>(GetBaseValue(object));
        if (value <= 0.0f) return 0;
        const float highest = GetBarterSetting("fBarterMax", DEFAULT_BARTER_MAX);
        const float lowest = GetBarterSetting("fBarterMin", DEFAULT_BARTER_MIN);
        float skill = 0.0f;
        if (const auto* values = trader ? trader->AsActorValueOwner() : nullptr) skill = values->GetActorValue(RE::ActorValue::kSpeech);
        const float factor = std::max(highest - (highest - lowest) * std::clamp(skill, 0.0f, FULL_SKILL) / FULL_SKILL, 1.0f);
        if (isBuying) return std::max(static_cast<std::int32_t>(std::lround(value * factor)), 1);
        return static_cast<std::int32_t>(std::lround(value / factor));
    }

    std::int32_t CountInInventory(RE::TESObjectREFR* holder, RE::FormID formID) {
        if (!holder) return 0;
        for (const auto& [object, entry] : holder->GetInventory()) {
            if (object && object->GetFormID() == formID) return std::max(entry.first, 0);
        }
        return 0;
    }

    // The chest a merchant sells from and pays out of, or nothing if the character is no merchant
    RE::TESObjectREFR* GetMerchantChest(RE::StaticFunctionTag*, RE::Actor* merchant) {
        const auto* faction = FindVendorFaction(merchant);
        return faction ? faction->vendorData.merchantContainer : nullptr;
    }

    // What a merchant offers and what the companion could sell to it. The first text, its parts separated by tabs:
    // ref ID of the merchant as eight hex digits, status (0 = trades, 1 = no merchant, 2 = does not trade at this
    // hour), gold of the merchant, gold of the buyer, the hours it trades from and to, how many kinds of wares it
    // has, how many kinds of things the buyer could sell, name. Then one text per thing, at most maxItems per side:
    // side (0 = the merchant sells it, 1 = the buyer could sell it), form ID as eight hex digits, how many, form
    // type as SKSE numbers it, price of one piece, name
    std::vector<std::string> GetMerchantLines(RE::StaticFunctionTag*, RE::Actor* merchant, RE::Actor* buyer, std::int32_t maxItems) {
        std::vector<std::string> lines;
        if (!merchant) return lines;
        const auto* faction = FindVendorFaction(merchant);
        const std::int32_t status = GetMerchantStatus(merchant, faction);
        auto* chest = faction ? faction->vendorData.merchantContainer : nullptr;
        const std::size_t limit = maxItems > 0 ? static_cast<std::size_t>(maxItems) : DEFAULT_MAX_WARES;
        std::size_t wareCount = 0;
        std::size_t sellCount = 0;
        std::int32_t merchantGold = 0;
        std::int32_t buyerGold = 0;
        // The first text is only known at the end
        lines.emplace_back();
        if (chest) {
            for (const auto& [object, entry] : chest->GetInventory()) {
                if (!object || entry.first <= 0 || object->Is(RE::FormType::LeveledItem)) continue;
                if (object->GetFormID() == GOLD_FORM_ID) {
                    merchantGold = entry.first;
                    continue;
                }
                const std::string name = CleanText(ViewOf(object->GetName()));
                const std::int32_t price = GetTradePrice(object, buyer, true);
                if (name.empty() || price <= 0) continue;
                ++wareCount;
                if (wareCount > limit) continue;
                lines.push_back(std::format("{}\t{:08X}\t{}\t{}\t{}\t{}", SIDE_MERCHANT_SELLS, object->GetFormID(), entry.first,
                                            static_cast<std::int32_t>(object->GetFormType()), price, name));
            }
        }
        if (buyer) {
            for (const auto& [object, entry] : buyer->GetInventory()) {
                if (!object || entry.first <= 0 || object->Is(RE::FormType::LeveledItem)) continue;
                if (object->GetFormID() == GOLD_FORM_ID) {
                    buyerGold = entry.first;
                    continue;
                }
                if (!faction) continue;
                const auto* data = entry.second.get();
                if (data && data->IsWorn()) continue;
                const std::string name = CleanText(ViewOf(object->GetName()));
                const std::int32_t price = GetTradePrice(object, buyer, false);
                if (name.empty() || price <= 0 || !DealsIn(faction, object)) continue;
                ++sellCount;
                if (sellCount > limit) continue;
                lines.push_back(std::format("{}\t{:08X}\t{}\t{}\t{}\t{}", SIDE_COMPANION_SELLS, object->GetFormID(), entry.first,
                                            static_cast<std::int32_t>(object->GetFormType()), price, name));
            }
        }
        const std::int32_t startHour = faction ? static_cast<std::int32_t>(faction->vendorData.vendorValues.startHour) : 0;
        const std::int32_t endHour = faction ? static_cast<std::int32_t>(faction->vendorData.vendorValues.endHour) : 0;
        lines[0] = std::format("{:08X}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", merchant->GetFormID(), status, merchantGold, buyerGold, startHour, endHour,
                               wareCount, sellCount, CleanText(ViewOf(merchant->GetDisplayFullName())));
        return lines;
    }

    // One thing the companion wants to buy from a merchant (isBuying) or sell to it. Four numbers: status (0 = fine,
    // 1 = no merchant, 2 = does not trade at this hour, 3 = the merchant does not deal in this kind of thing,
    // 4 = none there, 5 = it is worth nothing), price of one piece, how many there are (in the chest of the merchant
    // when buying, with the buyer when selling), and the gold of the one who pays (the buyer when buying, the merchant
    // when selling)
    std::vector<std::int32_t> GetTradeOffer(RE::StaticFunctionTag*, RE::Actor* merchant, RE::Actor* buyer, RE::TESForm* item, bool isBuying) {
        std::vector<std::int32_t> offer = {MERCHANT_IS_NONE, 0, 0, 0};
        const auto* faction = FindVendorFaction(merchant);
        auto* chest = faction ? faction->vendorData.merchantContainer : nullptr;
        offer[0] = GetMerchantStatus(merchant, faction);
        if (offer[0] != MERCHANT_TRADES || !chest || !buyer) return offer;
        const auto* object = item ? item->As<RE::TESBoundObject>() : nullptr;
        if (!object || object->GetFormID() == GOLD_FORM_ID) {
            offer[0] = OFFER_NONE_THERE;
            return offer;
        }
        offer[1] = GetTradePrice(object, buyer, isBuying);
        offer[2] = CountInInventory(isBuying ? chest : static_cast<RE::TESObjectREFR*>(buyer), object->GetFormID());
        offer[3] = CountInInventory(isBuying ? static_cast<RE::TESObjectREFR*>(buyer) : chest, GOLD_FORM_ID);
        if (offer[2] <= 0) {
            offer[0] = OFFER_NONE_THERE;
        } else if (!isBuying && !DealsIn(faction, object)) {
            offer[0] = OFFER_NOT_DEALT_IN;
        } else if (offer[1] <= 0) {
            offer[0] = OFFER_IS_WORTHLESS;
        } else {
            offer[0] = OFFER_IS_FINE;
        }
        return offer;
    }

    // The functions below exist from interface version 21 on

    // ------------------------------------------------------------ quick state
    // A script waits for the next picture of the game with nearly every question it asks the game. The state for the
    // AI took about six seconds that way, most of it for the characters around: several questions per character.
    // These functions answer many of those questions in one go

    constexpr std::size_t ACTOR_NUMBER_COUNT = 16;
    constexpr std::int32_t NEARBY_NOT_KNOWN = -1;
    constexpr std::int32_t NEARBY_RANK_FOUGHT = 0;
    constexpr std::int32_t NEARBY_RANK_HOSTILE = 1;
    constexpr std::int32_t NEARBY_RANK_LIVING = 2;
    constexpr std::int32_t NEARBY_RANK_DEAD = 3;

    std::mutex nearbyLock;
    // The names that belong to the numbers GetNearbyValues returned last
    std::vector<std::string> nearbyNames;

    // What a script would ask a character one by one. viewer: from whom the distance and the direction count (None =
    // both stay 0). The numbers: ref ID, dead, in a fight, position x, y, z, the direction it faces in degrees, weapon
    // drawn, sneaking, in the loaded area, distance to the viewer in meters, angle from where the viewer faces in
    // degrees (negative = left), ref ID of whom it fights (0 = nobody), told to wait by the player, and from interface
    // version 23 on: rides, sits on something (seated or about to get up; a rider counts as seated too)
    std::vector<std::int32_t> GetActorNumbers(RE::StaticFunctionTag*, RE::Actor* actor, RE::Actor* viewer) {
        std::vector<std::int32_t> numbers(ACTOR_NUMBER_COUNT, 0);
        if (!actor) return numbers;
        const auto position = actor->GetPosition();
        numbers[0] = static_cast<std::int32_t>(actor->GetFormID());
        numbers[1] = actor->IsDead() ? 1 : 0;
        numbers[2] = actor->IsInCombat() ? 1 : 0;
        numbers[3] = static_cast<std::int32_t>(position.x);
        numbers[4] = static_cast<std::int32_t>(position.y);
        numbers[5] = static_cast<std::int32_t>(position.z);
        numbers[6] = static_cast<std::int32_t>(actor->GetAngleZ() * DEGREES_PER_RADIAN);
        const auto* state = actor->AsActorState();
        numbers[7] = state && state->IsWeaponDrawn() ? 1 : 0;
        numbers[8] = actor->IsSneaking() ? 1 : 0;
        numbers[9] = actor->Is3DLoaded() ? 1 : 0;
        if (viewer && viewer != actor) {
            const View view = GetViewFrom(viewer, position);
            numbers[10] = view.distanceMeters;
            numbers[11] = view.angleDegrees;
        }
        const auto fought = actor->GetActorRuntimeData().currentCombatTarget.get();
        if (fought) numbers[12] = static_cast<std::int32_t>(fought->GetFormID());
        if (const auto* values = actor->AsActorValueOwner()) numbers[13] = values->GetActorValue(RE::ActorValue::kWaitingForPlayer) > 0.0f ? 1 : 0;
        numbers[14] = actor->IsOnMount() ? 1 : 0;
        if (state) {
            const auto sitState = state->GetSitSleepState();
            numbers[15] = sitState == RE::SIT_SLEEP_STATE::kIsSitting || sitState == RE::SIT_SLEEP_STATE::kWantToStand ? 1 : 0;
        }
        return numbers;
    }

    // The characters around the viewer, living and dead, without the player and the companion, at most maxCharacters
    // within radiusMeters: whom the companion fights first, then the hostile ones, the other living ones and the dead,
    // each nearest first. Five numbers per character, as the script built them before: ref ID, distance to the viewer
    // in meters, angle from where the viewer faces in degrees (negative = left), hostile to the player (for the dead:
    // how many different items they carry, plus 1), dead. Their names come from GetNearbyNames.
    // A single -1 means: the plugin cannot look around in this game, the script has to do it itself.
    // senseHalfAngleDegrees: NO_SIGHT_FILTER = everyone around. Otherwise only whom the viewer notices: whom it sees
    // within that many degrees to the left and right, whom it has close by and whom it hears (see GetSenseOf).
    // senses then gets one number per character of the list: SENSE_SEEN, SENSE_CLOSE or SENSE_HEARD
    std::vector<std::int32_t> CollectNearbyValues(RE::Actor* viewer, RE::Actor* companion, std::int32_t radiusMeters, std::int32_t maxCharacters,
                                                  std::int32_t senseHalfAngleDegrees, std::vector<std::int32_t>& senses) {
        std::vector<std::int32_t> values;
        std::vector<std::string> names;
        senses.clear();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !viewer || !viewer->Is3DLoaded() || !HasGameAddress(WORLD_SINGLETON_ID)) {
            const std::lock_guard<std::mutex> guard(nearbyLock);
            nearbyNames.clear();
            return {NEARBY_NOT_KNOWN};
        }
        struct SeenCharacter {
            RE::Actor* actor = nullptr;
            float distance = 0.0f;
            std::int32_t rank = NEARBY_RANK_LIVING;
            std::int32_t sense = SENSE_SEEN;
        };
        const auto foughtHandle = companion ? companion->GetActorRuntimeData().currentCombatTarget.get() : RE::NiPointer<RE::Actor>();
        const RE::Actor* fought = foughtHandle.get();
        const bool canTellHostile = HasGameAddress(HOSTILE_TO_ACTOR_ID);
        std::vector<SeenCharacter> seen;
        const auto own = viewer->GetPosition();
        if (radiusMeters > 0 && maxCharacters > 0) {
            ForEachReferenceAround(viewer, static_cast<float>(radiusMeters) * GAME_UNITS_PER_METER, [&](RE::TESObjectREFR* reference) {
                if (!reference || reference == viewer || reference == player || reference == companion || reference->IsDisabled() ||
                    reference->IsMarkedForDeletion()) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                auto* actor = reference->As<RE::Actor>();
                // A character without a body is not in the world right now, nor is one the game hides (see GetAbsence)
                if (!actor || !actor->Is3DLoaded() || !GetAbsence(actor, viewer).empty()) return RE::BSContainer::ForEachResult::kContinue;
                for (const auto& known : seen) {
                    if (known.actor == actor) return RE::BSContainer::ForEachResult::kContinue;
                }
                SeenCharacter character;
                character.actor = actor;
                character.distance = own.GetDistance(actor->GetPosition());
                if (actor == fought) {
                    character.rank = NEARBY_RANK_FOUGHT;
                } else if (actor->IsDead()) {
                    character.rank = NEARBY_RANK_DEAD;
                } else if (canTellHostile && actor->IsHostileToActor(player)) {
                    character.rank = NEARBY_RANK_HOSTILE;
                }
                seen.push_back(character);
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }
        if (senseHalfAngleDegrees > NO_SIGHT_FILTER) {
            // Only whom the viewer notices. Without the physics of the game the walls cannot be checked
            const auto* viewerCell = viewer->GetParentCell();
            const bool canCastRays = HasGameAddress(CELL_PHYSICS_ID) && HasGameAddress(PHYSICS_LOCK_ID) && HasGameAddress(PHYSICS_UNLOCK_ID);
            auto* physics = viewerCell && canCastRays ? viewerCell->GetbhkWorld() : nullptr;
            if (!physics) {
                const std::lock_guard<std::mutex> guard(nearbyLock);
                nearbyNames.clear();
                return {NEARBY_NOT_KNOWN};
            }
            {
                const float scale = HasGameAddress(PHYSICS_SCALE_ID) ? RE::bhkWorld::GetWorldScale() : DEFAULT_PHYSICS_SCALE;
                const RE::NiPoint3 eyes(own.x, own.y, own.z + SIGHT_EYE_UNITS);
                const float sightUnits = GetSightUnitsOf(viewer);
                const RE::BSReadLockGuard guard(physics->worldLock);
                for (auto& character : seen) {
                    character.sense = GetSenseOf(physics, scale, eyes, character.actor, GetViewFrom(viewer, character.actor->GetPosition()),
                                                 character.distance, senseHalfAngleDegrees, sightUnits);
                }
            }
            std::erase_if(seen, [](const SeenCharacter& character) { return character.sense == SENSE_NONE; });
        }
        std::sort(seen.begin(), seen.end(), [](const SeenCharacter& left, const SeenCharacter& right) {
            if (left.rank != right.rank) return left.rank < right.rank;
            return left.distance < right.distance;
        });
        if (maxCharacters > 0 && seen.size() > static_cast<std::size_t>(maxCharacters)) seen.resize(static_cast<std::size_t>(maxCharacters));
        for (const auto& character : seen) {
            auto* actor = character.actor;
            const View view = GetViewFrom(viewer, actor->GetPosition());
            const bool isDead = actor->IsDead();
            std::int32_t fourth = 0;
            if (isDead) {
                fourth = CountLoot(actor) + 1;
            } else if (canTellHostile && actor->IsHostileToActor(player)) {
                fourth = 1;
            }
            KeepHeight(viewer, actor->GetFormID(), view.upUnits);
            values.push_back(static_cast<std::int32_t>(actor->GetFormID()));
            values.push_back(view.distanceMeters);
            values.push_back(view.angleDegrees);
            values.push_back(fourth);
            values.push_back(isDead ? 1 : 0);
            senses.push_back(character.sense);
            // The place in the list goes in front, so that the server can tell which name belongs to which numbers
            names.push_back(std::to_string(names.size()) + ":" + CleanText(ViewOf(actor->GetDisplayFullName())));
        }
        {
            const std::lock_guard<std::mutex> guard(nearbyLock);
            nearbyNames = std::move(names);
        }
        return values;
    }

    // Everyone around the viewer (see CollectNearbyValues)
    std::vector<std::int32_t> GetNearbyValues(RE::StaticFunctionTag*, RE::Actor* viewer, RE::Actor* companion, std::int32_t radiusMeters,
                                              std::int32_t maxCharacters) {
        std::vector<std::int32_t> senses;
        return CollectNearbyValues(viewer, companion, radiusMeters, maxCharacters, NO_SIGHT_FILTER, senses);
    }

    // The names of the characters GetNearbyValues listed last: the place in that list, a colon and the name
    std::vector<std::string> GetNearbyNames(RE::StaticFunctionTag*) {
        const std::lock_guard<std::mutex> guard(nearbyLock);
        return nearbyNames;
    }

    // The function below exists from interface version 22 on

    // Why the target of a command is not really there (see GetAbsence), or an empty text if it is. viewer: the player.
    // What the plugin found goes into its log, so that a character nobody can see can be looked up afterwards
    std::string GetAbsenceReason(RE::StaticFunctionTag*, RE::TESObjectREFR* target, RE::Actor* viewer) {
        const std::string absence = GetAbsence(target, viewer);
        if (target) {
            const auto* actor = target->As<RE::Actor>();
            const auto* cell = target->GetParentCell();
            const auto* body = actor ? target->Get3D() : nullptr;
            const auto position = target->GetPosition();
            logger::info("Presence {:08X} {}: disabled={} deleted={} character={} body={} hidden={} cell={:08X} attached={} indoors={} world={:08X} position=({:.0f}, {:.0f}, {:.0f}) -> {}",
                         target->GetFormID(), CleanText(ViewOf(target->GetDisplayFullName())), target->IsDisabled(), target->IsMarkedForDeletion(),
                         actor != nullptr, target->Is3DLoaded(), body && body->GetAppCulled(), cell ? cell->GetFormID() : 0,
                         cell && cell->IsAttached(), cell && cell->IsInteriorCell(), GetRootWorldOf(cell) ? GetRootWorldOf(cell)->GetFormID() : 0,
                         position.x, position.y, position.z, absence.empty() ? "there" : absence);
        }
        return absence;
    }

    // The four functions below exist from interface version 23 on

    // Which menus were opened and closed again since the last call, oldest first: the name the game gives the menu,
    // a tab and how long it was open in milliseconds. The plugin forgets them once it has handed them out
    std::vector<std::string> TakeMenuLines(RE::StaticFunctionTag*) {
        std::deque<MenuVisit> taken;
        {
            const std::lock_guard<std::mutex> guard(menuLock);
            taken.swap(menuVisits);
        }
        std::vector<std::string> lines;
        lines.reserve(taken.size());
        for (const auto& visit : taken) lines.push_back(std::format("{}\t{}", CleanText(visit.name), visit.milliseconds));
        return lines;
    }

    // What a character holds in its hands, as the script built it before with several questions: "R:" and the name of
    // what is in the right hand, "L:" and the name of what is in the left one (a shield counts as held in the left
    // hand). A weapon held with both hands is only named for the right one. Nothing behind the colon = an empty hand
    std::vector<std::string> GetHandNames(RE::StaticFunctionTag*, RE::Actor* holder) {
        std::vector<std::string> hands = {"R:", "L:"};
        if (!holder) return hands;
        const RE::TESForm* rightHand = holder->GetEquippedObject(false);
        const RE::TESForm* leftHand = holder->GetEquippedObject(true);
        if (!leftHand) leftHand = holder->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kShield);
        if (rightHand) hands[0] += rightHand->GetName();
        if (leftHand && leftHand != rightHand) hands[1] += leftHand->GetName();
        return hands;
    }

    // ------------------------------------------------------------ loose items to collect (pick_up with 'all')
    // The words of the filter. The companion server and the script use the same ones
    constexpr std::string_view LOOSE_KIND_POTIONS = "potions";
    constexpr std::string_view LOOSE_KIND_FOOD = "food";
    constexpr std::string_view LOOSE_KIND_INGREDIENTS = "ingredients";
    constexpr std::string_view LOOSE_KIND_WEAPONS = "weapons";
    constexpr std::string_view LOOSE_KIND_ARMOR = "armor";
    constexpr std::string_view LOOSE_KIND_AMMO = "ammo";
    constexpr std::string_view LOOSE_KIND_BOOKS = "books";
    constexpr std::string_view LOOSE_KIND_SCROLLS = "scrolls";
    constexpr std::string_view LOOSE_KIND_KEYS = "keys";
    constexpr std::string_view LOOSE_KIND_GOLD = "gold";
    constexpr std::string_view LOOSE_KIND_MISC = "misc";
    constexpr std::string_view LOOSE_KIND_NONE = "";
    constexpr std::string_view LOOSE_FILTER_NO_CLUTTER = "no_clutter";
    // The keyword the game gives the odds and ends merchants pay next to nothing for: plates, tankards, quills
    constexpr std::string_view CLUTTER_KEYWORD = "VendorItemClutter";
    // What lies higher or lower than this, seen from the center, lies on another floor
    constexpr float LOOSE_ITEM_HEIGHT_UNITS = 2.5f * GAME_UNITS_PER_METER;
    constexpr std::int32_t LOOSE_NOT_KNOWN = -1;

    std::mutex looseLock;
    // What FindLooseItems left out the last time (see GetLooseItemNumbers)
    std::int32_t looseOwnedCount = 0;
    std::int32_t looseMoreCount = 0;

    // Under which word of the filter something falls that lies around, or LOOSE_KIND_NONE if it is nothing to collect
    std::string_view GetLooseKind(const RE::TESBoundObject* base) {
        switch (base->GetFormType()) {
            case RE::FormType::AlchemyItem: {
                const auto* potion = base->As<RE::AlchemyItem>();
                return potion && potion->IsFood() ? LOOSE_KIND_FOOD : LOOSE_KIND_POTIONS;
            }
            case RE::FormType::Ingredient:
                return LOOSE_KIND_INGREDIENTS;
            case RE::FormType::Weapon:
                return LOOSE_KIND_WEAPONS;
            case RE::FormType::Armor:
                return LOOSE_KIND_ARMOR;
            case RE::FormType::Ammo:
                return LOOSE_KIND_AMMO;
            case RE::FormType::Book:
                return LOOSE_KIND_BOOKS;
            case RE::FormType::Scroll:
                return LOOSE_KIND_SCROLLS;
            case RE::FormType::KeyMaster:
                return LOOSE_KIND_KEYS;
            case RE::FormType::SoulGem:
                return LOOSE_KIND_MISC;
            case RE::FormType::Misc:
                return base->IsGold() ? LOOSE_KIND_GOLD : LOOSE_KIND_MISC;
            case RE::FormType::Light: {
                // A torch can be carried, a candle on the wall cannot
                const auto* light = base->As<RE::TESObjectLIGH>();
                return light && light->CanBeCarried() ? LOOSE_KIND_MISC : LOOSE_KIND_NONE;
            }
            default:
                return LOOSE_KIND_NONE;
        }
    }

    bool IsClutter(const RE::TESBoundObject* base) {
        const auto* keywords = base->As<RE::BGSKeywordForm>();
        return keywords && keywords->HasKeywordString(CLUTTER_KEYWORD);
    }

    // The loose items a companion may collect within radiusMeters around the center (nothing = around the companion),
    // the ones nearest to the companion first, at most maxItems. filter: one of the words above (only that kind),
    // LOOSE_FILTER_NO_CLUTTER (everything but what the game marks with CLUTTER_KEYWORD), or empty for everything.
    // Left out: what belongs to someone (taking it would be stealing), what has no name, what is not meant for the
    // player, lights that cannot be carried, arrows that were shot (they are no items for the game, see
    // TakeRestingArrow) and what lies on another floor (see LOOSE_ITEM_HEIGHT_UNITS). GetLooseItemNumbers tells
    // afterwards how much was left out
    std::vector<RE::TESObjectREFR*> FindLooseItems(RE::StaticFunctionTag*, RE::Actor* companion, RE::TESObjectREFR* center,
                                                   std::int32_t radiusMeters, RE::BSFixedString filter, std::int32_t maxItems) {
        std::vector<RE::TESObjectREFR*> items;
        std::int32_t ownedCount = 0;
        std::int32_t moreCount = 0;
        const bool canScan = companion && radiusMeters > 0 && maxItems > 0 && HasGameAddress(CRIME_TO_ACTIVATE_ID);
        if (canScan) {
            if (!center) center = companion;
            const std::string wanted(ViewOf(filter.c_str()));
            const auto middle = center->GetPosition();
            const auto own = companion->GetPosition();
            const float radius = static_cast<float>(radiusMeters) * GAME_UNITS_PER_METER;
            const auto goOn = RE::BSContainer::ForEachResult::kContinue;
            std::vector<std::pair<float, RE::TESObjectREFR*>> found;
            ForEachReferenceAround(companion, radius + own.GetDistance(middle), [&](RE::TESObjectREFR* reference) {
                if (!reference || reference == companion || reference->IsDisabled() || reference->IsMarkedForDeletion() || !reference->Is3DLoaded()) {
                    return goOn;
                }
                const auto* base = reference->GetBaseObject();
                if (!base || !base->GetPlayable()) return goOn;
                const std::string_view kind = GetLooseKind(base);
                if (kind.empty()) return goOn;
                const auto position = reference->GetPosition();
                if (position.GetDistance(middle) > radius || std::abs(position.z - middle.z) > LOOSE_ITEM_HEIGHT_UNITS) return goOn;
                if (CleanText(ViewOf(reference->GetDisplayFullName())).empty()) return goOn;
                // A script does not keep the case of a text: the game hands it "Potions" if it met the word like that
                // first. In a test no potion was found for that reason
                if (EqualsIgnoreCase(wanted, LOOSE_FILTER_NO_CLUTTER)) {
                    if (IsClutter(base)) return goOn;
                } else if (!wanted.empty() && !EqualsIgnoreCase(wanted, kind)) {
                    return goOn;
                }
                if (reference->IsCrimeToActivate()) {
                    ++ownedCount;
                    return goOn;
                }
                found.emplace_back(own.GetDistance(position), reference);
                return goOn;
            });
            std::sort(found.begin(), found.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
            const std::size_t limit = static_cast<std::size_t>(maxItems);
            if (found.size() > limit) moreCount = static_cast<std::int32_t>(found.size() - limit);
            for (const auto& entry : found) {
                if (items.size() >= limit) break;
                items.push_back(entry.second);
            }
        }
        {
            const std::lock_guard<std::mutex> guard(looseLock);
            looseOwnedCount = canScan ? ownedCount : LOOSE_NOT_KNOWN;
            looseMoreCount = moreCount;
        }
        return items;
    }

    // What the last FindLooseItems left out: how many things that belong to someone (LOOSE_NOT_KNOWN = it could not
    // look at all), and how many more it found than it was allowed to return
    std::vector<std::int32_t> GetLooseItemNumbers(RE::StaticFunctionTag*) {
        const std::lock_guard<std::mutex> guard(looseLock);
        return {looseOwnedCount, looseMoreCount};
    }

    // The two functions below exist from interface version 24 on

    // ------------------------------------------------------------ the whole state in one go
    // Nearly every question a script asks the game waits for the next picture, and so does every value it puts into
    // its request to the companion server. A state took about two seconds that way, with about a hundred questions.
    // GetWholeState answers what goes along with every state in one go and packs it, so that the script hands it on
    // with two values: a list of numbers (what GetWholeState returns) and a list of texts (GetWholeStateTexts).
    // The companion server unpacks both again (expand_quick_state in game_state.py of the Mantella fork).
    //
    // The numbers: first STATE_HEAD_COUNT numbers for the script itself (see STATE_HEAD_...), then one block per list
    // of the state: which list (STATE_BLOCK_...), how many numbers, the numbers. Every block is built the way the
    // script built that list before (BuildState and BuildCompanionValues in KI_MCP_Gefaehrte_Loop.psc).
    // The texts: the name of the entry of the state, a tab, the text. A list of texts has one line per text.
    // What only the script knows it hands in: numbers (STATE_OPTION_...) and forms (STATE_FORM_...). The same
    // places are named in the script (StateOption..., StateForm...) and must be kept the same there

    constexpr std::size_t STATE_OPTION_PROTOCOL = 0;
    constexpr std::size_t STATE_OPTION_LIST_LIMIT = 1;
    constexpr std::size_t STATE_OPTION_MUTE = 2;
    constexpr std::size_t STATE_OPTION_HEARING = 3;
    // What the game told the script about the player and the companion. These stay questions of the script, because
    // nothing here is known to answer them exactly like the script functions do
    constexpr std::size_t STATE_OPTION_PLAYER_HEALTH = 4;
    constexpr std::size_t STATE_OPTION_PLAYER_SIT_STATE = 5;
    constexpr std::size_t STATE_OPTION_PLAYER_SLEEP_STATE = 6;
    constexpr std::size_t STATE_OPTION_COMPANION_HEALTH = 7;
    constexpr std::size_t STATE_OPTION_COMPANION_SIT_STATE = 8;
    constexpr std::size_t STATE_OPTION_COMPANION_RUNS = 9;
    // What the script keeps itself
    constexpr std::size_t STATE_OPTION_RIDE_HOLD = 10;
    constexpr std::size_t STATE_OPTION_FLEEING = 11;
    constexpr std::size_t STATE_OPTION_KEEPS_WEAPON_AWAY = 12;
    constexpr std::size_t STATE_OPTION_SNEAK_MODE = 13;
    constexpr std::size_t STATE_OPTION_AUTO_HEAL_PERCENT = 14;
    constexpr std::size_t STATE_OPTION_LEAD_STATE = 15;
    constexpr std::size_t STATE_OPTION_RIDE_LOCKED = 16;
    // How long the lists may get and how far they look, as the script handed it to the single functions before
    constexpr std::size_t STATE_OPTION_MAX_QUESTS = 17;
    constexpr std::size_t STATE_OPTION_MAX_QUEST_TARGETS = 18;
    constexpr std::size_t STATE_OPTION_PLACE_RADIUS = 19;
    constexpr std::size_t STATE_OPTION_MAX_PLACE_MARKERS = 20;
    constexpr std::size_t STATE_OPTION_MAX_PLACE_DOORS = 21;
    constexpr std::size_t STATE_OPTION_TRAP_RADIUS = 22;
    constexpr std::size_t STATE_OPTION_MAX_TRAPS = 23;
    constexpr std::size_t STATE_OPTION_MAX_HEARD_LINES = 24;
    constexpr std::size_t STATE_OPTION_MAX_EVENT_LINES = 25;
    constexpr std::size_t STATE_OPTION_INDOOR_RADIUS = 26;
    constexpr std::size_t STATE_OPTION_OUTDOOR_RADIUS = 27;
    // In which alias of its quests the game mod keeps the rider, its horse, the horse of the player and the horse
    // that was called
    constexpr std::size_t STATE_OPTION_ALIAS_RIDER = 28;
    constexpr std::size_t STATE_OPTION_ALIAS_HORSE = 29;
    constexpr std::size_t STATE_OPTION_ALIAS_PLAYER_HORSE = 30;
    constexpr std::size_t STATE_OPTION_ALIAS_CALLED_HORSE = 31;
    constexpr std::size_t STATE_OPTION_COUNT = 32;
    // From interface version 25 on, and not needed for a state: 1 = only what the viewer notices is listed
    // ('visible' with the companion as the viewer), and how many degrees to the left and right the viewer sees
    constexpr std::size_t STATE_OPTION_VISIBLE = 32;
    constexpr std::size_t STATE_OPTION_VISIBLE_HALF_ANGLE = 33;
    // From interface version 26 on, and not needed for a state: which of the facts the script used to ask the game
    // for the plugin is to fill in itself (STATE_FACT_...). The script lets it, and checks now and then by asking
    // the game as before: what the plugin worked out always goes back in the first numbers (STATE_HEAD_PLAYER_HEALTH ...)
    constexpr std::size_t STATE_OPTION_PLUGIN_FACTS = 34;
    constexpr std::int32_t STATE_FACT_HEALTH = 1;
    constexpr std::int32_t STATE_FACT_SITTING = 2;
    constexpr std::int32_t STATE_FACT_RUNNING = 4;

    // What the companion walks to, what it rides to, what it looks at, whom it follows on foot and on horseback,
    // and the three quests of the game mod that keep the horses
    constexpr std::size_t STATE_FORM_WALK_TARGET = 0;
    constexpr std::size_t STATE_FORM_RIDE_TARGET = 1;
    constexpr std::size_t STATE_FORM_LOOK_TARGET = 2;
    constexpr std::size_t STATE_FORM_LEADER = 3;
    constexpr std::size_t STATE_FORM_RIDE_LEADER = 4;
    constexpr std::size_t STATE_FORM_HORSE_QUEST = 5;
    constexpr std::size_t STATE_FORM_PLAYER_HORSE_QUEST = 6;
    constexpr std::size_t STATE_FORM_HORSE_CALL_QUEST = 7;
    constexpr std::size_t STATE_FORM_COUNT = 8;

    // For the script: how many numbers come before the first block, whether the player and the companion ride (so it
    // need not ask on what they sit otherwise), and whether a horse has lost the name the script gave it
    constexpr std::size_t STATE_HEAD_LENGTH = 0;
    constexpr std::size_t STATE_HEAD_PLAYER_RIDES = 1;
    constexpr std::size_t STATE_HEAD_COMPANION_RIDES = 2;
    constexpr std::size_t STATE_HEAD_PLAYER_HORSE_NAME_LOST = 3;
    constexpr std::size_t STATE_HEAD_OWN_HORSE_NAME_LOST = 4;
    // What the plugin worked out itself, for the script to check against what the game tells it
    constexpr std::size_t STATE_HEAD_PLAYER_HEALTH = 5;
    constexpr std::size_t STATE_HEAD_PLAYER_SIT_STATE = 6;
    constexpr std::size_t STATE_HEAD_PLAYER_SLEEP_STATE = 7;
    constexpr std::size_t STATE_HEAD_COMPANION_HEALTH = 8;
    constexpr std::size_t STATE_HEAD_COMPANION_SIT_STATE = 9;
    constexpr std::size_t STATE_HEAD_COMPANION_RUNS = 10;
    constexpr std::size_t STATE_HEAD_COUNT = 11;
    // Sitting and sleeping as scripts are told them: not, about to, doing it, about to stop
    constexpr std::int32_t REST_STATE_NONE = 0;
    constexpr std::int32_t REST_STATE_WANTED = 2;
    constexpr std::int32_t REST_STATE_DOING = 3;
    constexpr std::int32_t REST_STATE_ENDING = 4;
    constexpr float PERCENT_FULL = 100.0f;

    // The lists of numbers. The times come first: the script writes how long it took itself into the first of them
    constexpr std::int32_t STATE_BLOCK_TIMES = 1;
    constexpr std::int32_t STATE_BLOCK_PLAYER = 2;
    constexpr std::int32_t STATE_BLOCK_WORLD = 3;
    constexpr std::int32_t STATE_BLOCK_COMPANION = 4;
    constexpr std::int32_t STATE_BLOCK_GOLD = 5;
    constexpr std::int32_t STATE_BLOCK_HORSE_CALLED = 6;
    constexpr std::int32_t STATE_BLOCK_PLAYER_HORSE = 7;
    constexpr std::int32_t STATE_BLOCK_HORSE = 8;
    constexpr std::int32_t STATE_BLOCK_NEARBY = 9;
    // With 'visible': by which sense the viewer notices each character of STATE_BLOCK_NEARBY (see SENSE_...)
    constexpr std::int32_t STATE_BLOCK_NEARBY_SENSES = 10;
    // The plugin tells how long it took in tenths of a millisecond: it is often done within one
    constexpr std::int64_t MICROSECONDS_PER_TIME_STEP = 100;

    constexpr std::size_t STATE_PLAYER_VALUE_COUNT = 11;
    constexpr std::size_t STATE_WORLD_VALUE_COUNT = 7;
    constexpr std::size_t STATE_COMPANION_VALUE_COUNT = 22;
    constexpr std::int32_t WEATHER_NOT_KNOWN = -1;
    constexpr std::int32_t WEATHER_PLEASANT = 0;
    constexpr std::int32_t WEATHER_CLOUDY = 1;
    constexpr std::int32_t WEATHER_RAINY = 2;
    constexpr std::int32_t WEATHER_SNOW = 3;
    // Fog is no kind of weather for the game: its fog weathers are marked as cloudy, and what only has clouds in the
    // sky is marked as pleasant. They are told apart by how far the fog of the weather reaches by day (game units):
    // marked cloudy and no further than WEATHER_FOG_FAR_UNITS is fog, no further than WEATHER_DENSE_FOG_FAR_UNITS is
    // dense fog (SkyrimFog has 25000, RiftenOvercastFog and SkyrimOvercastWar have 9000, the weathers with clouds
    // 50000 to 100000). Marked snow and no further than WEATHER_FOG_FAR_UNITS is a snowstorm (SkyrimStormSnow has
    // 20000, SkyrimOvercastSnow 50000). A weather mod may bring other numbers
    constexpr std::int32_t WEATHER_FOG = 4;
    constexpr std::int32_t WEATHER_DENSE_FOG = 5;
    constexpr std::int32_t WEATHER_SNOWSTORM = 6;
    constexpr float WEATHER_FOG_FAR_UNITS = 30000.0f;
    constexpr float WEATHER_DENSE_FOG_FAR_UNITS = 14000.0f;
    constexpr float MINUTES_PER_DAY = 24.0f * MINUTES_PER_HOUR;
    constexpr std::int32_t HORSE_NOT_LOADED = -1;
    constexpr std::string_view UNKNOWN_LOCATION_NAME = "Unknown";
    constexpr char STATE_TEXT_SEPARATOR = '\t';

    std::mutex wholeStateLock;
    // The texts that belong to the numbers GetWholeState returned last
    std::vector<std::string> wholeStateTexts;

    std::int32_t OptionOf(const std::vector<std::int32_t>& options, std::size_t place) { return place < options.size() ? options[place] : 0; }

    RE::TESForm* FormOf(const std::vector<RE::TESForm*>& forms, std::size_t place) { return place < forms.size() ? forms[place] : nullptr; }

    std::int32_t IdOf(const RE::TESForm* form) { return form ? static_cast<std::int32_t>(form->GetFormID()) : 0; }

    void AddStateBlock(std::vector<std::int32_t>& numbers, std::int32_t block, const std::vector<std::int32_t>& values) {
        numbers.push_back(block);
        numbers.push_back(static_cast<std::int32_t>(values.size()));
        numbers.insert(numbers.end(), values.begin(), values.end());
    }

    void AddStateText(std::vector<std::string>& texts, std::string_view key, std::string_view text) {
        std::string line(key);
        line += STATE_TEXT_SEPARATOR;
        line += text;
        texts.push_back(std::move(line));
    }

    void AddStateTexts(std::vector<std::string>& texts, std::string_view key, const std::vector<std::string>& lines) {
        for (const auto& line : lines) AddStateText(texts, key, line);
    }

    // The character a quest keeps in one of its aliases, or nullptr. This is what ReferenceAlias.GetActorReference
    // tells a script (see RE::BGSRefAlias::GetReference)
    RE::Actor* GetAliasActor(const RE::TESForm* questForm, std::int32_t aliasID) {
        const auto* quest = questForm ? questForm->As<RE::TESQuest>() : nullptr;
        if (!quest || aliasID < 0) return nullptr;
        const auto reference = quest->GetAliasedRef(static_cast<std::uint32_t>(aliasID)).get();
        return reference ? reference->As<RE::Actor>() : nullptr;
    }

    // The name of the place the player is in: the location, or else the cell, or else the world. Indoors the name of
    // the cell comes first, with the location behind it: in a test the state only said "Helgen" inside Helgen Keep,
    // because the location of a town also holds for the buildings in it. The game has no names for single rooms
    std::string GetLocationNameOf(const RE::Actor* player) {
        std::string name;
        if (const auto* location = player->GetCurrentLocation()) name = CleanText(ViewOf(location->GetName()));
        if (const auto* indoorCell = player->GetParentCell(); indoorCell && indoorCell->IsInteriorCell()) {
            const std::string room = CleanText(ViewOf(indoorCell->GetFullName()));
            if (!room.empty() && !EqualsIgnoreCase(room, name)) name = name.empty() ? room : room + ", " + name;
        }
        if (name.empty()) {
            if (const auto* cell = player->GetParentCell()) name = CleanText(ViewOf(cell->GetName()));
        }
        if (name.empty()) {
            if (const auto* world = player->GetWorldspace()) name = CleanText(ViewOf(world->GetName()));
        }
        if (name.empty()) name = UNKNOWN_LOCATION_NAME;
        return name;
    }

    // The weather as Weather.GetClassification tells it to a script: WEATHER_PLEASANT to WEATHER_SNOW by the kind
    // the weather is marked as, WEATHER_NOT_KNOWN without a weather or without a mark
    std::int32_t GetWeatherClass() {
        const auto* sky = RE::Sky::GetSingleton();
        const auto* weather = sky ? sky->currentWeather : nullptr;
        if (!weather) return WEATHER_NOT_KNOWN;
        const auto& marks = weather->data.flags;
        const float fogFar = weather->fogData.dayFar;
        const bool isFoggy = fogFar > 0.0f && fogFar <= WEATHER_FOG_FAR_UNITS;
        if (marks.any(RE::TESWeather::WeatherDataFlag::kPleasant)) return WEATHER_PLEASANT;
        if (marks.any(RE::TESWeather::WeatherDataFlag::kCloudy)) {
            if (isFoggy) return fogFar <= WEATHER_DENSE_FOG_FAR_UNITS ? WEATHER_DENSE_FOG : WEATHER_FOG;
            return WEATHER_CLOUDY;
        }
        if (marks.any(RE::TESWeather::WeatherDataFlag::kRainy)) return WEATHER_RAINY;
        if (marks.any(RE::TESWeather::WeatherDataFlag::kSnow)) return isFoggy ? WEATHER_SNOWSTORM : WEATHER_SNOW;
        return WEATHER_NOT_KNOWN;
    }

    // The time of day in minutes since midnight, worked out like the script did: from the days since the game began
    std::int32_t GetMinutesOfDay() {
        const auto* calendar = RE::Calendar::GetSingleton();
        if (!calendar) return 0;
        const float days = calendar->GetCurrentGameTime();
        return static_cast<std::int32_t>((days - std::floor(days)) * MINUTES_PER_DAY);
    }

    // The name the script gave a horse with rename_horse, or an empty text if it gave none
    std::string GetKeptHorseName(const RE::Actor* horse, const std::vector<RE::TESForm*>& namedHorses, const std::vector<RE::BSFixedString>& horseNames) {
        if (namedHorses.size() != horseNames.size()) return std::string();
        for (std::size_t place = 0; place < namedHorses.size(); ++place) {
            if (namedHorses[place] == horse) return std::string(ViewOf(horseNames[place].c_str()));
        }
        return std::string();
    }

    // The name of a horse for the state. The game sometimes forgets a name a script gave: then the name the script
    // gave is told, and isNameLost says that the script has to give it again
    std::string GetHorseNameOf(RE::Actor* horse, const std::vector<RE::TESForm*>& namedHorses, const std::vector<RE::BSFixedString>& horseNames,
                               bool& isNameLost) {
        const std::string shownName(ViewOf(horse->GetDisplayFullName()));
        const std::string keptName = GetKeptHorseName(horse, namedHorses, horseNames);
        // Texts that come from a script may differ in case from what the game shows (see FindLooseItems)
        isNameLost = !keptName.empty() && !EqualsIgnoreCase(keptName, shownName);
        return CleanText(isNameLost ? keptName : shownName);
    }

    // Health in percent of what the character has for good, rounded like the script rounds it. In a test the script
    // told 109 percent for a companion with more health than usual for a while, so what is added for a while does
    // not count as full health there either
    std::int32_t GetHealthPercentOf(RE::Actor* actor) {
        const auto* values = actor->AsActorValueOwner();
        if (!values) return 0;
        const float full = values->GetPermanentActorValue(RE::ActorValue::kHealth);
        if (full <= 0.0f) return 0;
        return static_cast<std::int32_t>(values->GetActorValue(RE::ActorValue::kHealth) / full * PERCENT_FULL + 0.5f);
    }

    std::int32_t GetSitStateOf(RE::Actor* actor) {
        const auto* state = actor->AsActorState();
        if (!state) return REST_STATE_NONE;
        switch (state->GetSitSleepState()) {
            case RE::SIT_SLEEP_STATE::kWantToSit:
            case RE::SIT_SLEEP_STATE::kWaitingForSitAnim:
                return REST_STATE_WANTED;
            case RE::SIT_SLEEP_STATE::kIsSitting:
                return REST_STATE_DOING;
            case RE::SIT_SLEEP_STATE::kWantToStand:
                return REST_STATE_ENDING;
            default:
                return REST_STATE_NONE;
        }
    }

    std::int32_t GetSleepStateOf(RE::Actor* actor) {
        const auto* state = actor->AsActorState();
        if (!state) return REST_STATE_NONE;
        switch (state->GetSitSleepState()) {
            case RE::SIT_SLEEP_STATE::kWantToSleep:
            case RE::SIT_SLEEP_STATE::kWaitingForSleepAnim:
                return REST_STATE_WANTED;
            case RE::SIT_SLEEP_STATE::kIsSleeping:
                return REST_STATE_DOING;
            case RE::SIT_SLEEP_STATE::kWantToWake:
                return REST_STATE_ENDING;
            default:
                return REST_STATE_NONE;
        }
    }

    std::int32_t GetRunsOf(RE::Actor* actor) {
        const auto* state = actor->AsActorState();
        return state && (state->IsRunning() || state->IsSprinting()) ? 1 : 0;
    }

    // See the comment above. An empty list means: this state cannot be built here, the script builds it the old way
    std::vector<std::int32_t> GetWholeState(RE::StaticFunctionTag*, RE::Actor* player, RE::Actor* companion, RE::Actor* viewer,
                                            std::vector<RE::TESForm*> ownForms, std::vector<std::int32_t> options,
                                            std::vector<RE::TESForm*> namedHorses, std::vector<RE::BSFixedString> horseNames) {
        const auto startTime = std::chrono::steady_clock::now();
        std::vector<std::int32_t> numbers;
        std::vector<std::string> texts;
        // 'visible' is only built here with the companion as the viewer: what the player sees, the game tells by its camera
        const bool isVisibleOnly = OptionOf(options, STATE_OPTION_VISIBLE) != 0;
        const std::int32_t visibleHalfAngle = OptionOf(options, STATE_OPTION_VISIBLE_HALF_ANGLE);
        const bool canBuild = player && viewer && viewer->Is3DLoaded() && options.size() >= STATE_OPTION_COUNT && ownForms.size() >= STATE_FORM_COUNT &&
                              (!isVisibleOnly || (viewer != player && visibleHalfAngle > 0));
        if (!canBuild) {
            const std::lock_guard<std::mutex> guard(wholeStateLock);
            wholeStateTexts.clear();
            return numbers;
        }

        // The characters around come first: if the game cannot be searched, nothing else is touched
        const auto* viewerCell = viewer->GetParentCell();
        const bool isViewerIndoors = viewerCell && viewerCell->IsInteriorCell();
        std::vector<std::int32_t> nearbySenses;
        const std::vector<std::int32_t> nearbyValues = CollectNearbyValues(
            viewer, companion, OptionOf(options, isViewerIndoors ? STATE_OPTION_INDOOR_RADIUS : STATE_OPTION_OUTDOOR_RADIUS),
            OptionOf(options, STATE_OPTION_LIST_LIMIT), isVisibleOnly ? visibleHalfAngle : NO_SIGHT_FILTER, nearbySenses);
        if (nearbyValues.size() == 1) {
            const std::lock_guard<std::mutex> guard(wholeStateLock);
            wholeStateTexts.clear();
            return numbers;
        }

        // Whose own sentences of the game are cut off (see SetMutedSpeaker): the script told that with every state
        SetMutedSpeaker(nullptr, OptionOf(options, STATE_OPTION_MUTE) != 0 ? companion : nullptr);

        numbers.assign(STATE_HEAD_COUNT, 0);
        numbers[STATE_HEAD_LENGTH] = static_cast<std::int32_t>(STATE_HEAD_COUNT);
        // What the script used to ask the game: worked out here, told to the script, and used where the script says so
        const std::int32_t pluginFacts = OptionOf(options, STATE_OPTION_PLUGIN_FACTS);
        numbers[STATE_HEAD_PLAYER_HEALTH] = GetHealthPercentOf(player);
        numbers[STATE_HEAD_PLAYER_SIT_STATE] = GetSitStateOf(player);
        numbers[STATE_HEAD_PLAYER_SLEEP_STATE] = GetSleepStateOf(player);
        if (companion) {
            numbers[STATE_HEAD_COMPANION_HEALTH] = GetHealthPercentOf(companion);
            numbers[STATE_HEAD_COMPANION_SIT_STATE] = GetSitStateOf(companion);
            numbers[STATE_HEAD_COMPANION_RUNS] = GetRunsOf(companion);
        }
        const bool isHealthOwn = (pluginFacts & STATE_FACT_HEALTH) != 0;
        const bool isSittingOwn = (pluginFacts & STATE_FACT_SITTING) != 0;
        const bool isRunningOwn = (pluginFacts & STATE_FACT_RUNNING) != 0;
        // The script fills in the first of the two times in milliseconds. The second is how long this function took,
        // in tenths of a millisecond (see MICROSECONDS_PER_TIME_STEP)
        AddStateBlock(numbers, STATE_BLOCK_TIMES, {0, 0});
        const std::size_t placeOfOwnTime = numbers.size() - 1;

        AddStateText(texts, "location", GetLocationNameOf(player));
        AddStateText(texts, "player_name", CleanText(ViewOf(player->GetDisplayFullName())));

        const std::vector<std::int32_t> playerNumbers = GetActorNumbers(nullptr, player, nullptr);
        const auto* playerCell = player->GetParentCell();
        std::vector<std::int32_t> playerValues(STATE_PLAYER_VALUE_COUNT, 0);
        playerValues[0] = isHealthOwn ? numbers[STATE_HEAD_PLAYER_HEALTH] : OptionOf(options, STATE_OPTION_PLAYER_HEALTH);
        playerValues[1] = playerNumbers[2];
        playerValues[2] = playerNumbers[3];
        playerValues[3] = playerNumbers[4];
        playerValues[4] = playerNumbers[5];
        playerValues[5] = playerNumbers[6];
        playerValues[6] = playerCell && playerCell->IsInteriorCell() ? 1 : 0;
        playerValues[7] = playerNumbers[7];
        playerValues[8] = isSittingOwn ? numbers[STATE_HEAD_PLAYER_SIT_STATE] : OptionOf(options, STATE_OPTION_PLAYER_SIT_STATE);
        playerValues[9] = isSittingOwn ? numbers[STATE_HEAD_PLAYER_SLEEP_STATE] : OptionOf(options, STATE_OPTION_PLAYER_SLEEP_STATE);
        playerValues[10] = playerNumbers[8];
        AddStateBlock(numbers, STATE_BLOCK_PLAYER, playerValues);
        numbers[STATE_HEAD_PLAYER_RIDES] = playerNumbers[14];

        const bool isHearingWanted = OptionOf(options, STATE_OPTION_HEARING) != 0;
        std::vector<std::int32_t> worldValues(STATE_WORLD_VALUE_COUNT, 0);
        worldValues[0] = GetWeatherClass();
        worldValues[1] = GetMinutesOfDay();
        worldValues[2] = INTERFACE_VERSION;
        worldValues[3] = isHearingWanted ? 1 : 0;
        worldValues[4] = OptionOf(options, STATE_OPTION_PROTOCOL);
        worldValues[5] = viewer != player ? 1 : 0;
        worldValues[6] = isVisibleOnly ? 1 : 0;
        AddStateBlock(numbers, STATE_BLOCK_WORLD, worldValues);

        const std::int32_t maxQuests = OptionOf(options, STATE_OPTION_MAX_QUESTS);
        AddStateTexts(texts, "quests", GetOpenQuests(nullptr, maxQuests));
        AddStateTexts(texts, "quest_targets", GetQuestTargets(nullptr, maxQuests, OptionOf(options, STATE_OPTION_MAX_QUEST_TARGETS), viewer));
        // 'visible': only the places and doors in front of the viewer with nothing solid in between
        AddStateTexts(texts, "places",
                      GetPlaceLines(OptionOf(options, STATE_OPTION_PLACE_RADIUS), OptionOf(options, STATE_OPTION_MAX_PLACE_MARKERS),
                                    OptionOf(options, STATE_OPTION_MAX_PLACE_DOORS), viewer, isVisibleOnly ? visibleHalfAngle : NO_SIGHT_FILTER));
        AddStateTexts(texts, "traps", GetNearbyTraps(nullptr, OptionOf(options, STATE_OPTION_TRAP_RADIUS), OptionOf(options, STATE_OPTION_MAX_TRAPS), viewer));
        if (isHearingWanted) AddStateTexts(texts, "heard", TakeHeardLines(nullptr, OptionOf(options, STATE_OPTION_MAX_HEARD_LINES)));
        AddStateTexts(texts, "events", TakeEvents(nullptr, OptionOf(options, STATE_OPTION_MAX_EVENT_LINES)));

        // The horses: who rides what is kept in aliases of the quests of the game mod
        const auto* horseQuest = FormOf(ownForms, STATE_FORM_HORSE_QUEST);
        const bool isRideWanted = GetAliasActor(horseQuest, OptionOf(options, STATE_OPTION_ALIAS_RIDER)) != nullptr;

        if (companion) {
            AddStateText(texts, "companion_name", CleanText(ViewOf(companion->GetDisplayFullName())));
            const RE::BSFixedString voiceName = GetVoiceTypeName(nullptr, companion);
            if (!voiceName.empty()) AddStateText(texts, "companion_voice", ViewOf(voiceName.c_str()));
            if (!companion->Is3DLoaded()) {
                // Where the companion is if it is not in the loaded area: the name of the building, if the game has one
                const auto* companionCell = companion->GetParentCell();
                const std::string placeName = companionCell ? CleanText(ViewOf(companionCell->GetName())) : std::string();
                if (!placeName.empty()) AddStateText(texts, "companion_place", placeName);
            }

            const std::vector<std::int32_t> companionNumbers = GetActorNumbers(nullptr, companion, player);
            std::vector<std::int32_t> companionValues(STATE_COMPANION_VALUE_COUNT, 0);
            companionValues[0] = companionNumbers[0];
            companionValues[1] = isHealthOwn ? numbers[STATE_HEAD_COMPANION_HEALTH] : OptionOf(options, STATE_OPTION_COMPANION_HEALTH);
            companionValues[2] = companionNumbers[1];
            companionValues[3] = companionNumbers[2];
            const bool isHeldOnHorse = OptionOf(options, STATE_OPTION_RIDE_HOLD) != 0 && isRideWanted;
            companionValues[4] = companionNumbers[13] != 0 || isHeldOnHorse ? 1 : 0;
            // Far away (eg left behind in another place) the game has no position worth measuring
            if (companionNumbers[9] != 0) {
                companionValues[5] = 1;
                companionValues[6] = companionNumbers[10];
                companionValues[7] = companionNumbers[11];
                companionValues[9] = companionNumbers[6];
            }
            companionValues[8] = companionNumbers[12];
            companionValues[10] = OptionOf(options, STATE_OPTION_FLEEING);
            if (const auto* walkTarget = FormOf(ownForms, STATE_FORM_WALK_TARGET)) {
                companionValues[11] = IdOf(walkTarget);
            } else if (isRideWanted) {
                companionValues[11] = IdOf(FormOf(ownForms, STATE_FORM_RIDE_TARGET));
            }
            companionValues[12] = isSittingOwn ? numbers[STATE_HEAD_COMPANION_SIT_STATE] : OptionOf(options, STATE_OPTION_COMPANION_SIT_STATE);
            companionValues[13] = companionNumbers[7];
            companionValues[14] = IdOf(FormOf(ownForms, STATE_FORM_LOOK_TARGET));
            if (const auto* leader = FormOf(ownForms, STATE_FORM_LEADER)) {
                companionValues[15] = IdOf(leader);
            } else if (isRideWanted) {
                companionValues[15] = IdOf(FormOf(ownForms, STATE_FORM_RIDE_LEADER));
            }
            companionValues[16] = OptionOf(options, STATE_OPTION_KEEPS_WEAPON_AWAY);
            companionValues[17] = isRunningOwn ? numbers[STATE_HEAD_COMPANION_RUNS] : OptionOf(options, STATE_OPTION_COMPANION_RUNS);
            companionValues[18] = companionNumbers[8];
            companionValues[19] = OptionOf(options, STATE_OPTION_SNEAK_MODE);
            companionValues[20] = OptionOf(options, STATE_OPTION_AUTO_HEAL_PERCENT);
            companionValues[21] = OptionOf(options, STATE_OPTION_LEAD_STATE);
            AddStateBlock(numbers, STATE_BLOCK_COMPANION, companionValues);
            numbers[STATE_HEAD_COMPANION_RIDES] = companionNumbers[14];

            // What the companion holds, as GetHandNames tells it, with the names made safe to send
            for (const auto& hand : GetHandNames(nullptr, companion)) {
                const std::string_view handView(hand);
                const auto colon = handView.find(':');
                if (colon == std::string_view::npos) continue;
                AddStateText(texts, "hands", std::string(handView.substr(0, colon + 1)) + CleanText(handView.substr(colon + 1)));
            }
        }

        // Gold of the player and of the companion (0 without a companion)
        AddStateBlock(numbers, STATE_BLOCK_GOLD, {CountInInventory(player, GOLD_FORM_ID), companion ? CountInInventory(companion, GOLD_FORM_ID) : 0});

        if (const auto* calledHorse = GetAliasActor(FormOf(ownForms, STATE_FORM_HORSE_CALL_QUEST), OptionOf(options, STATE_OPTION_ALIAS_CALLED_HORSE))) {
            AddStateBlock(numbers, STATE_BLOCK_HORSE_CALLED, {IdOf(calledHorse)});
        }
        auto* playersHorse = GetAliasActor(FormOf(ownForms, STATE_FORM_PLAYER_HORSE_QUEST), OptionOf(options, STATE_OPTION_ALIAS_PLAYER_HORSE));
        if (playersHorse && !playersHorse->IsDead()) {
            const std::vector<std::int32_t> horseNumbers = GetActorNumbers(nullptr, playersHorse, player);
            std::vector<std::int32_t> horseValues = {horseNumbers[0], HORSE_NOT_LOADED, 0};
            if (horseNumbers[9] != 0) {
                horseValues[1] = horseNumbers[10];
                horseValues[2] = horseNumbers[11];
            }
            AddStateBlock(numbers, STATE_BLOCK_PLAYER_HORSE, horseValues);
            bool isNameLost = false;
            AddStateText(texts, "player_horse_name", GetHorseNameOf(playersHorse, namedHorses, horseNames, isNameLost));
            numbers[STATE_HEAD_PLAYER_HORSE_NAME_LOST] = isNameLost ? 1 : 0;
        }
        auto* ownHorse = companion ? GetAliasActor(horseQuest, OptionOf(options, STATE_OPTION_ALIAS_HORSE)) : nullptr;
        if (ownHorse && !ownHorse->IsDead()) {
            const std::vector<std::int32_t> horseNumbers = GetActorNumbers(nullptr, ownHorse, companion);
            std::vector<std::int32_t> horseValues = {horseNumbers[0], HORSE_NOT_LOADED, 0, isRideWanted ? 1 : 0, OptionOf(options, STATE_OPTION_RIDE_LOCKED)};
            if (horseNumbers[9] != 0 && companion->Is3DLoaded()) {
                horseValues[1] = horseNumbers[10];
                horseValues[2] = horseNumbers[11];
            }
            AddStateBlock(numbers, STATE_BLOCK_HORSE, horseValues);
            bool isNameLost = false;
            AddStateText(texts, "horse_name", GetHorseNameOf(ownHorse, namedHorses, horseNames, isNameLost));
            numbers[STATE_HEAD_OWN_HORSE_NAME_LOST] = isNameLost ? 1 : 0;
        }

        if (nearbyValues.size() > 1) {
            AddStateBlock(numbers, STATE_BLOCK_NEARBY, nearbyValues);
            if (isVisibleOnly) AddStateBlock(numbers, STATE_BLOCK_NEARBY_SENSES, nearbySenses);
            AddStateTexts(texts, "nearby_names", GetNearbyNames(nullptr));
        }

        numbers[placeOfOwnTime] = static_cast<std::int32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - startTime).count() / MICROSECONDS_PER_TIME_STEP);
        {
            const std::lock_guard<std::mutex> guard(wholeStateLock);
            wholeStateTexts = std::move(texts);
        }
        return numbers;
    }

    // The texts that belong to the numbers GetWholeState returned last: the name of the entry, a tab, the text
    std::vector<std::string> GetWholeStateTexts(RE::StaticFunctionTag*) {
        const std::lock_guard<std::mutex> guard(wholeStateLock);
        return wholeStateTexts;
    }

    // ------------------------------------------------------------------ cover
    // cover: a spot near the companion that none of its enemies can see. The spots that are tried lie on rings around
    // the companion. Each is pulled onto the navmesh of the game first: the ground the game lets characters walk on.
    // So a spot on a roof, inside a rock or behind the wall of the next room is not taken. Whether the enemies see a
    // spot is told by rays, from the eyes of each enemy to two heights of a body standing there.
    // The navmesh is asked on the main thread of the game (see RequestCover): nothing here could tell whether those
    // functions of the game may be called from the thread of a script, and the game itself asks them from its own threads.
    // The game has a search for cover of its own (CombatCoverSearch), but only its name is known, so it is not used

    // RE::Pathing::GetSingleton, RE::Pathing::GetPathingCell and RE::Pathing::FindClosestPointOnNavmesh, by their IDs
    // for Skyrim SE (see HasGameAddress)
    constexpr std::uint64_t PATHING_SINGLETON_ID = 514893;
    constexpr std::uint64_t PATHING_CELL_ID = 29866;
    constexpr std::uint64_t NAVMESH_CLOSEST_POINT_ID = 29832;
    // Who counts as an enemy to hide from: hostile characters within this distance, the nearest ones
    constexpr float COVER_ENEMY_RADIUS_UNITS = 60.0f * GAME_UNITS_PER_METER;
    constexpr std::size_t COVER_MAX_ENEMIES = 8;
    // The rings: the first one this far out, the next ones this much further each, with this many spots on a ring
    constexpr float COVER_FIRST_RING_UNITS = 2.0f * GAME_UNITS_PER_METER;
    constexpr float COVER_RING_STEP_UNITS = 1.5f * GAME_UNITS_PER_METER;
    constexpr std::int32_t COVER_DIRECTIONS = 16;
    constexpr float COVER_FULL_TURN = 2.0f * std::numbers::pi_v<float>;
    // A spot counts as walkable if the navmesh is no further from it than this to the side, and if it lies no more
    // than this much higher or lower than the companion (another floor is another place)
    constexpr float COVER_SNAP_UNITS = 1.5f * GAME_UNITS_PER_METER;
    constexpr float COVER_HEIGHT_UNITS = 4.0f * GAME_UNITS_PER_METER;
    // The two heights of the body an enemy must not see, and how close to the body a ray may end and still count as
    // stopped by something in between
    constexpr std::array<float, 2> COVER_BODY_HEIGHTS = {50.0f, 120.0f};
    constexpr float COVER_BODY_UNITS = 20.0f;
    // Losing more health than this between two looks counts as being hit
    constexpr float COVER_HIT_HEALTH = 1.0f;
    // What TakeCoverResult tells first: a spot was found, the companion is covered where it stands, there is no
    // such spot, there is no enemy, or the plugin cannot look. Then x, y and z of the spot and how many enemies
    constexpr float COVER_RESULT_FOUND = 1.0f;
    constexpr float COVER_RESULT_HERE = 2.0f;
    constexpr float COVER_RESULT_NONE = 0.0f;
    constexpr float COVER_RESULT_NO_ENEMY = -1.0f;
    constexpr float COVER_RESULT_CANNOT = -2.0f;
    constexpr std::size_t COVER_PLACE_OF_RESULT = 0;
    constexpr std::size_t COVER_PLACE_OF_ENEMIES = 4;
    constexpr float NOT_COVERED = -1.0f;

    struct CoverEnemy {
        RE::NiPoint3 feet;
        RE::NiPoint3 eyes;
        float distanceUnits = 0.0f;
        std::string name;
    };

    // Whether something solid of the world stands between these eyes and the whole body of someone standing at "feet".
    // Only call this while the physics are locked for reading
    bool IsCoveredFrom(RE::bhkWorld* physics, float scale, const RE::NiPoint3& eyes, const RE::NiPoint3& feet) {
        for (const float height : COVER_BODY_HEIGHTS) {
            const RE::NiPoint3 point(feet.x, feet.y, feet.z + height);
            const float length = (point - eyes).Length();
            const RayHit hit = CastRay(physics, scale, eyes, point);
            if (!hit.isHit || hit.distance >= length - COVER_BODY_UNITS) return false;
        }
        return true;
    }

    // The enemies around the companion that live and are there, the nearest first
    std::vector<CoverEnemy> CollectCoverEnemies(RE::Actor* companion) {
        std::vector<CoverEnemy> enemies;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !HasGameAddress(HOSTILE_TO_ACTOR_ID)) return enemies;
        const RE::NiPoint3 own = companion->GetPosition();
        ForEachReferenceAround(companion, COVER_ENEMY_RADIUS_UNITS, [&](RE::TESObjectREFR* reference) {
            if (!reference || reference == companion || reference == player || reference->IsDisabled() || reference->IsMarkedForDeletion()) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            auto* actor = reference->As<RE::Actor>();
            if (!actor || !actor->Is3DLoaded() || actor->IsDead() || actor->IsPlayerTeammate()) return RE::BSContainer::ForEachResult::kContinue;
            if (!actor->IsHostileToActor(player) && !actor->IsHostileToActor(companion)) return RE::BSContainer::ForEachResult::kContinue;
            CoverEnemy enemy;
            enemy.feet = actor->GetPosition();
            enemy.eyes = RE::NiPoint3(enemy.feet.x, enemy.feet.y, enemy.feet.z + SIGHT_EYE_UNITS);
            enemy.distanceUnits = own.GetDistance(enemy.feet);
            enemy.name = CleanText(ViewOf(actor->GetDisplayFullName()));
            enemies.push_back(std::move(enemy));
            return RE::BSContainer::ForEachResult::kContinue;
        });
        std::sort(enemies.begin(), enemies.end(), [](const CoverEnemy& left, const CoverEnemy& right) { return left.distanceUnits < right.distanceUnits; });
        if (enemies.size() > COVER_MAX_ENEMIES) enemies.resize(COVER_MAX_ENEMIES);
        return enemies;
    }

    // The point of the navmesh that is closest to a spot, if the game has a navmesh there
    bool SnapToNavmesh(RE::Pathing* pathing, RE::TESObjectCELL* cell, RE::TESWorldSpace* world, const RE::NiPoint3& spot, RE::NiPoint3& snapped) {
        RE::BSTSmartPointer<RE::BSPathingCell> pathingCell;
        if (!pathing->GetPathingCell(spot, cell, world, pathingCell) || !pathingCell) return false;
        return pathing->FindClosestPointOnNavmesh(pathingCell, spot, snapped);
    }

    // Looks for cover within radiusUnits around the companion, at least enemyUnits away from every enemy.
    // mustMove: the spot the companion stands on does not count, and neither does one right next to it.
    // Returns what TakeCoverResult hands on (see COVER_RESULT_...). Only call this on the main thread
    std::vector<float> FindCover(RE::Actor* companion, float radiusUnits, float enemyUnits, bool mustMove) {
        std::vector<float> result = {COVER_RESULT_CANNOT, 0.0f, 0.0f, 0.0f, 0.0f};
        if (!companion || companion->IsDead() || !companion->Is3DLoaded()) return result;
        if (!HasGameAddress(CELL_PHYSICS_ID) || !HasGameAddress(PHYSICS_LOCK_ID) || !HasGameAddress(PHYSICS_UNLOCK_ID)) return result;
        if (!HasGameAddress(PATHING_SINGLETON_ID) || !HasGameAddress(PATHING_CELL_ID) || !HasGameAddress(NAVMESH_CLOSEST_POINT_ID)) return result;
        auto* cell = companion->GetParentCell();
        auto* physics = cell ? cell->GetbhkWorld() : nullptr;
        auto* pathing = RE::Pathing::GetSingleton();
        if (!physics || !pathing) return result;

        const std::vector<CoverEnemy> enemies = CollectCoverEnemies(companion);
        result[COVER_PLACE_OF_ENEMIES] = static_cast<float>(enemies.size());
        if (enemies.empty()) {
            result[COVER_PLACE_OF_RESULT] = COVER_RESULT_NO_ENEMY;
            return result;
        }
        auto* world = cell->IsExteriorCell() ? cell->GetRuntimeData().worldSpace : nullptr;
        const float scale = HasGameAddress(PHYSICS_SCALE_ID) ? RE::bhkWorld::GetWorldScale() : DEFAULT_PHYSICS_SCALE;
        const RE::NiPoint3 own = companion->GetPosition();
        const float enemyCount = static_cast<float>(enemies.size());

        const RE::BSReadLockGuard guard(physics->worldLock);
        // How far the nearest enemy is from a spot that is cover from all of them, or NOT_COVERED
        const auto rate = [&](const RE::NiPoint3& feet) {
            float nearest = COVER_ENEMY_RADIUS_UNITS * 2.0f;
            for (const CoverEnemy& enemy : enemies) {
                const float apart = enemy.feet.GetDistance(feet);
                if (apart < enemyUnits) return NOT_COVERED;
                if (!IsCoveredFrom(physics, scale, enemy.eyes, feet)) return NOT_COVERED;
                nearest = std::min(nearest, apart);
            }
            return nearest;
        };
        if (!mustMove && rate(own) > NOT_COVERED) {
            result = {COVER_RESULT_HERE, own.x, own.y, own.z, enemyCount};
            return result;
        }
        // The nearest ring that has cover wins, and on it the spot furthest from the enemies
        std::int32_t ringNumber = 0;
        for (float ring = COVER_FIRST_RING_UNITS; ring <= radiusUnits; ring += COVER_RING_STEP_UNITS) {
            float bestApart = NOT_COVERED;
            RE::NiPoint3 best;
            // Every other ring is turned by half a step, so that the spots do not lie on the same lines
            const float turn = ringNumber % 2 == 0 ? 0.0f : 0.5f;
            ++ringNumber;
            for (std::int32_t direction = 0; direction < COVER_DIRECTIONS; ++direction) {
                const float angle = (static_cast<float>(direction) + turn) * COVER_FULL_TURN / static_cast<float>(COVER_DIRECTIONS);
                const RE::NiPoint3 spot(own.x + std::sin(angle) * ring, own.y + std::cos(angle) * ring, own.z);
                RE::NiPoint3 snapped;
                if (!SnapToNavmesh(pathing, cell, world, spot, snapped)) continue;
                const float sideEast = snapped.x - spot.x;
                const float sideNorth = snapped.y - spot.y;
                if (std::sqrt(sideEast * sideEast + sideNorth * sideNorth) > COVER_SNAP_UNITS) continue;
                if (std::abs(snapped.z - own.z) > COVER_HEIGHT_UNITS) continue;
                const float apart = rate(snapped);
                if (apart > bestApart) {
                    bestApart = apart;
                    best = snapped;
                }
            }
            if (bestApart > NOT_COVERED) {
                result = {COVER_RESULT_FOUND, best.x, best.y, best.z, enemyCount};
                return result;
            }
        }
        result[COVER_PLACE_OF_RESULT] = COVER_RESULT_NONE;
        return result;
    }

    std::mutex coverLock;
    std::vector<float> coverResult;
    bool isCoverPending = false;
    RE::FormID coverWatchedID = 0;
    float coverLastHealth = 0.0f;

    // Starts the search for cover (see FindCover). It runs a moment later, on the main thread of the game, and
    // TakeCoverResult hands out what it found. Returns whether the search could be handed to the game
    bool RequestCover(RE::StaticFunctionTag*, RE::Actor* companion, std::int32_t radiusMeters, std::int32_t enemyMeters, bool mustMove) {
        const auto* tasks = SKSE::GetTaskInterface();
        if (!companion || !tasks) return false;
        {
            const std::lock_guard<std::mutex> guard(coverLock);
            coverResult.clear();
            isCoverPending = true;
        }
        const auto handle = companion->GetHandle();
        tasks->AddTask([handle, radiusMeters, enemyMeters, mustMove]() {
            const auto seeker = handle.get();
            std::vector<float> found = FindCover(seeker.get(), static_cast<float>(radiusMeters) * GAME_UNITS_PER_METER,
                                                 static_cast<float>(enemyMeters) * GAME_UNITS_PER_METER, mustMove);
            const std::lock_guard<std::mutex> guard(coverLock);
            coverResult = std::move(found);
            isCoverPending = false;
        });
        return true;
    }

    // What the search RequestCover started has found, or an empty list while it is still running
    std::vector<float> TakeCoverResult(RE::StaticFunctionTag*) {
        const std::lock_guard<std::mutex> guard(coverLock);
        if (isCoverPending) return {};
        return coverResult;
    }

    // Why the companion should leave the cover it stands in, as the start of a sentence, or an empty text if there is
    // no reason: it has lost health since the last look, or an enemy that sees it has come closer than nearMeters.
    // isFirst: the first look at this cover, so there is no health from before to compare with
    std::string GetCoverThreat(RE::StaticFunctionTag*, RE::Actor* companion, std::int32_t nearMeters, bool isFirst) {
        if (!companion || companion->IsDead() || !companion->Is3DLoaded()) return "";
        const auto* values = companion->AsActorValueOwner();
        const float health = values ? values->GetActorValue(RE::ActorValue::kHealth) : 0.0f;
        bool wasHit = false;
        {
            const std::lock_guard<std::mutex> guard(coverLock);
            wasHit = !isFirst && coverWatchedID == companion->GetFormID() && health < coverLastHealth - COVER_HIT_HEALTH;
            coverWatchedID = companion->GetFormID();
            coverLastHealth = health;
        }
        if (wasHit) return "you were hit";
        if (!HasGameAddress(CELL_PHYSICS_ID) || !HasGameAddress(PHYSICS_LOCK_ID) || !HasGameAddress(PHYSICS_UNLOCK_ID)) return "";
        const auto* cell = companion->GetParentCell();
        auto* physics = cell ? cell->GetbhkWorld() : nullptr;
        if (!physics) return "";
        const std::vector<CoverEnemy> enemies = CollectCoverEnemies(companion);
        const float nearUnits = static_cast<float>(nearMeters) * GAME_UNITS_PER_METER;
        const float scale = HasGameAddress(PHYSICS_SCALE_ID) ? RE::bhkWorld::GetWorldScale() : DEFAULT_PHYSICS_SCALE;
        const RE::NiPoint3 own = companion->GetPosition();
        const RE::BSReadLockGuard guard(physics->worldLock);
        for (const CoverEnemy& enemy : enemies) {
            if (enemy.distanceUnits > nearUnits) break;
            if (!IsCoveredFrom(physics, scale, enemy.eyes, own)) return (enemy.name.empty() ? std::string("an enemy") : enemy.name) + " got too close";
        }
        return "";
    }

    // ------------------------------------------------------------------- view
    // view: what lies in front of the companion as a grid of cells, like a very coarse picture. One ray goes through
    // the middle of every cell, from the eyes of the companion, and tells what ground, wall or ceiling it meets and how
    // far away that is. Characters and things do not come from the rays: the plugin knows where they are, works out
    // which cells their outline (a box) falls into and looks whether something solid stands in front of them.
    // The plugin only hands out numbers and names. The companion server makes the lines of the picture from them
    // (see describe_view in state_text.py of the Mantella fork), so their form can change without a new plugin

    constexpr std::int32_t VIEW_FORMAT = 1;
    constexpr std::int32_t VIEW_MIN_COLUMNS = 4;
    constexpr std::int32_t VIEW_MAX_COLUMNS = 32;
    constexpr std::int32_t VIEW_MIN_ROWS = 3;
    constexpr std::int32_t VIEW_MAX_ROWS = 18;
    constexpr std::int32_t VIEW_MIN_DEGREES = 30;
    constexpr std::int32_t VIEW_MAX_DEGREES = 180;
    constexpr std::int32_t VIEW_RANGE_METERS = 80;
    constexpr float VIEW_RANGE_UNITS = static_cast<float>(VIEW_RANGE_METERS) * GAME_UNITS_PER_METER;
    constexpr float VIEW_THING_RANGE_UNITS = 25.0f * GAME_UNITS_PER_METER;
    // A cell is as high as it is wide. This part of the height of the picture lies above the level of the eyes,
    // the rest below, so that the ground in front of the feet is in it. The picture never reaches straight up or down
    constexpr float VIEW_PART_ABOVE = 0.4f;
    constexpr float VIEW_MAX_HEIGHT_DEGREES = 170.0f;
    // A surface that faces up this much is ground, one that faces down this much is a ceiling, the rest is wall
    constexpr float VIEW_GROUND_NORMAL = 0.6f;
    // A ray that goes down and meets nothing, or ground this far below the feet, has found a drop
    constexpr float VIEW_DROP_UNITS = 2.0f * GAME_UNITS_PER_METER;
    constexpr std::size_t VIEW_MAX_CREATURES = 20;
    constexpr std::size_t VIEW_MAX_THINGS = 20;
    // The boxes: how high and half as wide a character, a door and any other thing counts
    constexpr float VIEW_CREATURE_HEIGHT_UNITS = 130.0f;
    constexpr float VIEW_CREATURE_HALF_UNITS = 35.0f;
    constexpr float VIEW_DOOR_HEIGHT_UNITS = 180.0f;
    constexpr float VIEW_DOOR_HALF_UNITS = 55.0f;
    constexpr float VIEW_THING_HEIGHT_UNITS = 60.0f;
    constexpr float VIEW_THING_HALF_UNITS = 25.0f;
    constexpr float VIEW_NEAREST_UNITS = 1.0f;
    // Whether something stands in front of a character or a thing is told by a ray from the eyes to a point of it.
    // In a test an axe and a book behind the front of a counter were in the picture: the sight check for doors and
    // places (IsInSight) looks at three heights above a position and takes what is within two meters of it for the
    // thing itself, so rays over the counter into thin air counted as seeing the axe on the floor behind it.
    // A thing is looked at as a whole, with one ray to this height above its position, and what the ray meets this
    // close to that point counts as the thing: little for something loose, more for a chest, a seat or a door,
    // which are solid themselves. A character is looked at row by row of the picture, at the height of its body in
    // that row, never right at the edge of its box. One that is dead lies, so its box is low
    constexpr float VIEW_ITEM_AIM_UNITS = 10.0f;
    // (In a second test an axe and tankards on the low board right behind the front of a counter still showed: the
    // ray was allowed to end 20 units short of them, and the front stood closer to them than that)
    constexpr float VIEW_ITEM_NEAR_UNITS = 4.0f;
    // Something loose is small: with the box of a chest a wheel of cheese three meters away filled nine cells
    constexpr float VIEW_ITEM_HEIGHT_UNITS = 20.0f;
    constexpr float VIEW_ITEM_HALF_UNITS = 12.0f;
    // A character keeps its place in a cell before any thing, however close the thing is: in that test the cheese
    // on the counter pushed the innkeeper behind it out of every cell he stood in
    constexpr float VIEW_THING_ORDER_UNITS = 1.0e7f;
    constexpr float VIEW_SOLID_AIM_UNITS = 30.0f;
    constexpr float VIEW_SOLID_NEAR_UNITS = 50.0f;
    constexpr float VIEW_DOOR_AIM_UNITS = 90.0f;
    constexpr float VIEW_DOOR_NEAR_UNITS = 60.0f;
    constexpr float VIEW_CREATURE_NEAR_UNITS = 30.0f;
    constexpr float VIEW_BODY_EDGE_UNITS = 10.0f;
    constexpr float VIEW_LYING_HEIGHT_UNITS = 40.0f;
    // The rays of the picture may go through this many things that are no obstacle. In a test the companion sat on its
    // horse and every ray that went down counted as a drop: the horse under it is many parts, the usual six were used
    // up inside it, and a ray that met nothing on its way down counted as a drop. Such a ray tells nothing now
    constexpr std::int32_t VIEW_RAY_PASSES = 24;
    // Riding: the ground is where the hooves of the horse are, and the eyes are where the game says the head of the
    // rider is, if that lies where the head of a rider can be: this close to the rider sideways and this far above
    // the hooves. Otherwise the eyes count as this far above the hooves
    constexpr float VIEW_RIDING_EYE_UNITS = 170.0f;
    constexpr float VIEW_RIDING_EYE_LOWEST_UNITS = 100.0f;
    constexpr float VIEW_RIDING_EYE_HIGHEST_UNITS = 260.0f;
    constexpr float VIEW_RIDING_EYE_SIDE_UNITS = 100.0f;
    // What the numbers of a cell say: the ground, wall or ceiling the ray met, how many meters away along the ground,
    // whether it found a drop, and the two nearest characters or things in it (their place in the legend, counted
    // from 1, and their meters)
    constexpr std::int32_t VIEW_TERRAIN_NONE = 0;
    constexpr std::int32_t VIEW_TERRAIN_GROUND = 1;
    constexpr std::int32_t VIEW_TERRAIN_WALL = 2;
    constexpr std::int32_t VIEW_TERRAIN_ROOF = 3;
    // Water, as a try with two ways at once, so that a look in the game shows which of them works: the ray met
    // something in the layer for water (see RayHit), or the ground it met lies below the height the cell gives for
    // the water at that spot (RE::TESObjectCELL::GetWaterHeight, by its ID for Skyrim SE), or both. The meters are
    // those to where the ray meets the surface of the water
    constexpr std::int32_t VIEW_TERRAIN_WATER_BY_LAYER = 4;
    constexpr std::int32_t VIEW_TERRAIN_WATER_BY_HEIGHT = 5;
    constexpr std::int32_t VIEW_TERRAIN_WATER_BY_BOTH = 6;
    constexpr std::uint64_t CELL_WATER_HEIGHT_ID = 18543;
    constexpr float VIEW_WATER_DEPTH_UNITS = 5.0f;
    constexpr float VIEW_WATER_LOWEST_HEIGHT = -1000000.0f;
    constexpr float VIEW_WATER_HIGHEST_HEIGHT = 1000000.0f;
    constexpr std::size_t VIEW_ENTRIES_PER_CELL = 2;
    constexpr std::int32_t VIEW_VALUES_PER_CELL = 7;
    // What the numbers of the legend say: a character or a thing, the two halves of its ref ID, and for a character
    // whether it is hostile, dead or the player, for a thing which kind it is
    constexpr std::int32_t VIEW_VALUES_PER_LEGEND = 4;
    constexpr std::int32_t VIEW_IS_CREATURE = 1;
    constexpr std::int32_t VIEW_IS_THING = 2;
    constexpr std::int32_t VIEW_CREATURE_HOSTILE = 1;
    constexpr std::int32_t VIEW_CREATURE_DEAD = 2;
    constexpr std::int32_t VIEW_CREATURE_PLAYER = 4;
    constexpr std::int32_t VIEW_THING_DOOR = 1;
    constexpr std::int32_t VIEW_THING_CONTAINER = 2;
    constexpr std::int32_t VIEW_THING_FURNITURE = 3;
    constexpr std::int32_t VIEW_THING_ITEM = 4;
    constexpr std::uint32_t VIEW_HALF_BITS = 16;
    constexpr std::uint32_t VIEW_HALF_MASK = 0xFFFF;
    constexpr std::string_view VIEW_DOOR_NAME = "Door";
    constexpr std::string_view VIEW_NO_NAME = "unreadable";

    struct ViewEntry {
        std::int32_t kind = VIEW_IS_CREATURE;
        RE::FormID referenceID = 0;
        std::int32_t flags = 0;
        RE::NiPoint3 feet;
        float heightUnits = 0.0f;
        float halfUnits = 0.0f;
        float groundUnits = 0.0f;
        // For the ray that tells whether it can be seen: how high above its position the ray aims (things only), and
        // how close to that point the ray may end
        float aimUnits = 0.0f;
        float nearUnits = 0.0f;
        std::string name;
    };

    struct ViewCell {
        std::int32_t terrain = VIEW_TERRAIN_NONE;
        std::int32_t terrainMeters = 0;
        std::int32_t isDrop = 0;
        std::array<std::int32_t, VIEW_ENTRIES_PER_CELL> entries = {0, 0};
        std::array<float, VIEW_ENTRIES_PER_CELL> entryUnits = {0.0f, 0.0f};
        // What the entries are sorted by: characters first, then things, each the nearest first
        std::array<float, VIEW_ENTRIES_PER_CELL> entryOrder = {0.0f, 0.0f};
    };

    std::mutex viewLock;
    std::vector<std::string> viewNames;

    // Puts an entry of the legend into a cell, which keeps the first ones by "order" (see ViewCell)
    void AddToViewCell(ViewCell& cell, std::int32_t legendNumber, float groundUnits, float order) {
        for (std::size_t place = 0; place < VIEW_ENTRIES_PER_CELL; ++place) {
            if (cell.entries[place] != 0 && cell.entryOrder[place] <= order) continue;
            for (std::size_t moved = VIEW_ENTRIES_PER_CELL - 1; moved > place; --moved) {
                cell.entries[moved] = cell.entries[moved - 1];
                cell.entryUnits[moved] = cell.entryUnits[moved - 1];
                cell.entryOrder[moved] = cell.entryOrder[moved - 1];
            }
            cell.entries[place] = legendNumber;
            cell.entryUnits[place] = groundUnits;
            cell.entryOrder[place] = order;
            return;
        }
    }

    // Whether nothing solid of the world lies between the eyes and this point, or only within nearUnits of it.
    // Only call this while the physics are locked for reading
    bool IsViewPointSeen(RE::bhkWorld* physics, float scale, const RE::NiPoint3& eyes, const RE::NiPoint3& point, float nearUnits) {
        const float length = (point - eyes).Length();
        if (length <= nearUnits) return true;
        const RayHit hit = CastRay(physics, scale, eyes, point, VIEW_RAY_PASSES);
        return !hit.isHit || hit.distance >= length - nearUnits;
    }

    // The picture in front of the companion as numbers: VIEW_FORMAT, columns, rows, degrees from left to right, how
    // many meters the rays reach, how many entries the legend has, how many numbers each entry and each cell has,
    // then the legend, then the cells row by row from the top, each row from left to right. The names of the legend
    // come from GetViewNames. An empty list if the plugin cannot look
    std::vector<std::int32_t> GetView(RE::StaticFunctionTag*, RE::Actor* companion, std::int32_t columns, std::int32_t rows, std::int32_t degrees) {
        std::vector<std::int32_t> numbers;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!companion || !player || companion->IsDead() || !companion->Is3DLoaded()) return numbers;
        if (!HasGameAddress(CELL_PHYSICS_ID) || !HasGameAddress(PHYSICS_LOCK_ID) || !HasGameAddress(PHYSICS_UNLOCK_ID)) return numbers;
        auto* cell = companion->GetParentCell();
        auto* physics = cell ? cell->GetbhkWorld() : nullptr;
        if (!physics) return numbers;
        const bool canAskWater = HasGameAddress(CELL_WATER_HEIGHT_ID);
        columns = std::clamp(columns, VIEW_MIN_COLUMNS, VIEW_MAX_COLUMNS);
        rows = std::clamp(rows, VIEW_MIN_ROWS, VIEW_MAX_ROWS);
        degrees = std::clamp(degrees, VIEW_MIN_DEGREES, VIEW_MAX_DEGREES);

        const float scale = HasGameAddress(PHYSICS_SCALE_ID) ? RE::bhkWorld::GetWorldScale() : DEFAULT_PHYSICS_SCALE;
        // On foot the ground is at the feet and the eyes are a fixed height above them. In the saddle the ground is
        // at the hooves of the horse and the eyes are those of the rider (see VIEW_RIDING_EYE_UNITS)
        const RE::NiPoint3 own = companion->GetPosition();
        RE::NiPointer<RE::Actor> mount;
        const bool isRiding = companion->GetMount(mount) && mount;
        const RE::NiPoint3 feet(own.x, own.y, isRiding ? mount->GetPosition().z : own.z);
        RE::NiPoint3 eyes(feet.x, feet.y, feet.z + (isRiding ? VIEW_RIDING_EYE_UNITS : SIGHT_EYE_UNITS));
        if (isRiding) {
            const RE::NiPoint3 head = companion->GetLookingAtLocation();
            const float headEast = head.x - own.x;
            const float headNorth = head.y - own.y;
            const float headUp = head.z - feet.z;
            if (std::sqrt(headEast * headEast + headNorth * headNorth) <= VIEW_RIDING_EYE_SIDE_UNITS && headUp >= VIEW_RIDING_EYE_LOWEST_UNITS &&
                headUp <= VIEW_RIDING_EYE_HIGHEST_UNITS) {
                eyes = head;
            }
        }
        const RE::Actor* ownMount = isRiding ? mount.get() : nullptr;
        // No further than the companion sees in this weather and light. The rays still go their whole way, so that
        // they can tell a drop from ground that is only too far away to be seen
        const float sightLimit = GetSightUnitsOf(companion);
        const float rangeUnits = std::min(VIEW_RANGE_UNITS, sightLimit);
        const float thingRangeUnits = std::min(VIEW_THING_RANGE_UNITS, sightLimit);
        const float heading = companion->GetAngleZ();
        const float columnAngle = static_cast<float>(degrees) / DEGREES_PER_RADIAN / static_cast<float>(columns);
        const float rowAngle = std::min(columnAngle, VIEW_MAX_HEIGHT_DEGREES / DEGREES_PER_RADIAN / static_cast<float>(rows));
        const float topAngle = static_cast<float>(rows) * rowAngle * VIEW_PART_ABOVE;
        const float halfColumns = static_cast<float>(columns) * 0.5f;

        // The characters and things around, the nearest first
        std::vector<ViewEntry> creatures;
        std::vector<ViewEntry> things;
        const bool canTellHostile = HasGameAddress(HOSTILE_TO_ACTOR_ID);
        bool isPlayerListed = false;
        const auto addCreature = [&](RE::Actor* actor) {
            ViewEntry entry;
            entry.kind = VIEW_IS_CREATURE;
            entry.referenceID = actor->GetFormID();
            entry.feet = actor->GetPosition();
            entry.heightUnits = VIEW_CREATURE_HEIGHT_UNITS;
            entry.halfUnits = VIEW_CREATURE_HALF_UNITS;
            entry.nearUnits = VIEW_CREATURE_NEAR_UNITS;
            entry.name = CleanText(ViewOf(actor->GetDisplayFullName()));
            if (actor == player) {
                entry.flags = VIEW_CREATURE_PLAYER;
                isPlayerListed = true;
            } else if (actor->IsDead()) {
                entry.flags = VIEW_CREATURE_DEAD;
                entry.heightUnits = VIEW_LYING_HEIGHT_UNITS;
            } else if (canTellHostile && actor->IsHostileToActor(player)) {
                entry.flags = VIEW_CREATURE_HOSTILE;
            }
            creatures.push_back(std::move(entry));
        };
        ForEachReferenceAround(companion, rangeUnits, [&](RE::TESObjectREFR* reference) {
            if (!reference || reference == companion || reference->IsDisabled() || reference->IsMarkedForDeletion()) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            if (auto* actor = reference->As<RE::Actor>()) {
                // The horse the companion sits on is not something it sees in front of it
                if (actor != ownMount && actor->Is3DLoaded() && feet.GetDistance(actor->GetPosition()) <= rangeUnits) addCreature(actor);
                return RE::BSContainer::ForEachResult::kContinue;
            }
            const auto* base = reference->GetBaseObject();
            if (!base || feet.GetDistance(reference->GetPosition()) > thingRangeUnits) return RE::BSContainer::ForEachResult::kContinue;
            ViewEntry entry;
            entry.kind = VIEW_IS_THING;
            entry.heightUnits = VIEW_THING_HEIGHT_UNITS;
            entry.halfUnits = VIEW_THING_HALF_UNITS;
            entry.name = CleanText(ViewOf(reference->GetDisplayFullName()));
            if (base->Is(RE::FormType::Door)) {
                entry.flags = VIEW_THING_DOOR;
                entry.heightUnits = VIEW_DOOR_HEIGHT_UNITS;
                entry.halfUnits = VIEW_DOOR_HALF_UNITS;
                entry.aimUnits = VIEW_DOOR_AIM_UNITS;
                entry.nearUnits = VIEW_DOOR_NEAR_UNITS;
                if (entry.name.empty()) entry.name = std::string(VIEW_DOOR_NAME);
            } else {
                const std::int32_t thingKind = GetThingKind(base);
                if (thingKind < 0 || entry.name.empty()) return RE::BSContainer::ForEachResult::kContinue;
                entry.flags = thingKind == THING_IS_CONTAINER ? VIEW_THING_CONTAINER : (thingKind == THING_IS_FURNITURE ? VIEW_THING_FURNITURE : VIEW_THING_ITEM);
                const bool isSolid = thingKind != THING_IS_ITEM;
                entry.aimUnits = isSolid ? VIEW_SOLID_AIM_UNITS : VIEW_ITEM_AIM_UNITS;
                entry.nearUnits = isSolid ? VIEW_SOLID_NEAR_UNITS : VIEW_ITEM_NEAR_UNITS;
                if (!isSolid) {
                    entry.heightUnits = VIEW_ITEM_HEIGHT_UNITS;
                    entry.halfUnits = VIEW_ITEM_HALF_UNITS;
                }
            }
            entry.referenceID = reference->GetFormID();
            entry.feet = reference->GetPosition();
            things.push_back(std::move(entry));
            return RE::BSContainer::ForEachResult::kContinue;
        });
        if (!isPlayerListed && player->Is3DLoaded() && feet.GetDistance(player->GetPosition()) <= rangeUnits) addCreature(player);

        std::vector<ViewCell> cells(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows));
        std::vector<ViewEntry> legend;
        {
            const RE::BSReadLockGuard guard(physics->worldLock);
            // The rays: one through the middle of every cell
            for (std::int32_t row = 0; row < rows; ++row) {
                const float pitch = topAngle - (static_cast<float>(row) + 0.5f) * rowAngle;
                for (std::int32_t column = 0; column < columns; ++column) {
                    const float turn = heading + (static_cast<float>(column) + 0.5f - halfColumns) * columnAngle;
                    const RE::NiPoint3 direction(std::sin(turn) * std::cos(pitch), std::cos(turn) * std::cos(pitch), std::sin(pitch));
                    const RE::NiPoint3 start = eyes + direction * RAY_START_UNITS;
                    const RayHit hit = CastRay(physics, scale, start, eyes + direction * VIEW_RANGE_UNITS, VIEW_RAY_PASSES);
                    ViewCell& viewCell = cells[static_cast<std::size_t>(row) * static_cast<std::size_t>(columns) + static_cast<std::size_t>(column)];
                    if (!hit.isHit) {
                        if (hit.waterDistance >= 0.0f) {
                            // Water with nothing solid behind it within reach
                            const RE::NiPoint3 farSurface = start + direction * hit.waterDistance;
                            const float farEast = farSurface.x - feet.x;
                            const float farNorth = farSurface.y - feet.y;
                            const float farUnits = std::sqrt(farEast * farEast + farNorth * farNorth);
                            if (farUnits > rangeUnits) continue;
                            viewCell.terrain = VIEW_TERRAIN_WATER_BY_LAYER;
                            viewCell.terrainMeters = ToWholeMeters(farUnits);
                            continue;
                        }
                        // A ray that got through to its end on the way down has found a drop. One that was cut short tells nothing
                        viewCell.isDrop = pitch < 0.0f && !hit.isCutShort ? 1 : 0;
                        continue;
                    }
                    const RE::NiPoint3 point = start + direction * hit.distance;
                    // Water in front of what the ray met?
                    const bool isWaterByLayer = hit.waterDistance >= 0.0f;
                    RE::NiPoint3 surface = start + direction * (isWaterByLayer ? hit.waterDistance : 0.0f);
                    bool isWaterByHeight = false;
                    float waterHeight = 0.0f;
                    if (canAskWater && pitch < 0.0f && cell->GetWaterHeight(point, waterHeight) && waterHeight > VIEW_WATER_LOWEST_HEIGHT &&
                        waterHeight < VIEW_WATER_HIGHEST_HEIGHT && point.z < waterHeight - VIEW_WATER_DEPTH_UNITS && start.z > waterHeight) {
                        isWaterByHeight = true;
                        if (!isWaterByLayer) surface = start + direction * ((waterHeight - start.z) / direction.z);
                    }
                    if (isWaterByLayer || isWaterByHeight) {
                        const float waterEast = surface.x - feet.x;
                        const float waterNorth = surface.y - feet.y;
                        const float waterUnits = std::sqrt(waterEast * waterEast + waterNorth * waterNorth);
                        if (waterUnits > rangeUnits) continue;
                        viewCell.terrain = isWaterByLayer && isWaterByHeight ? VIEW_TERRAIN_WATER_BY_BOTH
                                                                             : (isWaterByLayer ? VIEW_TERRAIN_WATER_BY_LAYER : VIEW_TERRAIN_WATER_BY_HEIGHT);
                        viewCell.terrainMeters = ToWholeMeters(waterUnits);
                        continue;
                    }
                    const float east = point.x - feet.x;
                    const float north = point.y - feet.y;
                    const float groundUnits = std::sqrt(east * east + north * north);
                    // Too far away to be seen: the cell stays empty, and it is no drop
                    if (groundUnits > rangeUnits) continue;
                    viewCell.terrainMeters = ToWholeMeters(groundUnits);
                    if (hit.normalUp > VIEW_GROUND_NORMAL) {
                        viewCell.terrain = VIEW_TERRAIN_GROUND;
                        viewCell.isDrop = pitch < 0.0f && point.z < feet.z - VIEW_DROP_UNITS ? 1 : 0;
                    } else if (hit.normalUp < -VIEW_GROUND_NORMAL) {
                        viewCell.terrain = VIEW_TERRAIN_ROOF;
                    } else {
                        viewCell.terrain = VIEW_TERRAIN_WALL;
                    }
                }
            }
            // The characters and things: into the cells their box covers, as far as nothing solid stands in front of them
            const auto place = [&](std::vector<ViewEntry>& entries, std::size_t limit) {
                for (ViewEntry& entry : entries) {
                    const float east = entry.feet.x - feet.x;
                    const float north = entry.feet.y - feet.y;
                    entry.groundUnits = std::sqrt(east * east + north * north);
                }
                std::sort(entries.begin(), entries.end(), [](const ViewEntry& left, const ViewEntry& right) { return left.groundUnits < right.groundUnits; });
                std::size_t taken = 0;
                for (const ViewEntry& entry : entries) {
                    if (taken >= limit) break;
                    if (entry.groundUnits < VIEW_NEAREST_UNITS) continue;
                    const float side = std::remainder(std::atan2(entry.feet.x - feet.x, entry.feet.y - feet.y) - heading, 2.0f * std::numbers::pi_v<float>);
                    const float halfAngle = std::atan(entry.halfUnits / entry.groundUnits);
                    const auto firstColumn = static_cast<std::int32_t>(std::floor((side - halfAngle) / columnAngle + halfColumns));
                    const auto lastColumn = static_cast<std::int32_t>(std::floor((side + halfAngle) / columnAngle + halfColumns));
                    const float lowAngle = std::atan2(entry.feet.z - eyes.z, entry.groundUnits);
                    const float highAngle = std::atan2(entry.feet.z + entry.heightUnits - eyes.z, entry.groundUnits);
                    const auto firstRow = static_cast<std::int32_t>(std::floor((topAngle - highAngle) / rowAngle));
                    const auto lastRow = static_cast<std::int32_t>(std::floor((topAngle - lowAngle) / rowAngle));
                    if (lastColumn < 0 || firstColumn >= columns || lastRow < 0 || firstRow >= rows) continue;
                    const bool isThing = entry.kind == VIEW_IS_THING;
                    if (isThing && !IsViewPointSeen(physics, scale, eyes, RE::NiPoint3(entry.feet.x, entry.feet.y, entry.feet.z + entry.aimUnits), entry.nearUnits)) {
                        continue;
                    }
                    std::int32_t legendNumber = 0;
                    for (std::int32_t row = std::max(firstRow, 0); row <= std::min(lastRow, rows - 1); ++row) {
                        if (!isThing) {
                            // The height of the body in this row of the picture
                            const float rowPitch = topAngle - (static_cast<float>(row) + 0.5f) * rowAngle;
                            const float bodyHeight = std::clamp(eyes.z + std::tan(rowPitch) * entry.groundUnits, entry.feet.z + VIEW_BODY_EDGE_UNITS,
                                                                entry.feet.z + entry.heightUnits - VIEW_BODY_EDGE_UNITS);
                            if (!IsViewPointSeen(physics, scale, eyes, RE::NiPoint3(entry.feet.x, entry.feet.y, bodyHeight), entry.nearUnits)) continue;
                        }
                        if (legendNumber == 0) {
                            legend.push_back(entry);
                            ++taken;
                            legendNumber = static_cast<std::int32_t>(legend.size());
                        }
                        for (std::int32_t column = std::max(firstColumn, 0); column <= std::min(lastColumn, columns - 1); ++column) {
                            AddToViewCell(cells[static_cast<std::size_t>(row) * static_cast<std::size_t>(columns) + static_cast<std::size_t>(column)], legendNumber,
                                          entry.groundUnits, entry.groundUnits + (isThing ? VIEW_THING_ORDER_UNITS : 0.0f));
                        }
                    }
                }
            };
            place(creatures, VIEW_MAX_CREATURES);
            place(things, VIEW_MAX_THINGS);
        }

        std::vector<std::string> names;
        numbers = {VIEW_FORMAT, columns, rows, degrees, ToWholeMeters(rangeUnits), static_cast<std::int32_t>(legend.size()), VIEW_VALUES_PER_LEGEND, VIEW_VALUES_PER_CELL};
        for (const ViewEntry& entry : legend) {
            numbers.push_back(entry.kind);
            numbers.push_back(static_cast<std::int32_t>((entry.referenceID >> VIEW_HALF_BITS) & VIEW_HALF_MASK));
            numbers.push_back(static_cast<std::int32_t>(entry.referenceID & VIEW_HALF_MASK));
            numbers.push_back(entry.flags);
            names.push_back(!entry.name.empty() && IsValidUtf(entry.name) ? entry.name : std::string(VIEW_NO_NAME));
        }
        for (const ViewCell& viewCell : cells) {
            numbers.push_back(viewCell.terrain);
            numbers.push_back(viewCell.terrainMeters);
            numbers.push_back(viewCell.isDrop);
            for (std::size_t place = 0; place < VIEW_ENTRIES_PER_CELL; ++place) {
                numbers.push_back(viewCell.entries[place]);
                numbers.push_back(viewCell.entries[place] != 0 ? ToWholeMeters(viewCell.entryUnits[place]) : 0);
            }
        }
        {
            const std::lock_guard<std::mutex> guard(viewLock);
            viewNames = std::move(names);
        }
        return numbers;
    }

    // The names that belong to the legend GetView returned last, in its order
    std::vector<std::string> GetViewNames(RE::StaticFunctionTag*) {
        const std::lock_guard<std::mutex> guard(viewLock);
        return viewNames;
    }

    // ------------------------------------------------------------------ sight
    // How far a character sees: outdoors by the weather and the time of day, indoors by the light where it stands.
    // The meters are what the player of the mod chose after looking at pictures of the weathers of the game.
    // A torch does not make the sight longer than this, it only keeps it from getting shorter than SIGHT_TORCH_METERS

    constexpr float SIGHT_CLEAR_METERS = 200.0f;
    constexpr float SIGHT_RAIN_METERS = 100.0f;
    constexpr float SIGHT_SNOW_METERS = 100.0f;
    constexpr float SIGHT_FOG_METERS = 50.0f;
    constexpr float SIGHT_SNOWSTORM_METERS = 50.0f;
    constexpr float SIGHT_DENSE_FOG_METERS = 25.0f;
    // The time of day as a part of that, in percent: all of it by day, down to half through the evening, down to a
    // quarter until one o'clock, a quarter until four, and back up to all of it through the morning
    constexpr float SIGHT_DAY_START_HOUR = 7.0f;
    constexpr float SIGHT_DAY_END_HOUR = 18.0f;
    constexpr float SIGHT_EVENING_END_HOUR = 21.0f;
    constexpr float SIGHT_DEEP_NIGHT_START_HOUR = 1.0f;
    constexpr float SIGHT_DEEP_NIGHT_END_HOUR = 4.0f;
    constexpr float SIGHT_FULL_PERCENT = 100.0f;
    constexpr float SIGHT_EVENING_PERCENT = 50.0f;
    constexpr float SIGHT_NIGHT_PERCENT = 25.0f;
    constexpr float HOURS_PER_DAY = 24.0f;
    // Indoors a place is bright unless the game keeps it as a dungeon (caves, crypts, ruins, mines: their location, or
    // one it lies in, carries this keyword). The light level the game tells a script (Actor.GetLightLevel) is not
    // used: in a test it was 0 for the companion in an inn lit by a fire and candles. In a dark place a torch in
    // the hand of the companion, or of the player beside it, lets it see further
    constexpr std::string_view SIGHT_DARK_PLACE_KEYWORD = "LocTypeDungeon";
    constexpr std::int32_t SIGHT_MAX_PARENT_LOCATIONS = 6;
    constexpr float SIGHT_BRIGHT_METERS = 50.0f;
    constexpr float SIGHT_DARK_METERS = 15.0f;
    constexpr float SIGHT_DARK_TORCH_METERS = 25.0f;
    constexpr std::int32_t SIGHT_LIGHT_NOT_USED = 0;
    constexpr std::int32_t SIGHT_LIGHT_BRIGHT = 1;
    constexpr std::int32_t SIGHT_LIGHT_DIM = 2;
    constexpr std::int32_t SIGHT_LIGHT_DARK = 3;
    // A torch in the hand of the viewer, or of the player if the player is this close to the viewer
    constexpr float SIGHT_TORCH_METERS = 20.0f;
    constexpr float SIGHT_TORCH_NEAR_UNITS = 15.0f * GAME_UNITS_PER_METER;

    // Which part of the sight is left at this time of day, in percent (see SIGHT_DAY_START_HOUR and the lines after it)
    float GetSightTimePercent(std::int32_t minutesOfDay) {
        const float hour = static_cast<float>(minutesOfDay) / MINUTES_PER_HOUR;
        if (hour >= SIGHT_DAY_START_HOUR && hour < SIGHT_DAY_END_HOUR) return SIGHT_FULL_PERCENT;
        if (hour >= SIGHT_DAY_END_HOUR && hour < SIGHT_EVENING_END_HOUR) {
            return SIGHT_FULL_PERCENT - (hour - SIGHT_DAY_END_HOUR) / (SIGHT_EVENING_END_HOUR - SIGHT_DAY_END_HOUR) * (SIGHT_FULL_PERCENT - SIGHT_EVENING_PERCENT);
        }
        if (hour >= SIGHT_DEEP_NIGHT_START_HOUR && hour < SIGHT_DEEP_NIGHT_END_HOUR) return SIGHT_NIGHT_PERCENT;
        if (hour >= SIGHT_DEEP_NIGHT_END_HOUR && hour < SIGHT_DAY_START_HOUR) {
            return SIGHT_NIGHT_PERCENT + (hour - SIGHT_DEEP_NIGHT_END_HOUR) / (SIGHT_DAY_START_HOUR - SIGHT_DEEP_NIGHT_END_HOUR) * (SIGHT_FULL_PERCENT - SIGHT_NIGHT_PERCENT);
        }
        // From the end of the evening over midnight to the deep of the night
        const float sinceEvening = hour >= SIGHT_EVENING_END_HOUR ? hour - SIGHT_EVENING_END_HOUR : hour + HOURS_PER_DAY - SIGHT_EVENING_END_HOUR;
        const float untilDeepNight = SIGHT_DEEP_NIGHT_START_HOUR + HOURS_PER_DAY - SIGHT_EVENING_END_HOUR;
        return SIGHT_EVENING_PERCENT - sinceEvening / untilDeepNight * (SIGHT_EVENING_PERCENT - SIGHT_NIGHT_PERCENT);
    }

    // Whether the place a character is in counts as dark (see SIGHT_DARK_PLACE_KEYWORD)
    bool IsInDarkPlace(const RE::Actor* viewer) {
        const RE::BGSLocation* location = viewer->GetCurrentLocation();
        for (std::int32_t step = 0; location && step < SIGHT_MAX_PARENT_LOCATIONS; ++step) {
            if (location->HasKeywordString(SIGHT_DARK_PLACE_KEYWORD)) return true;
            location = location->parentLoc;
        }
        return false;
    }

    bool HoldsTorch(RE::Actor* holder) {
        const RE::TESForm* rightHand = holder->GetEquippedObject(false);
        const RE::TESForm* leftHand = holder->GetEquippedObject(true);
        return (leftHand && leftHand->Is(RE::FormType::Light)) || (rightHand && rightHand->Is(RE::FormType::Light));
    }

    // Works out how far the viewer sees right now and keeps it for what looks next (see GetSightUnitsOf).
    // lightLevel: what Actor.GetLightLevel told the script for the viewer, below 0 if it could not tell.
    // Returns: the meters, the weather (WEATHER_...), the part the time of day leaves in percent, indoors or not,
    // the light (SIGHT_LIGHT_...), whether a torch keeps the sight from being shorter, the light level, and the
    // time of day in minutes since midnight
    std::vector<std::int32_t> UpdateSight(RE::StaticFunctionTag*, RE::Actor* viewer, std::int32_t lightLevel) {
        std::vector<std::int32_t> numbers;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!viewer || !player) return numbers;
        const auto* cell = viewer->GetParentCell();
        const bool isIndoors = cell && cell->IsInteriorCell();
        const std::int32_t minutesOfDay = GetMinutesOfDay();
        std::int32_t weather = WEATHER_NOT_KNOWN;
        float timePercent = SIGHT_FULL_PERCENT;
        std::int32_t light = SIGHT_LIGHT_NOT_USED;
        float meters = SIGHT_CLEAR_METERS;
        const bool hasTorch = HoldsTorch(viewer) ||
                              (viewer != player && viewer->GetPosition().GetDistance(player->GetPosition()) <= SIGHT_TORCH_NEAR_UNITS && HoldsTorch(player));
        bool isTorchUsed = false;
        if (isIndoors) {
            if (IsInDarkPlace(viewer)) {
                light = SIGHT_LIGHT_DARK;
                meters = hasTorch ? SIGHT_DARK_TORCH_METERS : SIGHT_DARK_METERS;
                isTorchUsed = hasTorch;
            } else {
                light = SIGHT_LIGHT_BRIGHT;
                meters = SIGHT_BRIGHT_METERS;
            }
        } else {
            weather = GetWeatherClass();
            switch (weather) {
                case WEATHER_RAINY:
                    meters = SIGHT_RAIN_METERS;
                    break;
                case WEATHER_SNOW:
                    meters = SIGHT_SNOW_METERS;
                    break;
                case WEATHER_FOG:
                    meters = SIGHT_FOG_METERS;
                    break;
                case WEATHER_SNOWSTORM:
                    meters = SIGHT_SNOWSTORM_METERS;
                    break;
                case WEATHER_DENSE_FOG:
                    meters = SIGHT_DENSE_FOG_METERS;
                    break;
                default:
                    meters = SIGHT_CLEAR_METERS;
                    break;
            }
            timePercent = GetSightTimePercent(minutesOfDay);
            meters = meters * timePercent / SIGHT_FULL_PERCENT;
        }
        if (hasTorch && meters < SIGHT_TORCH_METERS) {
            isTorchUsed = true;
            meters = SIGHT_TORCH_METERS;
        }
        meters = std::max(meters, 1.0f);
        {
            const std::lock_guard<std::mutex> guard(sightLock);
            keptSightViewerID = viewer->GetFormID();
            keptSightUnits = meters * GAME_UNITS_PER_METER;
            keptSightTime = std::chrono::steady_clock::now();
        }
        numbers = {static_cast<std::int32_t>(std::lround(meters)), weather, static_cast<std::int32_t>(std::lround(timePercent)), isIndoors ? 1 : 0, light,
                   isTorchUsed ? 1 : 0, lightLevel, minutesOfDay};
        return numbers;
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
        vm->RegisterFunction("SetMutedSpeaker", SCRIPT_NAME, SetMutedSpeaker);
        vm->RegisterFunction("GetSurroundingsToward", SCRIPT_NAME, GetSurroundingsToward);
        vm->RegisterFunction("GetGroundHeight", SCRIPT_NAME, GetGroundHeight);
        vm->RegisterFunction("GetMount", SCRIPT_NAME, GetMount);
        vm->RegisterFunction("GetVisiblePlaces", SCRIPT_NAME, GetVisiblePlaces);
        vm->RegisterFunction("GetLootCount", SCRIPT_NAME, GetLootCount);
        vm->RegisterFunction("TickEvents", SCRIPT_NAME, TickEvents);
        vm->RegisterFunction("TakeEvents", SCRIPT_NAME, TakeEvents);
        vm->RegisterFunction("TakeUrgentEvents", SCRIPT_NAME, TakeUrgentEvents);
        vm->RegisterFunction("SkipTransferEvents", SCRIPT_NAME, SkipTransferEvents);
        vm->RegisterFunction("GetBookText", SCRIPT_NAME, GetBookText);
        vm->RegisterFunction("SetForceSneak", SCRIPT_NAME, SetForceSneak);
        vm->RegisterFunction("StartHandCast", SCRIPT_NAME, StartHandCast);
        vm->RegisterFunction("EndHandCast", SCRIPT_NAME, EndHandCast);
        vm->RegisterFunction("GetHandCastState", SCRIPT_NAME, GetHandCastState);
        vm->RegisterFunction("StartAttack", SCRIPT_NAME, StartAttack);
        vm->RegisterFunction("EndAttack", SCRIPT_NAME, EndAttack);
        vm->RegisterFunction("GetAttackActionResult", SCRIPT_NAME, GetAttackActionResult);
        vm->RegisterFunction("GetAttackState", SCRIPT_NAME, GetAttackState);
        vm->RegisterFunction("GetInventoryLines", SCRIPT_NAME, GetInventoryLines);
        vm->RegisterFunction("SetTalkTarget", SCRIPT_NAME, SetTalkTarget);
        vm->RegisterFunction("GetHealSpells", SCRIPT_NAME, GetHealSpells);
        vm->RegisterFunction("GetMerchantChest", SCRIPT_NAME, GetMerchantChest);
        vm->RegisterFunction("GetMerchantLines", SCRIPT_NAME, GetMerchantLines);
        vm->RegisterFunction("GetTradeOffer", SCRIPT_NAME, GetTradeOffer);
        vm->RegisterFunction("GetActorNumbers", SCRIPT_NAME, GetActorNumbers);
        vm->RegisterFunction("GetNearbyValues", SCRIPT_NAME, GetNearbyValues);
        vm->RegisterFunction("GetNearbyNames", SCRIPT_NAME, GetNearbyNames);
        vm->RegisterFunction("GetAbsenceReason", SCRIPT_NAME, GetAbsenceReason);
        vm->RegisterFunction("TakeMenuLines", SCRIPT_NAME, TakeMenuLines);
        vm->RegisterFunction("GetHandNames", SCRIPT_NAME, GetHandNames);
        vm->RegisterFunction("FindLooseItems", SCRIPT_NAME, FindLooseItems);
        vm->RegisterFunction("GetLooseItemNumbers", SCRIPT_NAME, GetLooseItemNumbers);
        vm->RegisterFunction("GetWholeState", SCRIPT_NAME, GetWholeState);
        vm->RegisterFunction("GetWholeStateTexts", SCRIPT_NAME, GetWholeStateTexts);
        vm->RegisterFunction("RequestCover", SCRIPT_NAME, RequestCover);
        vm->RegisterFunction("TakeCoverResult", SCRIPT_NAME, TakeCoverResult);
        vm->RegisterFunction("GetCoverThreat", SCRIPT_NAME, GetCoverThreat);
        vm->RegisterFunction("GetView", SCRIPT_NAME, GetView);
        vm->RegisterFunction("GetViewNames", SCRIPT_NAME, GetViewNames);
        vm->RegisterFunction("UpdateSight", SCRIPT_NAME, UpdateSight);
        vm->RegisterFunction("TakeHeights", SCRIPT_NAME, TakeHeights);
        vm->RegisterFunction("IsBed", SCRIPT_NAME, IsBed);
        vm->RegisterFunction("GetAmmoKind", SCRIPT_NAME, GetAmmoKind);
        vm->RegisterFunction("TickWatch", SCRIPT_NAME, TickWatch);
        vm->RegisterFunction("SetWatchedHorses", SCRIPT_NAME, SetWatchedHorses);
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
