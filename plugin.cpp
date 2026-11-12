// Plugin of the AI companion mod for Skyrim (KI_MCP_Gefaehrte.esp).
// It reads what Papyrus scripts cannot get at and hands it to the script KI_MCP_Gefaehrte_Native.
// So far: the open quests of the player with the texts of their objectives,
// and what the characters around the player say (the subtitles of the game).
//
// The project layout and the places of the subtitle hook come from mikastamm/mantella-vanilla-dialogue (MIT license),
// the build setup from Racvb0.
// The quests are only read. For the subtitles the plugin hooks into two calls of the game (see InstallSubtitleHook).

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
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "PCH.h"
#include "logger.h"

namespace Companion {

    // Goes up whenever the functions for the script change. The script passes it on to the companion server
    constexpr std::int32_t INTERFACE_VERSION = 2;
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

    bool Bind(RE::BSScript::IVirtualMachine* vm) {
        vm->RegisterFunction("GetInterfaceVersion", SCRIPT_NAME, GetInterfaceVersion);
        vm->RegisterFunction("GetOpenQuests", SCRIPT_NAME, GetOpenQuests);
        vm->RegisterFunction("IsHearing", SCRIPT_NAME, IsHearing);
        vm->RegisterFunction("TakeHeardLines", SCRIPT_NAME, TakeHeardLines);
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
    if (const auto* messaging = SKSE::GetMessagingInterface()) {
        messaging->RegisterListener("SKSE", Companion::OnMessage);
    }
    return true;
}
