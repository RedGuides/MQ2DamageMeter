// MQ2DamageMeter.cpp : Defines the entry point for the DLL application.
//

// PLUGIN_API is only to be used for callbacks.  All existing callbacks at this time
// are shown below. Remove the ones your plugin does not use.  Always use Initialize
// and Shutdown for setup and cleanup.

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

class HitItem : public EQSuccessfulHit
{
public:
	const std::string Name;
	const DWORD Timestamp;

	HitItem(const EQSuccessfulHit& hit, std::string_view Name) : 
		EQSuccessfulHit(hit),
		Name(Name),
		Timestamp(EQGetTime())
	{}
};

class DamageTrackingItem
{
public:
	const int ID;
	const std::string Name;
	std::multimap<const int, const HitItem> Hits;

	DamageTrackingItem(int ID, std::string_view Name) :
		ID(ID), Name(Name), Hits({}) {}

	int GetTotal(int HitID) const
	{
		auto hits = Hits.equal_range(HitID);
		return std::accumulate(hits.first, hits.second, 0,
			[](int total, const std::pair<const int, HitItem>& hit)
			{
				return total + hit.second.DamageCaused;
			});
	}

	int GetTotal() const
	{
		return std::accumulate(std::cbegin(Hits), std::cend(Hits), 0,
			[](int total, const std::pair<const int, HitItem>& hit)
			{
				return total + hit.second.DamageCaused;
			});
	}

	bool Compare(const ImGuiTableSortSpecs* sort_specs, const std::unique_ptr<DamageTrackingItem>& other) const
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

	std::vector<DamageTrackingItem> GetBreakdown()
	{
		return std::accumulate(std::cbegin(Hits), std::cend(Hits), std::vector<DamageTrackingItem>({}),
			[](std::vector<DamageTrackingItem>& items, const std::pair<const int, HitItem>& hit)
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
		ImGui::TableNextCell();
		ImGui::ProgressBar((float)hitTotal / total, ImVec2(-FLT_MIN, 0.f), std::to_string(hitTotal).c_str());

		return open;
	}
};

// indexed by attacker ID
std::vector<std::unique_ptr<DamageTrackingItem>> damage_map;
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

	auto damage = std::find_if(std::begin(damage_map), std::end(damage_map),
		[&attackerID](const std::unique_ptr<DamageTrackingItem>& damage_item)
		{
			return damage_item->ID == attackerID;
		});

	if (damage == std::end(damage_map))
	{
		damage = damage_map.emplace(
			std::end(damage_map),
			std::make_unique<DamageTrackingItem>(hit.AttackerID, attackerSpawn != nullptr ? attackerSpawn->Name : "UNKNOWN"));
	}

	// TODO: can the name and spawn change per ID? unlikely, but make sure we clear the list on zone to be safe (and perhaps log it?)

	auto damagedSpawn = GetSpawnByID(hit.DamagedID);
	(*damage)->Hits.emplace(hit.DamagedID, HitItem(hit, damagedSpawn != nullptr ? damagedSpawn->Name : "UNKNOWN"));
}

class CEverQuestHook
{
public:
	char* ReportSuccessfulHeal__Trampoline(EQSuccessfulHeal*);
	char* ReportSuccessfulHeal__Detour(EQSuccessfulHeal* heal)
	{
		return ReportSuccessfulHeal__Trampoline(heal);
	}

	char ReportSuccessfulHit__Trampoline(EQSuccessfulHit*, bool, int);
	char ReportSuccessfulHit__Detour(EQSuccessfulHit* hit, bool output, int actual)
	{
		if (hit && hit->DamageCaused > 0)
		{
			AddDamage(*hit);
		}

		return ReportSuccessfulHit__Trampoline(hit, output, actual);
	}
};

DETOUR_TRAMPOLINE_EMPTY(char* CEverQuestHook::ReportSuccessfulHeal__Trampoline(EQSuccessfulHeal*))
DETOUR_TRAMPOLINE_EMPTY(char CEverQuestHook::ReportSuccessfulHit__Trampoline(EQSuccessfulHit*, bool, int))

// This is just for testing, remove it because the disassembly appears to always call ReportSuccessfulHit from here
#define __msgReportSuccessfulHit_x 0x5AA0B0
INITIALIZE_EQGAME_OFFSET(__msgReportSuccessfulHit);
void msgReportSuccessfulHit_Trampoline(unsigned int, unsigned short*);
void msgReportSuccessfulHit_Detour(unsigned int param_1, unsigned short* param_1_00)
{
	auto hit = reinterpret_cast<EQSuccessfulHit*>(param_1_00);
	return msgReportSuccessfulHit_Trampoline(param_1, param_1_00);
}
DETOUR_TRAMPOLINE_EMPTY(void msgReportSuccessfulHit_Trampoline(unsigned int, unsigned short*))

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
	//EQSuccessfulHit hit({
	//	/*0x00*/ uint16_t      DamagedID;                // Spawn that was hit
	///*0x02*/ uint16_t      AttackerID;               // Spawn who did the hit
	///*0x04*/ uint8_t       Skill;                    // 1 HS etc...
	///*0x05*/ int           SpellID;
	///*0x09*/ int           DamageCaused;
	///*0x0d*/ float         Force;
	///*0x11*/ float         HitHeading;
	///*0x15*/ float         HitPitch;
	///*0x19*/ bool          bSecondary;
	///*0x1a*/ uint8_t       Unknown0x1A[6];
	//	});
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

	EzDetour(__msgReportSuccessfulHit, &msgReportSuccessfulHit_Detour, &msgReportSuccessfulHit_Trampoline);

	SelfCallbackID = AddTokenMessageCmd(SELF_DOT_STRINGID, SelfDotCallback);
	OtherCallbackID = AddTokenMessageCmd(OTHER_DOT_STRINGID, OtherDotCallback);
	OnmeCallbackID = AddTokenMessageCmd(ONME_DOT_STRINGID, OnmeDotCallback);

	// Examples:
	// AddCommand("/mycommand", MyCommand);
	// AddXMLFile("MQUI_MyXMLFile.xml");
	// AddMQ2Data("mytlo", MyTLOData);
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

	RemoveDetour(__msgReportSuccessfulHit);

	if (SelfCallbackID >= 0)
		RemoveTokenMessageCmd(SELF_DOT_STRINGID, SelfCallbackID);

	if (OtherCallbackID >= 0)
		RemoveTokenMessageCmd(OTHER_DOT_STRINGID, OtherCallbackID);

	if (OnmeCallbackID >= 0)
		RemoveTokenMessageCmd(ONME_DOT_STRINGID, OnmeCallbackID);

	// Examples:
	// RemoveCommand("/mycommand");
	// RemoveXMLFile("MQUI_MyXMLFile.xml");
	// RemoveMQ2Data("mytlo");
}

/**
 * @fn OnCleanUI
 *
 * This is called once just before the shutdown of the UI system and each time the
 * game requests that the UI be cleaned.  Most commonly this happens when a
 * /loadskin command is issued, but it also occurs when reaching the character
 * select screen and when first entering the game.
 *
 * One purpose of this function is to allow you to destroy any custom windows that
 * you have created and cleanup any UI items that need to be removed.
 */
PLUGIN_API void OnCleanUI()
{
	// DebugSpewAlways("MQ2DamageMeter::OnCleanUI()");
}

/**
 * @fn OnReloadUI
 *
 * This is called once just after the UI system is loaded. Most commonly this
 * happens when a /loadskin command is issued, but it also occurs when first
 * entering the game.
 *
 * One purpose of this function is to allow you to recreate any custom windows
 * that you have setup.
 */
PLUGIN_API void OnReloadUI()
{
	// DebugSpewAlways("MQ2DamageMeter::OnReloadUI()");
}

/**
 * @fn OnDrawHUD
 *
 * This is called each time the Heads Up Display (HUD) is drawn.  The HUD is
 * responsible for the net status and packet loss bar.
 *
 * Note that this is not called at all if the HUD is not shown (default F11 to
 * toggle).
 *
 * Because the net status is updated frequently, it is recommended to have a
 * timer or counter at the start of this call to limit the amount of times the
 * code in this section is executed.
 */
PLUGIN_API void OnDrawHUD()
{
	/*
		static int DrawHUDCount = 0;
		// Skip ~500 draws
		if (++DrawHUDCount > 500)
		{
			DrawHUDCount = 0;
			DebugSpewAlways("MQ2DamageMeter::OnDrawHUD()");
		}
	*/
}

/**
 * @fn SetGameState
 *
 * This is called when the GameState changes.  It is also called once after the
 * plugin is initialized.
 *
 * For a list of known GameState values, see the constants that begin with
 * GAMESTATE_.  The most commonly used of these is GAMESTATE_INGAME.
 *
 * When zoning, this is called once after @ref OnBeginZone @ref OnRemoveSpawn
 * and @ref OnRemoveGroundItem are all done and then called once again after
 * @ref OnEndZone and @ref OnAddSpawn are done but prior to @ref OnAddGroundItem
 * and @ref OnZoned
 *
 * @param GameState int - The value of GameState at the time of the call
 */
PLUGIN_API void SetGameState(int GameState)
{
	// DebugSpewAlways("MQ2DamageMeter::SetGameState(%d)", GameState);
}


/**
 * @fn OnPulse
 *
 * This is called each time MQ2 goes through its heartbeat (pulse) function.
 *
 * Because this happens very frequently, it is recommended to have a timer or
 * counter at the start of this call to limit the amount of times the code in
 * this section is executed.
 */
PLUGIN_API void OnPulse()
{
	/*
		static int PulseCount = 0;
		// Skip ~500 pulses
		if (++PulseCount > 500)
		{
			PulseCount = 0;
			DebugSpewAlways("MQ2DamageMeter::OnPulse()");
		}
	*/
}

/**
 * @fn OnWriteChatColor
 *
 * This is called each time WriteChatColor is called (whether by MQ2Main or by any
 * plugin).  This can be considered the "when outputting text from MQ" callback.
 *
 * This ignores filters on display, so if they are needed either implement them in
 * this section or see @ref OnIncomingChat where filters are already handled.
 *
 * If CEverQuest::dsp_chat is not called, and events are required, they'll need to
 * be implemented here as well.  Otherwise, see @ref OnIncomingChat where that is
 * already handled.
 *
 * For a list of Color values, see the constants for USERCOLOR_.  The default is
 * USERCOLOR_DEFAULT.
 *
 * @param Line const char* - The line that was passed to WriteChatColor
 * @param Color int - The type of chat text this is to be sent as
 * @param Filter int - (default 0)
 */
PLUGIN_API void OnWriteChatColor(const char* Line, int Color, int Filter)
{
	// DebugSpewAlways("MQ2DamageMeter::OnWriteChatColor(%s, %d, %d)", Line, Color, Filter);
}

/**
 * @fn OnIncomingChat
 *
 * This is called each time a line of chat is shown.  It occurs after MQ filters
 * and chat events have been handled.  If you need to know when MQ2 has sent chat,
 * consider using @ref OnWriteChatColor instead.
 *
 * For a list of Color values, see the constants for USERCOLOR_. The default is
 * USERCOLOR_DEFAULT.
 *
 * @param Line const char* - The line of text that was shown
 * @param Color int - The type of chat text this was sent as
 *
 * @return bool - whether something was done based on the incoming chat
 */
PLUGIN_API bool OnIncomingChat(const char* Line, DWORD Color)
{
	// DebugSpewAlways("MQ2DamageMeter::OnIncomingChat(%s, %d)", Line, Color);
	return false;
}

/**
 * @fn OnAddSpawn
 *
 * This is called each time a spawn is added to a zone (ie, something spawns). It is
 * also called for each existing spawn when a plugin first initializes.
 *
 * When zoning, this is called for all spawns in the zone after @ref OnEndZone is
 * called and before @ref OnZoned is called.
 *
 * @param pNewSpawn PSPAWNINFO - The spawn that was added
 */
PLUGIN_API void OnAddSpawn(PSPAWNINFO pNewSpawn)
{
	// DebugSpewAlways("MQ2DamageMeter::OnAddSpawn(%s)", pNewSpawn->Name);
}

/**
 * @fn OnRemoveSpawn
 *
 * This is called each time a spawn is removed from a zone (ie, something despawns
 * or is killed).  It is NOT called when a plugin shuts down.
 *
 * When zoning, this is called for all spawns in the zone after @ref OnBeginZone is
 * called.
 *
 * @param pSpawn PSPAWNINFO - The spawn that was removed
 */
PLUGIN_API void OnRemoveSpawn(PSPAWNINFO pSpawn)
{
	// DebugSpewAlways("MQ2DamageMeter::OnRemoveSpawn(%s)", pSpawn->Name);
}

/**
 * @fn OnAddGroundItem
 *
 * This is called each time a ground item is added to a zone (ie, something spawns).
 * It is also called for each existing ground item when a plugin first initializes.
 *
 * When zoning, this is called for all ground items in the zone after @ref OnEndZone
 * is called and before @ref OnZoned is called.
 *
 * @param pNewGroundItem PGROUNDITEM - The ground item that was added
 */
PLUGIN_API void OnAddGroundItem(PGROUNDITEM pNewGroundItem)
{
	// DebugSpewAlways("MQ2DamageMeter::OnAddGroundItem(%d)", pNewGroundItem->DropID);
}

/**
 * @fn OnRemoveGroundItem
 *
 * This is called each time a ground item is removed from a zone (ie, something
 * despawns or is picked up).  It is NOT called when a plugin shuts down.
 *
 * When zoning, this is called for all ground items in the zone after
 * @ref OnBeginZone is called.
 *
 * @param pGroundItem PGROUNDITEM - The ground item that was removed
 */
PLUGIN_API void OnRemoveGroundItem(PGROUNDITEM pGroundItem)
{
	// DebugSpewAlways("MQ2DamageMeter::OnRemoveGroundItem(%d)", pGroundItem->DropID);
}

/**
 * @fn OnBeginZone
 *
 * This is called just after entering a zone line and as the loading screen appears.
 */
PLUGIN_API void OnBeginZone()
{
	// DebugSpewAlways("MQ2DamageMeter::OnBeginZone()");
}

/**
 * @fn OnEndZone
 *
 * This is called just after the loading screen, but prior to the zone being fully
 * loaded.
 *
 * This should occur before @ref OnAddSpawn and @ref OnAddGroundItem are called. It
 * always occurs before @ref OnZoned is called.
 */
PLUGIN_API void OnEndZone()
{
	// DebugSpewAlways("MQ2DamageMeter::OnEndZone()");
}

/**
 * @fn OnZoned
 *
 * This is called after entering a new zone and the zone is considered "loaded."
 *
 * It occurs after @ref OnEndZone @ref OnAddSpawn and @ref OnAddGroundItem have
 * been called.
 */
PLUGIN_API void OnZoned()
{
	// DebugSpewAlways("MQ2DamageMeter::OnZoned()");
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
	/*
		static int UpdateCount = 0;
		// Skip ~4000 updates for debug spew message
		if (++UpdateCount > 4000)
		{
			UpdateCount = 0;
			DebugSpewAlways("MQ2DamageMeter::OnUpdateImGui()");
		}

		if (GetGameState() == GAMESTATE_INGAME)
		{
			static bool ShowMQ2DamageMeterWindow = true;
			ImGui::Begin("MQ2DamageMeter", &ShowMQ2DamageMeterWindow, ImGuiWindowFlags_MenuBar);
			if (ImGui::BeginMenuBar())
			{
				ImGui::Text("MQ2DamageMeter is loaded!");
				ImGui::EndMenuBar();
			}
			ImGui::End();
		}
	*/
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
			ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_MultiSortable
			| ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV
			| ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollFreezeTopRow;

		if (ImGui::BeginTable("##barchart", 2, flags))
		{
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Damage", ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthStretch);
			
			auto sort_specs = ImGui::TableGetSortSpecs();
			// TODO: Optimize by tracking here if damage has changed (simple case, it would be ideal to track if damage _order_ has changed)
			if (sort_specs != nullptr && sort_specs->SpecsChanged && damage_map.size() > 1)
			{
				std::sort(std::begin(damage_map), std::end(damage_map),
					[&sort_specs](const std::unique_ptr<DamageTrackingItem>& a, const std::unique_ptr<DamageTrackingItem>& b)
					{ return a->Compare(sort_specs, b); });
			}

			ImGui::TableAutoHeaders();
			ImGuiListClipper clipper;
			clipper.Begin(damage_map.size());

			// TODO: Pre-calculuate the numbers we care about based on the GUI selections for display here

			// this is expensive-ish, so make sure we limit the number of draw routines we update
			auto totalDamage = std::accumulate(std::cbegin(damage_map), std::cend(damage_map), 0,
				[](int total, const std::unique_ptr<DamageTrackingItem>& item) { return total + item->GetTotal(); });

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

/**
 * @fn OnMacroStart
 *
 * This is called each time a macro starts (ex: /mac somemacro.mac), prior to
 * launching the macro.
 *
 * @param Name const char* - The name of the macro that was launched
 */
PLUGIN_API void OnMacroStart(const char* Name)
{
	// DebugSpewAlways("MQ2DamageMeter::OnMacroStart(%s)", Name);
}

/**
 * @fn OnMacroStop
 *
 * This is called each time a macro stops (ex: /endmac), after the macro has ended.
 *
 * @param Name const char* - The name of the macro that was stopped.
 */
PLUGIN_API void OnMacroStop(const char* Name)
{
	// DebugSpewAlways("MQ2DamageMeter::OnMacroStop(%s)", Name);
}

/**
 * @fn OnLoadPlugin
 *
 * This is called each time a plugin is loaded (ex: /plugin someplugin), after the
 * plugin has been loaded and any associated -AutoExec.cfg file has been launched.
 * This means it will be executed after the plugin's @ref InitializePlugin callback.
 *
 * This is also called when THIS plugin is loaded, but initialization tasks should
 * still be done in @ref InitializePlugin.
 *
 * @param Name const char* - The name of the plugin that was loaded
 */
PLUGIN_API void OnLoadPlugin(const char* Name)
{
	// DebugSpewAlways("MQ2DamageMeter::OnLoadPlugin(%s)", Name);
}

/**
 * @fn OnUnloadPlugin
 *
 * This is called each time a plugin is unloaded (ex: /plugin someplugin unload),
 * just prior to the plugin unloading.  This means it will be executed prior to that
 * plugin's @ref ShutdownPlugin callback.
 *
 * This is also called when THIS plugin is unloaded, but shutdown tasks should still
 * be done in @ref ShutdownPlugin.
 *
 * @param Name const char* - The name of the plugin that is to be unloaded
 */
PLUGIN_API void OnUnloadPlugin(const char* Name)
{
	// DebugSpewAlways("MQ2DamageMeter::OnUnloadPlugin(%s)", Name);
}
}