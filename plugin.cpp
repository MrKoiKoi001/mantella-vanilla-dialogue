// Plugin of the AI companion mod for Skyrim (KI_MCP_Gefaehrte.esp).
// It reads what Papyrus scripts cannot get at and hands it to the script KI_MCP_Gefaehrte_Native.
// So far: the open quests of the player with the texts of their objectives.
//
// The project layout comes from mikastamm/mantella-vanilla-dialogue (MIT license), the build setup from Racvb0.
// This file only reads data of the game. It changes nothing and hooks into nothing.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "PCH.h"
#include "logger.h"

namespace Companion {

    // Goes up whenever the functions for the script change. The script passes it on to the companion server
    constexpr std::int32_t INTERFACE_VERSION = 1;
    constexpr std::string_view SCRIPT_NAME = "KI_MCP_Gefaehrte_Native";
    // Stands between the parts of one quest line. CleanText makes sure that no text of the game contains it
    constexpr char FIELD_SEPARATOR = '\t';
    constexpr std::size_t MAX_OBJECTIVES_PER_QUEST = 6;
    // In bytes. Longer texts are cut off
    constexpr std::size_t MAX_TEXT_LENGTH = 200;
    constexpr std::size_t DEFAULT_MAX_QUESTS = 25;
    constexpr std::string_view CUT_OFF_MARK = "...";
    constexpr std::string_view UNKNOWN_VALUE = "?";

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

    // Returns the text as valid UTF-8 in one line, without spaces around it and not longer than MAX_TEXT_LENGTH
    std::string CleanText(std::string_view raw) {
        std::string text = IsValidUtf(raw) ? std::string(raw) : FromWindowsText(raw);
        for (char& letter : text) {
            // Tabs, line breaks and the like. The tab is the separator of the quest lines
            if (static_cast<unsigned char>(letter) < 0x20) letter = ' ';
        }
        const auto first = text.find_first_not_of(' ');
        if (first == std::string::npos) return std::string();
        const auto last = text.find_last_not_of(' ');
        text = text.substr(first, last - first + 1);

        if (text.size() > MAX_TEXT_LENGTH) {
            std::size_t cut = MAX_TEXT_LENGTH;
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
            auto& objectives = quests[known->second].objectives;
            if (objectives.size() < MAX_OBJECTIVES_PER_QUEST) objectives.push_back(std::move(text));
        }

        std::stable_partition(quests.begin(), quests.end(), [](const OpenQuest& entry) { return entry.quest->IsActive(); });
        return quests;
    }

    // ----------------------------------------- functions for the Papyrus script

    std::int32_t GetInterfaceVersion(RE::StaticFunctionTag*) { return INTERFACE_VERSION; }

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

    bool Bind(RE::BSScript::IVirtualMachine* vm) {
        vm->RegisterFunction("GetInterfaceVersion", SCRIPT_NAME, GetInterfaceVersion);
        vm->RegisterFunction("GetOpenQuests", SCRIPT_NAME, GetOpenQuests);
        return true;
    }

}  // namespace Companion

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    SetupLog();
    SKSE::GetPapyrusInterface()->Register(Companion::Bind);
    logger::info("Loaded. Functions for the script {} registered, interface version {}.", Companion::SCRIPT_NAME,
                 Companion::INTERFACE_VERSION);
    return true;
}
