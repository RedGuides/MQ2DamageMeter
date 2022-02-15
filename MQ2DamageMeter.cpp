// MQ2DamageMeter.cpp : Defines the entry point for the DLL application.
//

// PLUGIN_API is only to be used for callbacks.  All existing callbacks at this time
// are shown below. Remove the ones your plugin does not use.  Always use Initialize
// and Shutdown for setup and cleanup.

#include "sqlite3.h"

#include <mq/Plugin.h>

PreSetup("MQ2DamageMeter");
PLUGIN_VERSION(0.1);

// TODO: These need to be configurable so when the strings data changes it won't take a recompile (though this is very rare)
// these map to entries in eqstr_us.txt
#define SELF_DOT_STRINGID 9072
#define OTHER_DOT_STRINGID 13327
#define ONME_DOT_STRINGID 12954

namespace mq {
/**
 * plugin-tracked variables
 */

 // categories of characters to track
enum DamageTrackingCategory
{
	DTC_NONE = 0,
	DTC_SELF = 1,
	DTC_GROUP = 2,
	DTC_FELLOWSHIP = 4,
	DTC_GUILD = 8,
	DTC_RAID = 16
};

class HitInstance : public EQSuccessfulHit
{
public:
	const std::string Name;
	const DWORD Timestamp;

	HitInstance(const EQSuccessfulHit& hit, std::string_view Name) : 
		EQSuccessfulHit(hit),
		Name(Name),
		Timestamp(EQGetTime())
	{}
};

class AttackerTracking
{
public:
	const int ID;
	const std::string Name;
	std::multimap<const int, const HitInstance> Hits;

	AttackerTracking(int ID, std::string_view Name) :
		ID(ID), Name(Name), Hits({}) {}

	int GetTotal(int HitID) const
	{
		auto hits = Hits.equal_range(HitID);
		return std::accumulate(hits.first, hits.second, 0,
			[](int total, const std::pair<const int, HitInstance>& hit)
			{
				return total + hit.second.DamageCaused;
			});
	}

	int GetTotal() const
	{
		return std::accumulate(std::cbegin(Hits), std::cend(Hits), 0,
			[](int total, const std::pair<const int, HitInstance>& hit)
			{
				return total + hit.second.DamageCaused;
			});
	}

	bool Compare(const ImGuiTableSortSpecs* sort_specs, const std::unique_ptr<AttackerTracking>& other) const
	{
		for (int n = 0; n < sort_specs->SpecsCount; ++n)
		{
			auto sort_spec = sort_specs->Specs[n];
			switch (sort_spec.ColumnIndex)
			{
			case 0: // Name
				if (sort_spec.SortDirection == ImGuiSortDirection_Ascending)
					return Name < other->Name;

				if (sort_spec.SortDirection == ImGuiSortDirection_Descending)
					return Name > other->Name;

				break;

			case 1: // Damage
				if (sort_spec.SortDirection == ImGuiSortDirection_Ascending)
					return GetTotal() < other->GetTotal();

				if (sort_spec.SortDirection == ImGuiSortDirection_Descending)
					return GetTotal() > other->GetTotal();

				break;

			default:
				break;
			}
		}

		return GetTotal() < other->GetTotal();
	}

	std::vector<AttackerTracking> GetBreakdown()
	{
		return std::accumulate(std::cbegin(Hits), std::cend(Hits), std::vector<AttackerTracking>({}),
			[](std::vector<AttackerTracking>& items, const std::pair<const int, HitInstance>& hit)
			{
				auto last = items.rbegin();
				if (last != items.rend() && last->ID == hit.first)
				{
					// modify last entry, same ID
					last->Hits.emplace(hit);
				}
				else
				{
					// add new entry, ID doesn't exist
					auto& item = items.emplace_back(hit.first, hit.second.Name);
					item.Hits.emplace(hit);
				}

				return items;
			});
	}

	template <int flags>
	bool ShowProgressBar(int total)
	{
		auto hitTotal = GetTotal();
		ImGui::TableNextRow();

		auto open = ImGui::TreeNodeEx(Name.c_str(), flags);
		ImGui::TableNextColumn();
		ImGui::Text(Name.c_str());

		ImGui::TableNextColumn();
		ImGui::ProgressBar((float)hitTotal / total, ImVec2(-FLT_MIN, 0.f), std::to_string(hitTotal).c_str());

		return open;
	}
};

// indexed by attacker ID
std::vector<std::unique_ptr<AttackerTracking>> damage_map;
void AddDamage(const EQSuccessfulHit& hit)
{
	auto attackerID = hit.AttackerID;
	auto attackerSpawn = GetSpawnByID(attackerID);
	if (attackerSpawn && attackerSpawn->MasterID > 0)
	{
		attackerID = attackerSpawn->MasterID;
		attackerSpawn = GetSpawnByID(attackerID);
		// TODO: when tracking skill, set this to the pet ID (or something similar? need a way to resolve the pet's name in the skill display)
		// All the information we need is actually stored in the hit, so just need to track a special skill/spellID
	}

	auto damage = std::find_if(damage_map.begin(), damage_map.end(),
		[&attackerID](const std::unique_ptr<AttackerTracking>& damage_item)
		{
			return damage_item->ID == attackerID;
		});

	if (damage == damage_map.end())
	{
		damage = damage_map.emplace(
			damage,
			std::make_unique<AttackerTracking>(hit.AttackerID, attackerSpawn != nullptr ? attackerSpawn->Name : "UNKNOWN"));
	}

	// TODO: can the name and spawn change per ID? unlikely, but make sure we clear the list on zone to be safe (and perhaps log it?)

	auto damagedSpawn = GetSpawnByID(hit.DamagedID);
	(*damage)->Hits.emplace(hit.DamagedID, HitInstance(hit, damagedSpawn != nullptr ? damagedSpawn->Name : "UNKNOWN"));
}

class CEverQuestHook
{
public:
	DETOUR_TRAMPOLINE_DEF(char*, ReportSuccessfulHeal__Trampoline, (EQSuccessfulHeal*))
	char* ReportSuccessfulHeal__Detour(EQSuccessfulHeal* heal)
	{
		return ReportSuccessfulHeal__Trampoline(heal);
	}

	DETOUR_TRAMPOLINE_DEF(char,  ReportSuccessfulHit__Trampoline, (EQSuccessfulHit*, bool, int))
	char ReportSuccessfulHit__Detour(EQSuccessfulHit* hit, bool output, int actual)
	{
		if (hit && hit->DamageCaused > 0)
		{
			AddDamage(*hit);
		}

		return ReportSuccessfulHit__Trampoline(hit, output, actual);
	}
};

int SelfCallbackID = -1;
int OtherCallbackID = -1;
int OnmeCallbackID = -1;

void SelfDotCallback(const mq::TokenTextParam& param)
{
	// %1 has taken %2 damage from your %3.%4
	std::string_view targetName = param.Tokens.at(0);
	std::string_view damageString = param.Tokens.at(1);
	std::string_view spellName = param.Tokens.at(2);
	std::string_view critString = param.Tokens.at(3); // could be empty

	DebugSpewAlways("StringID: %d World: %d Color: %d Tokens:", param.StringID, param.World, param.Color);
	for (auto& token : param.Tokens)
	{
		DebugSpewAlways("%s", token.c_str());
	}
}

void OtherDotCallback(const mq::TokenTextParam& param)
{
	// %1 has taken %2 damage from %3 by %4.%5
}

void OnmeDotCallback(const mq::TokenTextParam& param)
{
	// You have taken %1 damage from %2 by %3.%4
}

/**
 * @fn InitializePlugin
 *
 * This is called once on plugin initialization and can be considered the startup
 * routine for the plugin.
 */
PLUGIN_API void InitializePlugin()
{
	DebugSpewAlways("MQ2DamageMeter::Initializing version %f", MQ2Version);

	EzDetour(CEverQuest__ReportSuccessfulHeal, &CEverQuestHook::ReportSuccessfulHeal__Detour, &CEverQuestHook::ReportSuccessfulHeal__Trampoline);
	EzDetour(CEverQuest__ReportSuccessfulHit, &CEverQuestHook::ReportSuccessfulHit__Detour, &CEverQuestHook::ReportSuccessfulHit__Trampoline);

	SelfCallbackID = AddTokenMessageCmd(SELF_DOT_STRINGID, SelfDotCallback);
	OtherCallbackID = AddTokenMessageCmd(OTHER_DOT_STRINGID, OtherDotCallback);
	OnmeCallbackID = AddTokenMessageCmd(ONME_DOT_STRINGID, OnmeDotCallback);
}

/**
 * @fn ShutdownPlugin
 *
 * This is called once when the plugin has been asked to shutdown.  The plugin has
 * not actually shut down until this completes.
 */
PLUGIN_API void ShutdownPlugin()
{
	DebugSpewAlways("MQ2DamageMeter::Shutting down");

	RemoveDetour(CEverQuest__ReportSuccessfulHeal);
	RemoveDetour(CEverQuest__ReportSuccessfulHit);

	if (SelfCallbackID >= 0)
		RemoveTokenMessageCmd(SELF_DOT_STRINGID, SelfCallbackID);

	if (OtherCallbackID >= 0)
		RemoveTokenMessageCmd(OTHER_DOT_STRINGID, OtherCallbackID);

	if (OnmeCallbackID >= 0)
		RemoveTokenMessageCmd(ONME_DOT_STRINGID, OnmeCallbackID);
}

/**
 * @fn OnUpdateImGui
 *
 * This is called each time that the ImGui Overlay is rendered. Use this to render
 * and update plugin specific widgets.
 *
 * Because this happens extremely frequently, it is recommended to move any actual
 * work to a separate call and use this only for updating the display.
 */
PLUGIN_API void OnUpdateImGui()
{
	if (GetGameState() == GAMESTATE_INGAME)
	{
		static bool ShowMQ2DamageMeterWindow = true;
		ImGui::Begin("MQ2DamageMeter", &ShowMQ2DamageMeterWindow, ImGuiWindowFlags_MenuBar);
		if (ImGui::BeginMenuBar())
		{
			ImGui::Text("MQ2DamageMeter is loaded!");
			ImGui::EndMenuBar();
		}

		static ImGuiTableFlags flags =
			ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable
			| ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV
			| ImGuiTableFlags_ScrollY;

		if (ImGui::BeginTable("##barchart", 2, flags))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Damage", ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthStretch);
			
			auto sort_specs = ImGui::TableGetSortSpecs();
			// TODO: Optimize by tracking here if damage has changed (simple case, it would be ideal to track if damage _order_ has changed)
			if (sort_specs != nullptr && sort_specs->SpecsDirty && damage_map.size() > 1)
			{
				std::sort(std::begin(damage_map), std::end(damage_map),
					[&sort_specs](const std::unique_ptr<AttackerTracking>& a, const std::unique_ptr<AttackerTracking>& b)
					{ return a->Compare(sort_specs, b); });

				sort_specs->SpecsDirty = false;
			}

			ImGui::TableHeadersRow();
			ImGuiListClipper clipper;
			clipper.Begin((int)damage_map.size());

			// TODO: Pre-calculuate the numbers we care about based on the GUI selections for display here

			// this is expensive-ish, so make sure we limit the number of draw routines we update
			auto totalDamage = std::accumulate(std::cbegin(damage_map), std::cend(damage_map), 0,
				[](int total, const std::unique_ptr<AttackerTracking>& item) { return total + item->GetTotal(); });

			while (clipper.Step())
			{
				for (int n = clipper.DisplayStart; n < clipper.DisplayEnd; ++n)
				{
					ImGui::PushID(damage_map[n]->ID);
					if (damage_map[n]->ShowProgressBar<ImGuiTreeNodeFlags_SpanFullWidth>(totalDamage))
					{
						for (auto& hit : damage_map[n]->GetBreakdown())
						{
							hit.ShowProgressBar<ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanFullWidth>(totalDamage);
						}
						ImGui::TreePop();
					}
					ImGui::PopID();
				}
			}

			ImGui::EndTable();
		}
		ImGui::End();
	}
}
}