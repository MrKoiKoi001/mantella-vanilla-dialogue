// Plugin of the AI companion mod for Skyrim (KI_MCP_Gefaehrte.esp).
// It reads what Papyrus scripts cannot get at and hands it to the script KI_MCP_Gefaehrte_Native.
// So far: the open quests of the player with the texts of their objectives and where their targets are,
// the places around the player (map markers and doors that lead somewhere),
// the things close to the player (furniture, containers and items that lie around),
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
    constexpr std::int32_t INTERFACE_VERSION = 4;
    constexpr std::string_view SCRIPT_NAME = "KI_MCP_Gefaehrte_Native";
    // Stands between the parts of one quest line. CleanText makes sure that no text of the game contains it
    constexpr char FIELD_SEPARATOR = '\t';
    constexpr std::size_t MAX_OBJECTIVES_PER_QUEST = 6;
    // In bytes. Longer texts are cut off
    constexpr std::size_t MAX_TEXT_LENGTH = 200;
    constexpr std::size_t DEFAULT_MAX_QUESTS = 25;
    constexpr std::string_view CUT_OFF_MARK = "...";

    // What was heard and not fetched by the script yet. Older lines make room for newer ones
    constexpr std::size_t MAX_STORED_HEARD_LINES = 40;
    constexpr std::size_t DEFAULT_MAX_HEARD_LINES = 20;
    constexpr std::size_t MAX_HEARD_TEXT_LENGTH = 300;
    // Lines older than this are not handed out any more
    constexpr std::int64_t MAX_HEARD_AGE_SECONDS = 600;
    // The same line of the same speaker within this time counts as one
    constexpr std::int64_t SAME_LINE_SECONDS = 20;

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
    // A call to a fixed address is the byte E8 followed by the distance to the target as four bytes
    constexpr std::uint8_t DIRECT_CALL_BYTE = 0xE8;
    constexpr std::size_t DIRECT_CALL_LENGTH = 5;
    // Each redirected call needs 14 bytes of the trampoline
    constexpr std::size_t TRAMPOLINE_BYTES = 64;
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
    // For furniture: a character can sit on it. For a container: it is locked
    constexpr std::int32_t FURNITURE_IS_SEAT = 1;
    constexpr std::int32_t CONTAINER_IS_LOCKED = 1;
    constexpr std::int32_t DEFAULT_THING_RADIUS_METERS = 10;
    constexpr std::size_t DEFAULT_MAX_THINGS_PER_KIND = 4;

    // Functions of the game this plugin calls through the Address Library, by their IDs for Skyrim SE (which Skyrim VR shares).
    // The Address Library for Skyrim VR is a file that grows from release to release, and an ID it does not have
    // ends the game with an error. So the plugin looks into the file first (see HasGameAddress)
    constexpr std::uint64_t CONDITION_IS_TRUE_ID = 29074;
    constexpr std::uint64_t WORLD_SINGLETON_ID = 516923;
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
    // Where something is, seen from the player: how far away, and at which angle from where the player faces.
    // The companion server works out the same for the companion (src/companion/game_state.py in the Mantella fork)

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
        // Negative = to the left of where the player faces, positive = to the right
        std::int32_t angleDegrees = 0;
    };

    View GetViewFromPlayer(const RE::PlayerCharacter* player, const RE::NiPoint3& point) {
        const auto own = player->GetPosition();
        const float east = point.x - own.x;
        const float north = point.y - own.y;
        const float up = point.z - own.z;
        // The game counts directions clockwise from north, like a compass
        const float direction = std::atan2(east, north) * DEGREES_PER_RADIAN;
        const float facing = player->GetAngleZ() * DEGREES_PER_RADIAN;
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
        View view;
        std::string name;
        std::string place;
    };

    // The first door on the way to the target that is in the area of the player, or nullptr
    const RE::TESObjectREFR* FindDoorOnTheWay(const RE::TESQuestTarget* target, const RE::TESObjectCELL* playerCell) {
        const auto& doors = target->teleportPath.teleportRefs;
        if (doors.empty() || doors.size() > MAX_DOORS_ON_THE_WAY || doors.size() > doors.capacity()) return nullptr;
        if (!IsReadableMemory(doors.data(), doors.size() * sizeof(RE::TeleportPath::TeleportLink))) return nullptr;
        for (const auto& door : doors) {
            if (!IsKnownForm(door.ref)) continue;
            if (IsSameSpace(playerCell, GetCellOf(door.ref))) return door.ref;
        }
        return nullptr;
    }

    // A door in the loaded area around the player that leads straight into this cell, the nearest one, or nullptr.
    // For targets inside a building of the town the player stands in, if the game has no way to the target at hand
    const RE::TESObjectREFR* FindLoadedDoorInto(const RE::TESObjectCELL* targetCell, RE::PlayerCharacter* player) {
        if (!targetCell || !targetCell->IsInteriorCell() || !HasGameAddress(WORLD_SINGLETON_ID)) return nullptr;
        auto* world = RE::TES::GetSingleton();
        if (!world) return nullptr;
        const auto own = player->GetPosition();
        const RE::TESObjectREFR* nearestDoor = nullptr;
        float nearestDistance = 0.0f;

        world->ForEachReferenceInRange(player, static_cast<float>(DEFAULT_PLACE_RADIUS_METERS) * GAME_UNITS_PER_METER, [&](RE::TESObjectREFR* reference) {
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
                                            bool canCheckConditions) {
        std::vector<QuestTarget> targets;
        if (!objective->targets) return targets;
        const auto* playerCell = GetCellOf(player);
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
            if (IsSameSpace(playerCell, targetCell)) {
                found.kind = TARGET_IS_HERE;
                found.view = GetViewFromPlayer(player, reference->GetPosition());
            } else {
                // First the way the game itself has worked out, then a door that leads straight to where the target is
                const auto* door = FindDoorOnTheWay(target, playerCell);
                if (!door) door = FindLoadedDoorInto(targetCell, player);
                if (door) {
                    found.kind = TARGET_IS_BEHIND_DOOR;
                    found.view = GetViewFromPlayer(player, door->GetPosition());
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

    std::vector<Place> CollectMapMarkers(RE::PlayerCharacter* player, float radius, std::size_t limit) {
        std::vector<Place> places;
        const auto* playerCell = GetCellOf(player);
        const auto* markers = GetMapMarkers(player);
        // Indoors the map of the world outside says nothing about what is near
        if (!markers || !playerCell || playerCell->IsInteriorCell()) return places;
        const auto own = player->GetPosition();

        for (const auto& handle : *markers) {
            const auto marker = handle.get();
            if (!marker || marker->IsDisabled()) continue;
            const float distance = own.GetDistance(marker->GetPosition());
            if (distance > radius) continue;
            const auto* extra = marker->extraList.GetByType<RE::ExtraMapMarker>();
            const auto* data = extra ? extra->mapData : nullptr;
            if (!data || !IsSameSpace(playerCell, GetCellOf(marker.get()))) continue;

            Place place;
            place.kind = PLACE_IS_MAP_MARKER;
            place.name = CleanText(ViewOf(data->locationName.GetFullName()));
            if (place.name.empty()) continue;
            place.distance = distance;
            place.view = GetViewFromPlayer(player, marker->GetPosition());
            place.type = static_cast<std::int32_t>(data->type.underlying());
            place.flags = data->flags.any(RE::MapMarkerData::Flag::kCanTravelTo) ? MARKER_IS_KNOWN : 0;
            places.push_back(std::move(place));
        }
        KeepNearest(places, limit);
        return places;
    }

    // The doors in the loaded area around the player that lead into another area (a building, a cave, out of town)
    std::vector<Place> CollectDoors(RE::PlayerCharacter* player, float radius, std::size_t limit) {
        std::vector<Place> places;
        if (!HasGameAddress(WORLD_SINGLETON_ID)) return places;
        auto* world = RE::TES::GetSingleton();
        if (!world) return places;
        const auto own = player->GetPosition();

        world->ForEachReferenceInRange(player, radius, [&](RE::TESObjectREFR* reference) {
            if (!reference || reference->IsDisabled()) return RE::BSContainer::ForEachResult::kContinue;
            const auto* base = reference->GetBaseObject();
            if (!base || !base->Is(RE::FormType::Door)) return RE::BSContainer::ForEachResult::kContinue;
            std::string name = GetNameBehindDoor(reference);
            if (name.empty()) return RE::BSContainer::ForEachResult::kContinue;

            Place place;
            place.kind = PLACE_IS_DOOR;
            place.name = std::move(name);
            place.distance = own.GetDistance(reference->GetPosition());
            place.view = GetViewFromPlayer(player, reference->GetPosition());
            places.push_back(std::move(place));
            return RE::BSContainer::ForEachResult::kContinue;
        });
        KeepNearest(places, limit);
        return places;
    }

    // ----------------------------------------------------------------- things
    // What stands and lies around the player: furniture (chairs, benches, beds, workbenches), containers (chests, barrels,
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

    // The things within the radius around the player: of every kind the nearest ones, at most limitPerKind,
    // furniture first, then containers, then items. Things without a name are left out (the game has many
    // invisible pieces of furniture that only tell characters where to lean or stand)
    std::vector<Thing> CollectThings(RE::PlayerCharacter* player, float radius, std::size_t limitPerKind) {
        std::array<std::vector<Thing>, THING_KIND_COUNT> thingsByKind;
        if (!HasGameAddress(WORLD_SINGLETON_ID)) return {};
        auto* world = RE::TES::GetSingleton();
        if (!world) return {};
        const auto own = player->GetPosition();

        world->ForEachReferenceInRange(player, radius, [&](RE::TESObjectREFR* reference) {
            if (!reference || reference == player || reference->IsDisabled() || reference->IsMarkedForDeletion()) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            const auto* base = reference->GetBaseObject();
            const std::int32_t kind = base ? GetThingKind(base) : -1;
            if (kind < 0) return RE::BSContainer::ForEachResult::kContinue;
            std::string name = CleanText(ViewOf(reference->GetDisplayFullName()));
            if (name.empty()) return RE::BSContainer::ForEachResult::kContinue;

            Thing thing;
            thing.kind = kind;
            thing.referenceID = reference->GetFormID();
            thing.name = std::move(name);
            thing.distance = own.GetDistance(reference->GetPosition());
            thing.view = GetViewFromPlayer(player, reference->GetPosition());
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

    // ---------------------------------------------------------------- hearing
    // The game calls SubtitleManager::ShowSubtitle for every line a character near the player speaks.
    // The plugin redirects the two calls through its own function, which passes the call on and keeps the line.
    // The script fetches the kept lines when the companion server asks for the state.

    struct HeardLine {
        RE::FormID speakerID = 0;
        std::string speakerName;
        std::string text;
        std::chrono::steady_clock::time_point time;
    };

    std::mutex heardLock;
    std::deque<HeardLine> heardLines;
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
        for (const auto& known : heardLines) {
            if (known.speakerID == speakerID && known.text == text && SecondsBetween(known.time, now) < SAME_LINE_SECONDS) return;
        }
        HeardLine line;
        line.speakerID = speakerID;
        line.speakerName = CleanText(speakerName);
        line.text = std::move(text);
        line.time = now;
        heardLines.push_back(std::move(line));
        while (heardLines.size() > MAX_STORED_HEARD_LINES) heardLines.pop_front();
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
            if (!speaker || !subtitle || subtitle[0] == '\0') return;
            KeepPlayerChoice();
            KeepHeardLine(speaker->GetFormID(), ViewOf(speaker->GetDisplayFullName()), subtitle);
        } catch (...) {
            // Whatever goes wrong here must not reach the game
        }
    }

    void ForgetHeardLines() {
        const std::lock_guard<std::mutex> guard(heardLock);
        heardLines.clear();
        lastChoice = nullptr;
        lastChoiceText.clear();
    }

    struct SubtitleHook {
        static void Thunk(RE::SubtitleManager* manager, RE::TESObjectREFR* speaker, const char* subtitle, bool alwaysDisplay) {
            original(manager, speaker, subtitle, alwaysDisplay);
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

    // Redirects the two calls of ShowSubtitle through SubtitleHook::Thunk.
    // The places inside the two functions are known for Skyrim SE and AE. Before anything is changed, both places
    // are checked: each has to be a call that goes to ShowSubtitle. If not (another version of the game,
    // or another mod was there first), nothing is changed and the plugin only does without hearing
    bool InstallSubtitleHook() {
        if (IsHearingSwitchedOff()) {
            logger::info("Hearing is switched off in {}.", SETTINGS_FILE);
            return false;
        }
        const std::ptrdiff_t actorOffset = REL::Module::IsAE() ? ACTOR_SOUND_CALL_OFFSET_AE : ACTOR_SOUND_CALL_OFFSET_SE;
        REL::Relocation<std::uintptr_t> referenceSite(RELOCATION_ID(REFERENCE_SOUND_ID_SE, REFERENCE_SOUND_ID_AE), REFERENCE_SOUND_CALL_OFFSET);
        REL::Relocation<std::uintptr_t> actorSite(RELOCATION_ID(ACTOR_SOUND_ID_SE, ACTOR_SOUND_ID_AE), actorOffset);
        REL::Relocation<std::uintptr_t> showSubtitle(RELOCATION_ID(SHOW_SUBTITLE_ID_SE, SHOW_SUBTITLE_ID_AE));
        const std::array<std::uintptr_t, 2> sites{ referenceSite.address(), actorSite.address() };

        for (const auto site : sites) {
            const auto target = GetDirectCallTarget(site);
            if (target != showSubtitle.address()) {
                logger::error("No hearing: the call at {:X} goes to {:X} instead of ShowSubtitle at {:X}. Nothing was changed.", site,
                              target, showSubtitle.address());
                return false;
            }
        }

        auto& trampoline = SKSE::GetTrampoline();
        // Older versions of SKSE (as the one for Skyrim VR) hand out no memory for this, so the plugin gets its own
        if (trampoline.empty()) trampoline.create(TRAMPOLINE_BYTES);
        for (const auto site : sites) {
            SubtitleHook::original = trampoline.write_call<DIRECT_CALL_LENGTH>(site, SubtitleHook::Thunk);
        }
        logger::info("Hearing: hooked into the two calls of ShowSubtitle.");
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

    // Whether the plugin can ask the game about the conditions of quest targets. Without that, GetQuestTargets
    // hands out every target of an objective, also those the game itself would not show at the moment
    bool canCheckTargetConditions = false;

    // Where the targets of the tracked quests are, at most maxTargets. One text per target, its parts separated by tabs:
    // place of the quest in the list GetOpenQuests(maxQuests) returns (from 0), place of the objective in that quest (from 0),
    // where the target is (0 = in the area of the player, 1 = behind a door of this area, 2 = somewhere else),
    // distance to the player in meters and angle from where the player faces in degrees (negative = left) - for 1 these are
    // the distance and angle of that door, for 2 both are 0 -, name of the target (can be empty),
    // and for 1 and 2 the name of the place behind the door or of the place the target is in (can be empty)
    std::vector<std::string> GetQuestTargets(RE::StaticFunctionTag*, std::int32_t maxQuests, std::int32_t maxTargets) {
        std::vector<std::string> lines;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return lines;
        const auto quests = CollectOpenQuests();
        const std::size_t questLimit = maxQuests > 0 ? static_cast<std::size_t>(maxQuests) : DEFAULT_MAX_QUESTS;
        const std::size_t targetLimit = maxTargets > 0 ? static_cast<std::size_t>(maxTargets) : DEFAULT_MAX_QUEST_TARGETS;

        for (std::size_t questIndex = 0; questIndex < quests.size() && questIndex < questLimit; ++questIndex) {
            const auto& entry = quests[questIndex];
            // The compass of the game only shows the targets of tracked quests, and so does this list
            if (!entry.quest->IsActive()) continue;
            for (std::size_t objectiveIndex = 0; objectiveIndex < entry.objectiveForms.size(); ++objectiveIndex) {
                const auto targets = CollectTargets(entry.quest, entry.objectiveForms[objectiveIndex], player, canCheckTargetConditions);
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
                    lines.push_back(std::move(line));
                }
            }
        }
        logger::debug("GetQuestTargets: {} targets sent", lines.size());
        return lines;
    }

    // The places around the player, map markers first, then doors, each nearest first:
    // at most maxMarkers map markers within radiusMeters (none indoors), and at most maxDoors doors in the loaded area
    // within radiusMeters that lead into another area. One text per place, its parts separated by tabs:
    // kind (0 = map marker, 1 = door), distance to the player in meters, angle from where the player faces in degrees
    // (negative = left), for a map marker the kind of place as the game numbers it (13 = farm, 21 = stable ...; 0 for a door),
    // for a map marker 1 if the player has been there (0 otherwise, and for a door),
    // name of the place, for a door the name of the place it leads to
    std::vector<std::string> GetNearbyPlaces(RE::StaticFunctionTag*, std::int32_t radiusMeters, std::int32_t maxMarkers, std::int32_t maxDoors) {
        std::vector<std::string> lines;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return lines;
        const float radius = static_cast<float>(radiusMeters > 0 ? radiusMeters : DEFAULT_PLACE_RADIUS_METERS) * GAME_UNITS_PER_METER;
        // A limit of 0 switches that kind of place off, a negative one means: the usual number
        const std::size_t markerLimit = maxMarkers >= 0 ? static_cast<std::size_t>(maxMarkers) : DEFAULT_MAX_MAP_MARKERS;
        const std::size_t doorLimit = maxDoors >= 0 ? static_cast<std::size_t>(maxDoors) : DEFAULT_MAX_DOORS;

        std::vector<Place> places;
        if (markerLimit > 0) places = CollectMapMarkers(player, radius, markerLimit);
        if (doorLimit > 0) {
            auto doors = CollectDoors(player, radius, doorLimit);
            for (auto& door : doors) places.push_back(std::move(door));
        }
        for (const auto& place : places) {
            std::string line = std::to_string(place.kind);
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

    // The things close to the player: furniture first, then containers, then items that lie around, each nearest first,
    // at most maxPerKind of each kind within radiusMeters. One text per thing, its parts separated by tabs:
    // kind (0 = furniture, 1 = container, 2 = item), ref ID as eight hex digits, distance to the player in meters,
    // angle from where the player faces in degrees (negative = left),
    // for furniture 1 if a character can sit on it, for a container 1 if it is locked (0 otherwise), name
    std::vector<std::string> GetNearbyThings(RE::StaticFunctionTag*, std::int32_t radiusMeters, std::int32_t maxPerKind) {
        std::vector<std::string> lines;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return lines;
        const float radius = static_cast<float>(radiusMeters > 0 ? radiusMeters : DEFAULT_THING_RADIUS_METERS) * GAME_UNITS_PER_METER;
        const std::size_t limit = maxPerKind > 0 ? static_cast<std::size_t>(maxPerKind) : DEFAULT_MAX_THINGS_PER_KIND;

        for (const auto& thing : CollectThings(player, radius, limit)) {
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
            lines.push_back(std::move(line));
        }
        logger::debug("GetNearbyThings: {} things sent", lines.size());
        return lines;
    }

    bool Bind(RE::BSScript::IVirtualMachine* vm) {
        vm->RegisterFunction("GetInterfaceVersion", SCRIPT_NAME, GetInterfaceVersion);
        vm->RegisterFunction("GetOpenQuests", SCRIPT_NAME, GetOpenQuests);
        vm->RegisterFunction("IsHearing", SCRIPT_NAME, IsHearing);
        vm->RegisterFunction("TakeHeardLines", SCRIPT_NAME, TakeHeardLines);
        vm->RegisterFunction("GetQuestTargets", SCRIPT_NAME, GetQuestTargets);
        vm->RegisterFunction("GetNearbyPlaces", SCRIPT_NAME, GetNearbyPlaces);
        vm->RegisterFunction("GetNearbyThings", SCRIPT_NAME, GetNearbyThings);
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
    if (const auto* messaging = SKSE::GetMessagingInterface()) {
        messaging->RegisterListener("SKSE", Companion::OnMessage);
    }
    return true;
}
