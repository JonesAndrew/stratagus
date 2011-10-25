//       _________ __                 __
//      /   _____//  |_____________ _/  |______     ____  __ __  ______
//      \_____  \\   __\_  __ \__  \\   __\__  \   / ___\|  |  \/  ___/
//      /        \|  |  |  | \// __ \|  |  / __ \_/ /_/  >  |  /\___ |
//     /_______  /|__|  |__|  (____  /__| (____  /\___  /|____//____  >
//             \/                  \/          \//_____/            \/
//  ______________________                           ______________________
//                        T H E   W A R   B E G I N S
//         Stratagus - A free fantasy real time strategy game engine
/**@name replay.cpp - Replay game. */
//
//      (c) Copyright 2000-2008 by Lutz Sammer, Andreas Arens, and Jimmy Salmon.
//
//      This program is free software; you can redistribute it and/or modify
//      it under the terms of the GNU General Public License as published by
//      the Free Software Foundation; only version 2 of the License.
//
//      This program is distributed in the hope that it will be useful,
//      but WITHOUT ANY WARRANTY; without even the implied warranty of
//      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//      GNU General Public License for more details.
//
//      You should have received a copy of the GNU General Public License
//      along with this program; if not, write to the Free Software
//      Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
//      02111-1307, USA.
//

//@{

//----------------------------------------------------------------------------
// Includes
//----------------------------------------------------------------------------

#include <time.h>

#include "stratagus.h"
#include "replay.h"
//#include "version.h"
#include "iolib.h"
#include "iocompat.h"
#include "script.h"
#include "unittype.h"
#include "unit_manager.h"
#include "settings.h"
#include "commands.h"
#include "player.h"
#include "map.h"
#include "netconnect.h"
#include "network.h"
#include "interface.h"
#include "actions.h"

//----------------------------------------------------------------------------
// Structures
//----------------------------------------------------------------------------

/**
**  LogEntry structure.
*/
class LogEntry {
public:
	LogEntry() : GameCycle(0), Flush(0), PosX(0), PosY(0), DestUnitNumber(0),
		Num(0), SyncRandSeed(0), Next(NULL)
	{
		UnitNumber = 0;
	}

	unsigned long GameCycle;
	int UnitNumber;
	std::string UnitIdent;
	std::string Action;
	int Flush;
	int PosX;
	int PosY;
	int DestUnitNumber;
	std::string Value;
	int Num;
	unsigned SyncRandSeed;
	LogEntry *Next;
};

/**
**  Multiplayer Player definition
*/
class MPPlayer {
public:
	MPPlayer() : Race(0), Team(0), Type(0) {}

	std::string Name;
	int Race;
	int Team;
	int Type;
};

/**
** Full replay structure (definition + logs)
*/
class FullReplay {
public:
	FullReplay() :
		MapId(0), Type(0), Race(0), LocalPlayer(0),
		Resource(0), NumUnits(0), Difficulty(0), NoFow(false), RevealMap(0),
		MapRichness(0), GameType(0), Opponents(0), Commands(NULL)
	{
		memset(Engine, 0, sizeof(Engine));
		memset(Network, 0, sizeof(Network));
	}
	std::string Comment1;
	std::string Comment2;
	std::string Comment3;
	std::string Date;
	std::string Map;
	std::string MapPath;
	unsigned MapId;

	int Type;
	int Race;
	int LocalPlayer;
	MPPlayer Players[PlayerMax];

	int Resource;
	int NumUnits;
	int Difficulty;
	bool NoFow;
	int RevealMap;
	int MapRichness;
	int GameType;
	int Opponents;
	int Engine[3];
	int Network[3];
	LogEntry *Commands;
};

//----------------------------------------------------------------------------
// Constants
//----------------------------------------------------------------------------


//----------------------------------------------------------------------------
// Variables
//----------------------------------------------------------------------------

bool CommandLogDisabled;           /// True if command log is off
ReplayType ReplayGameType;         /// Replay game type
static bool DisabledLog;           /// Disabled log for replay
static CFile *LogFile;             /// Replay log file
static unsigned long NextLogCycle; /// Next log cycle number
static int InitReplay;             /// Initialize replay
static FullReplay *CurrentReplay;
static LogEntry *ReplayStep;

static void AppendLog(LogEntry *log, CFile *dest);

//----------------------------------------------------------------------------
// Log commands
//----------------------------------------------------------------------------

/**
** Allocate & fill a new FullReplay structure, from GameSettings.
**
** @return A new FullReplay structure
*/
static FullReplay *StartReplay()
{
	FullReplay *replay;
	char *s;
	time_t now;
	char *s1;

	replay = new FullReplay;

	time(&now);
	s = ctime(&now);
	if ((s1 = strchr(s, '\n'))) {
		*s1 = '\0';
	}

	replay->Comment1 = "Generated by Stratagus Version " VERSION "";
	replay->Comment2 = "Visit " HOMEPAGE " for more information";

	if (GameSettings.NetGameType == SettingsSinglePlayerGame) {
		replay->Type = ReplaySinglePlayer;
	} else {
		replay->Type = ReplayMultiPlayer;
	}

	for (int i = 0; i < PlayerMax; ++i) {
		replay->Players[i].Name = Players[i].Name;
		replay->Players[i].Race = GameSettings.Presets[i].Race;
		replay->Players[i].Team = GameSettings.Presets[i].Team;
		replay->Players[i].Type = GameSettings.Presets[i].Type;
	}

	replay->LocalPlayer = ThisPlayer->Index;

	replay->Date = s;
	replay->Map = Map.Info.Description;
	replay->MapId = (signed int)Map.Info.MapUID;
	replay->MapPath = CurrentMapPath;
	replay->Resource = GameSettings.Resources;
	replay->NumUnits = GameSettings.NumUnits;
	replay->Difficulty = GameSettings.Difficulty;
	replay->NoFow = GameSettings.NoFogOfWar;
	replay->GameType = GameSettings.GameType;
	replay->RevealMap = GameSettings.RevealMap;
	replay->MapRichness = GameSettings.MapRichness;
	replay->Opponents = GameSettings.Opponents;

	replay->Engine[0] = StratagusMajorVersion;
	replay->Engine[1] = StratagusMinorVersion;
	replay->Engine[2] = StratagusPatchLevel;

	replay->Network[0] = NetworkProtocolMajorVersion;
	replay->Network[1] = NetworkProtocolMinorVersion;
	replay->Network[2] = NetworkProtocolPatchLevel;
	return replay;
}

/**
**  Applies settings the game used at the start of the replay
*/
static void ApplyReplaySettings()
{
	if (CurrentReplay->Type == ReplayMultiPlayer) {
		ExitNetwork1();
		NetPlayers = 2;
		GameSettings.NetGameType = SettingsMultiPlayerGame;

		ReplayGameType = ReplayMultiPlayer;
		NetLocalPlayerNumber = CurrentReplay->LocalPlayer;
	} else {
		GameSettings.NetGameType = SettingsSinglePlayerGame;
		ReplayGameType = ReplaySinglePlayer;
	}

	for (int i = 0; i < PlayerMax; ++i) {
		GameSettings.Presets[i].Race = CurrentReplay->Players[i].Race;
		GameSettings.Presets[i].Team = CurrentReplay->Players[i].Team;
		GameSettings.Presets[i].Type = CurrentReplay->Players[i].Type;
	}

	if (strcpy_s(CurrentMapPath, sizeof(CurrentMapPath), CurrentReplay->MapPath.c_str()) != 0) {
		fprintf(stderr, "Replay map path is too long\n");
		// FIXME: need to handle errors better
		Exit(1);
	}
	GameSettings.Resources = CurrentReplay->Resource;
	GameSettings.NumUnits = CurrentReplay->NumUnits;
	GameSettings.Difficulty = CurrentReplay->Difficulty;
	Map.NoFogOfWar = GameSettings.NoFogOfWar = CurrentReplay->NoFow;
	GameSettings.GameType = CurrentReplay->GameType;
	FlagRevealMap = GameSettings.RevealMap = CurrentReplay->RevealMap;
	GameSettings.MapRichness = CurrentReplay->MapRichness;
	GameSettings.Opponents = CurrentReplay->Opponents;

	// FIXME : check engine version
	// FIXME : FIXME: check network version
	// FIXME : check mapid
}

/**
**  Free a replay from memory
**
**  @param replay  Pointer to the replay to be freed
*/
static void DeleteReplay(FullReplay *replay)
{
	LogEntry *log;
	LogEntry *next;

	log = replay->Commands;
	while (log) {
		next = log->Next;
		delete log;
		log = next;
	}

	delete replay;
}

static void PrintLogCommand(LogEntry *log, CFile *dest)
{
	dest->printf("Log( { ");
	dest->printf("GameCycle = %lu, ", log->GameCycle);
	if (log->UnitNumber != -1) {
		dest->printf("UnitNumber = %d, ", log->UnitNumber);
	}
	if (!log->UnitIdent.empty()) {
		dest->printf("UnitIdent = \"%s\", ", log->UnitIdent.c_str());
	}
	dest->printf("Action = \"%s\", ", log->Action.c_str());
	dest->printf("Flush = %d, ", log->Flush);
	if (log->PosX != -1 || log->PosY != -1) {
		dest->printf("PosX = %d, PosY = %d, ", log->PosX, log->PosY);
	}
	if (log->DestUnitNumber != -1) {
		dest->printf("DestUnitNumber = %d, ", log->DestUnitNumber);
	}
	if (!log->Value.empty()) {
		dest->printf("Value = [[%s]], ", log->Value.c_str());
	}
	if (log->Num != -1) {
		dest->printf("Num = %d, ", log->Num);
	}
	dest->printf("SyncRandSeed = %d } )\n", (signed)log->SyncRandSeed);
}

/**
**  Output the FullReplay list to dest file
**
**  @param dest  The file to output to
*/
static void SaveFullLog(CFile *dest)
{
	LogEntry *log;
	int i;

	dest->printf("\n--- -----------------------------------------\n");
	dest->printf("--- MODULE: replay list\n");

	dest->printf("\n");
	dest->printf("ReplayLog( {\n");
	dest->printf("  Comment1 = \"%s\",\n", CurrentReplay->Comment1.c_str());
	dest->printf("  Comment2 = \"%s\",\n", CurrentReplay->Comment2.c_str());
	dest->printf("  Date = \"%s\",\n", CurrentReplay->Date.c_str());
	dest->printf("  Map = \"%s\",\n", CurrentReplay->Map.c_str());
	dest->printf("  MapPath = \"%s\",\n", CurrentReplay->MapPath.c_str());
	dest->printf("  MapId = %u,\n", CurrentReplay->MapId);
	dest->printf("  Type = %d,\n", CurrentReplay->Type);
	dest->printf("  Race = %d,\n", CurrentReplay->Race);
	dest->printf("  LocalPlayer = %d,\n", CurrentReplay->LocalPlayer);
	dest->printf("  Players = {\n");
	for (i = 0; i < PlayerMax; ++i) {
		if (!CurrentReplay->Players[i].Name.empty()) {
			dest->printf("\t{ Name = \"%s\",", CurrentReplay->Players[i].Name.c_str());
		} else {
			dest->printf("\t{");
		}
		dest->printf(" Race = %d,", CurrentReplay->Players[i].Race);
		dest->printf(" Team = %d,", CurrentReplay->Players[i].Team);
		dest->printf(" Type = %d }%s", CurrentReplay->Players[i].Type,
			i != PlayerMax - 1 ? ",\n" : "\n");
	}
	dest->printf("  },\n");
	dest->printf("  Resource = %d,\n", CurrentReplay->Resource);
	dest->printf("  NumUnits = %d,\n", CurrentReplay->NumUnits);
	dest->printf("  Difficulty = %d,\n", CurrentReplay->Difficulty);
	dest->printf("  NoFow = %s,\n", CurrentReplay->NoFow ? "true" : "false");
	dest->printf("  RevealMap = %d,\n", CurrentReplay->RevealMap);
	dest->printf("  GameType = %d,\n", CurrentReplay->GameType);
	dest->printf("  Opponents = %d,\n", CurrentReplay->Opponents);
	dest->printf("  MapRichness = %d,\n", CurrentReplay->MapRichness);
	dest->printf("  Engine = { %d, %d, %d },\n",
		CurrentReplay->Engine[0], CurrentReplay->Engine[1], CurrentReplay->Engine[2]);
	dest->printf("  Network = { %d, %d, %d }\n",
		CurrentReplay->Network[0], CurrentReplay->Network[1], CurrentReplay->Network[2]);
	dest->printf("} )\n");
	log = CurrentReplay->Commands;
	while (log) {
		PrintLogCommand(log, dest);
		log = log->Next;
	}
}

/**
**  Append the LogEntry structure at the end of currentLog, and to LogFile
**
**  @param log   Pointer the replay log entry to be added
**  @param dest  The file to output to
*/
static void AppendLog(LogEntry *log, CFile *dest)
{
	LogEntry **last;

	// Append to linked list
	last = &CurrentReplay->Commands;
	while (*last) {
		last = &(*last)->Next;
	}

	*last = log;
	log->Next = 0;

	// Append to file
	if (!dest) {
		return;
	}

	PrintLogCommand(log, dest);
	dest->flush();
}

/**
**  Log commands into file.
**
**  This could later be used to recover, crashed games.
**
**  @param action  Command name (move,attack,...).
**  @param unit    Unit that receive the command.
**  @param flush   Append command or flush old commands.
**  @param x       optional X map position.
**  @param y       optional y map position.
**  @param dest    optional destination unit.
**  @param value   optional command argument (unit-type,...).
**  @param num     optional number argument
*/
void CommandLog(const char *action, const CUnit *unit, int flush,
	int x, int y, const CUnit *dest, const char *value, int num)
{
	LogEntry *log;

	if (CommandLogDisabled) { // No log wanted
		return;
	}

	//
	// Create and write header of log file. The player number is added
	// to the save file name, to test more than one player on one computer.
	//
	if (!LogFile) {
		struct stat tmp;
		char buf[16];
		std::string path(Parameters::Instance.GetUserDirectory());
		if(!GameName.empty()) {
			path += "/";
			path += GameName;
		}
		path += "/logs";

		if(stat(path.c_str(), &tmp) < 0) {
			makedir(path.c_str(), 0777);
		}

		snprintf(buf, sizeof(buf), "%d", ThisPlayer->Index);

		path += "/log_of_stratagus_";
		path += buf;
		path += ".log";

		LogFile = new CFile;
		if (LogFile->open(path.c_str(), CL_OPEN_WRITE) == -1) {
			// don't retry for each command
			CommandLogDisabled = false;
			delete LogFile;
			LogFile = NULL;
			return;
		}

		if (CurrentReplay) {
			SaveFullLog(LogFile);
		}
	}

	if (!CurrentReplay) {
		CurrentReplay = StartReplay();

		SaveFullLog(LogFile);
	}

	if (!action) {
		return;
	}

	log = new LogEntry;

	//
	// Frame, unit, (type-ident only to be better readable).
	//
	log->GameCycle = GameCycle;

	log->UnitNumber = (unit ? UnitNumber(*unit) : -1);
	log->UnitIdent = (unit ? unit->Type->Ident.c_str() : "");

	log->Action = action;
	log->Flush = flush;

	//
	// Coordinates given.
	//
	log->PosX = x;
	log->PosY = y;

	//
	// Destination given.
	//
	log->DestUnitNumber = (dest ? UnitNumber(*dest) : -1);

	//
	// Value given.
	//
	log->Value = (value ? value : "");

	//
	// Number given.
	//
	log->Num = num;

	log->SyncRandSeed = SyncRandSeed;

	// Append it to ReplayLog list
	AppendLog(log, LogFile);
}

/**
** Parse log
*/
static int CclLog(lua_State *l)
{
	LogEntry *log;
	LogEntry **last;
	const char *value;

	LuaCheckArgs(l, 1);
	if (!lua_istable(l, 1)) {
		LuaError(l, "incorrect argument");
	}

	Assert(CurrentReplay);

	log = new LogEntry;
	log->UnitNumber = -1;
	log->PosX = -1;
	log->PosY = -1;
	log->DestUnitNumber = -1;
	log->Num = -1;

	lua_pushnil(l);
	while (lua_next(l, 1)) {
		value = LuaToString(l, -2);
		if (!strcmp(value, "GameCycle")) {
			log->GameCycle = LuaToNumber(l, -1);
		} else if (!strcmp(value, "UnitNumber")) {
			log->UnitNumber = LuaToNumber(l, -1);
		} else if (!strcmp(value, "UnitIdent")) {
			log->UnitIdent = LuaToString(l, -1);
		} else if (!strcmp(value, "Action")) {
			log->Action = LuaToString(l, -1);
		} else if (!strcmp(value, "Flush")) {
			log->Flush = LuaToNumber(l, -1);
		} else if (!strcmp(value, "PosX")) {
			log->PosX = LuaToNumber(l, -1);
		} else if (!strcmp(value, "PosY")) {
			log->PosY = LuaToNumber(l, -1);
		} else if (!strcmp(value, "DestUnitNumber")) {
			log->DestUnitNumber = LuaToNumber(l, -1);
		} else if (!strcmp(value, "Value")) {
			log->Value = LuaToString(l, -1);
		} else if (!strcmp(value, "Num")) {
			log->Num = LuaToNumber(l, -1);
		} else if (!strcmp(value, "SyncRandSeed")) {
			log->SyncRandSeed = (unsigned)LuaToNumber(l, -1);
		} else {
			LuaError(l, "Unsupported key: %s" _C_ value);
		}
		lua_pop(l, 1);
	}

	// Append to linked list
	last = &CurrentReplay->Commands;
	while (*last) {
		last = &(*last)->Next;
	}

	*last = log;

	return 0;
}

/**
** Parse replay-log
*/
static int CclReplayLog(lua_State *l)
{
	FullReplay *replay;
	const char *value;
	int j;

	LuaCheckArgs(l, 1);
	if (!lua_istable(l, 1)) {
		LuaError(l, "incorrect argument");
	}

	Assert(CurrentReplay == NULL);

	replay = new FullReplay;

	lua_pushnil(l);
	while (lua_next(l, 1) != 0) {
		value = LuaToString(l, -2);
		if (!strcmp(value, "Comment1")) {
			replay->Comment1 = LuaToString(l, -1);
		} else if (!strcmp(value, "Comment2")) {
			replay->Comment2 = LuaToString(l, -1);
		} else if (!strcmp(value, "Comment3")) {
			replay->Comment3 = LuaToString(l, -1);
		} else if (!strcmp(value, "Date")) {
			replay->Date = LuaToString(l, -1);
		} else if (!strcmp(value, "Map")) {
			replay->Map = LuaToString(l, -1);
		} else if (!strcmp(value, "MapPath")) {
			replay->MapPath = LuaToString(l, -1);
		} else if (!strcmp(value, "MapId")) {
			replay->MapId = LuaToNumber(l, -1);
		} else if (!strcmp(value, "Type")) {
			replay->Type = LuaToNumber(l, -1);
		} else if (!strcmp(value, "Race")) {
			replay->Race = LuaToNumber(l, -1);
		} else if (!strcmp(value, "LocalPlayer")) {
			replay->LocalPlayer = LuaToNumber(l, -1);
		} else if (!strcmp(value, "Players")) {
			if (!lua_istable(l, -1) || lua_objlen(l, -1) != PlayerMax) {
				LuaError(l, "incorrect argument");
			}
			for (j = 0; j < PlayerMax; ++j) {
				int top;

				lua_rawgeti(l, -1, j + 1);
				if (!lua_istable(l, -1)) {
					LuaError(l, "incorrect argument");
				}
				top = lua_gettop(l);
				lua_pushnil(l);
				while (lua_next(l, top) != 0) {
					value = LuaToString(l, -2);
					if (!strcmp(value, "Name")) {
						replay->Players[j].Name = LuaToString(l, -1);
					} else if (!strcmp(value, "Race")) {
						replay->Players[j].Race = LuaToNumber(l, -1);
					} else if (!strcmp(value, "Team")) {
						replay->Players[j].Team = LuaToNumber(l, -1);
					} else if (!strcmp(value, "Type")) {
						replay->Players[j].Type = LuaToNumber(l, -1);
					} else {
						LuaError(l, "Unsupported key: %s" _C_ value);
					}
					lua_pop(l, 1);
				}
				lua_pop(l, 1);
			}
		} else if (!strcmp(value, "Resource")) {
			replay->Resource = LuaToNumber(l, -1);
		} else if (!strcmp(value, "NumUnits")) {
			replay->NumUnits = LuaToNumber(l, -1);
		} else if (!strcmp(value, "Difficulty")) {
			replay->Difficulty = LuaToNumber(l, -1);
		} else if (!strcmp(value, "NoFow")) {
			replay->NoFow = LuaToBoolean(l, -1);
		} else if (!strcmp(value, "RevealMap")) {
			replay->RevealMap = LuaToNumber(l, -1);
		} else if (!strcmp(value, "GameType")) {
			replay->GameType = LuaToNumber(l, -1);
		} else if (!strcmp(value, "Opponents")) {
			replay->Opponents = LuaToNumber(l, -1);
		} else if (!strcmp(value, "MapRichness")) {
			replay->MapRichness = LuaToNumber(l, -1);
		} else if (!strcmp(value, "Engine")) {
			if (!lua_istable(l, -1) || lua_objlen(l, -1) != 3) {
				LuaError(l, "incorrect argument");
			}
			lua_rawgeti(l, -1, 1);
			replay->Engine[0] = LuaToNumber(l, -1);
			lua_pop(l, 1);
			lua_rawgeti(l, -1, 2);
			replay->Engine[1] = LuaToNumber(l, -1);
			lua_pop(l, 1);
			lua_rawgeti(l, -1, 3);
			replay->Engine[2] = LuaToNumber(l, -1);
			lua_pop(l, 1);
		} else if (!strcmp(value, "Network")) {
			if (!lua_istable(l, -1) || lua_objlen(l, -1) != 3) {
				LuaError(l, "incorrect argument");
			}
			lua_rawgeti(l, -1, 1);
			replay->Network[0] = LuaToNumber(l, -1);
			lua_pop(l, 1);
			lua_rawgeti(l, -1, 2);
			replay->Network[1] = LuaToNumber(l, -1);
			lua_pop(l, 1);
			lua_rawgeti(l, -1, 3);
			replay->Network[2] = LuaToNumber(l, -1);
			lua_pop(l, 1);
		} else {
			LuaError(l, "Unsupported key: %s" _C_ value);
		}
		lua_pop(l, 1);
	}

	CurrentReplay = replay;

	// Apply CurrentReplay settings.
	if (!SaveGameLoading) {
		ApplyReplaySettings();
	} else {
		CommandLogDisabled = false;
	}

	return 0;
}

/**
**  Check if we're replaying a game
*/
bool IsReplayGame()
{
	return ReplayGameType != ReplayNone;
}

/**
**  Save generated replay
**
**  @param file  file to save to.
*/
void SaveReplayList(CFile *file)
{
	SaveFullLog(file);
}

/**
**  Load a log file to replay a game
**
**  @param name  name of file to load.
*/
int LoadReplay(const std::string &name)
{
	CleanReplayLog();
	ReplayGameType = ReplaySinglePlayer;

	LuaLoadFile(name);

	NextLogCycle = ~0UL;
	if (!CommandLogDisabled) {
		CommandLogDisabled = true;
		DisabledLog = true;
	}
	GameObserve = true;
	InitReplay = 1;

	return 0;
}

/**
**  End logging
*/
void EndReplayLog()
{
	if (LogFile) {
		LogFile->close();
		delete LogFile;
		LogFile = NULL;
	}
	if (CurrentReplay) {
		DeleteReplay(CurrentReplay);
		CurrentReplay = NULL;
	}
	ReplayStep = NULL;
}

/**
**  Clean replay log
*/
void CleanReplayLog()
{
	if (CurrentReplay) {
		DeleteReplay(CurrentReplay);
		CurrentReplay = 0;
	}
	ReplayStep = NULL;

// if (DisabledLog) {
		CommandLogDisabled = false;
		DisabledLog = false;
// }
	GameObserve = false;
	NetPlayers = 0;
	ReplayGameType = ReplayNone;
}

/**
**  Do next replay
*/
static void DoNextReplay()
{
	Assert(ReplayStep != 0);

	NextLogCycle = ReplayStep->GameCycle;

	if (NextLogCycle != GameCycle) {
		return;
	}

	const int unit = ReplayStep->UnitNumber;
	const char *action = ReplayStep->Action.c_str();
	const int flags = ReplayStep->Flush;
	const Vec2i pos = { ReplayStep->PosX, ReplayStep->PosY};
	const int arg1 = ReplayStep->PosX;
	const int arg2 = ReplayStep->PosY;
	CUnit *dunit = (ReplayStep->DestUnitNumber != -1 ? UnitSlots[ReplayStep->DestUnitNumber] : NoUnitP);
	const char *val = ReplayStep->Value.c_str();
	const int num = ReplayStep->Num;

	Assert(unit == -1 || ReplayStep->UnitIdent == UnitSlots[unit]->Type->Ident);

	if (SyncRandSeed != ReplayStep->SyncRandSeed) {
#ifdef DEBUG
		if (!ReplayStep->SyncRandSeed) {
			// Replay without the 'sync info
			ThisPlayer->Notify(NotifyYellow, -1, -1, _("No sync info for this replay !"));
		} else {
			ThisPlayer->Notify(NotifyYellow, -1, -1, _("Replay got out of sync (%lu) !"), GameCycle);
			DebugPrint("OUT OF SYNC %u != %u\n" _C_ SyncRandSeed _C_ ReplayStep->SyncRandSeed);
			DebugPrint("OUT OF SYNC GameCycle %lu \n" _C_ GameCycle);
			Assert(0);
			// ReplayStep = 0;
			// NextLogCycle = ~0UL;
			// return;
		}
#else
		ThisPlayer->Notify(NotifyYellow, -1, -1, _("Replay got out of sync !"));
		ReplayStep = 0;
		NextLogCycle = ~0UL;
		return;
#endif
	}

	if (!strcmp(action, "stop")) {
		SendCommandStopUnit(*UnitSlots[unit]);
	} else if (!strcmp(action, "stand-ground")) {
		SendCommandStandGround(*UnitSlots[unit], flags);
	} else if (!strcmp(action, "follow")) {
		SendCommandFollow(*UnitSlots[unit], *dunit, flags);
	} else if (!strcmp(action, "move")) {
		SendCommandMove(*UnitSlots[unit], pos, flags);
	} else if (!strcmp(action, "repair")) {
		SendCommandRepair(*UnitSlots[unit], pos, dunit, flags);
	} else if (!strcmp(action, "auto-repair")) {
		SendCommandAutoRepair(*UnitSlots[unit], arg1);
	} else if (!strcmp(action, "attack")) {
		SendCommandAttack(*UnitSlots[unit], pos, dunit, flags);
	} else if (!strcmp(action, "attack-ground")) {
		SendCommandAttackGround(*UnitSlots[unit], pos, flags);
	} else if (!strcmp(action, "patrol")) {
		SendCommandPatrol(*UnitSlots[unit], pos, flags);
	} else if (!strcmp(action, "board")) {
		SendCommandBoard(*UnitSlots[unit], *dunit, flags);
	} else if (!strcmp(action, "unload")) {
		SendCommandUnload(*UnitSlots[unit], pos, dunit, flags);
	} else if (!strcmp(action, "build")) {
		SendCommandBuildBuilding(*UnitSlots[unit], pos, *UnitTypeByIdent(val), flags);
	} else if (!strcmp(action, "dismiss")) {
		SendCommandDismiss(*UnitSlots[unit]);
	} else if (!strcmp(action, "resource-loc")) {
		SendCommandResourceLoc(*UnitSlots[unit], pos, flags);
	} else if (!strcmp(action, "resource")) {
		SendCommandResource(*UnitSlots[unit], *dunit, flags);
	} else if (!strcmp(action, "return")) {
		SendCommandReturnGoods(*UnitSlots[unit], dunit, flags);
	} else if (!strcmp(action, "train")) {
		SendCommandTrainUnit(*UnitSlots[unit], *UnitTypeByIdent(val), flags);
	} else if (!strcmp(action, "cancel-train")) {
		SendCommandCancelTraining(*UnitSlots[unit], num, (val && *val) ? UnitTypeByIdent(val) : NULL);
	} else if (!strcmp(action, "upgrade-to")) {
		SendCommandUpgradeTo(*UnitSlots[unit], *UnitTypeByIdent(val), flags);
	} else if (!strcmp(action, "cancel-upgrade-to")) {
		SendCommandCancelUpgradeTo(*UnitSlots[unit]);
	} else if (!strcmp(action, "research")) {
		SendCommandResearch(*UnitSlots[unit], CUpgrade::Get(val), flags);
	} else if (!strcmp(action, "cancel-research")) {
		SendCommandCancelResearch(*UnitSlots[unit]);
	} else if (!strcmp(action, "spell-cast")) {
		SendCommandSpellCast(*UnitSlots[unit], pos, dunit, num, flags);
	} else if (!strcmp(action, "auto-spell-cast")) {
		SendCommandAutoSpellCast(*UnitSlots[unit], num, arg1);
	} else if (!strcmp(action, "diplomacy")) {
		int state;
		if (!strcmp(val, "neutral")) {
			state = DiplomacyNeutral;
		} else if (!strcmp(val, "allied")) {
			state = DiplomacyAllied;
		} else if (!strcmp(val, "enemy")) {
			state = DiplomacyEnemy;
		} else if (!strcmp(val, "crazy")) {
			state = DiplomacyCrazy;
		} else {
			DebugPrint("Invalid diplomacy command: %s" _C_ val);
			state = -1;
		}
		SendCommandDiplomacy(arg1, state, arg2);
	} else if (!strcmp(action, "set-resource")) {
		SendCommandSetResource(arg1, num, arg2);
	} else if (!strcmp(action, "shared-vision")) {
		bool state;
		state = atoi(val) ? true : false;
		SendCommandSharedVision(arg1, state, arg2);
	} else if (!strcmp(action, "input")) {
		if (val[0] == '-') {
			CclCommand(val + 1, false);
		} else {
			HandleCheats(val);
		}
	} else if (!strcmp(action, "quit")) {
		CommandQuit(arg1);
	} else {
		DebugPrint("Invalid action: %s" _C_ action);
	}

	ReplayStep = ReplayStep->Next;
	NextLogCycle = ReplayStep ? (unsigned)ReplayStep->GameCycle : ~0UL;
}

/**
**  Replay user commands from log each cycle
*/
static void ReplayEachCycle()
{
	if (!CurrentReplay) {
		return;
	}
	if (InitReplay) {
		for (int i = 0; i < PlayerMax; ++i) {
			if (!CurrentReplay->Players[i].Name.empty()) {
				Players[i].SetName(CurrentReplay->Players[i].Name);
			}
		}
		ReplayStep = CurrentReplay->Commands;
		NextLogCycle = (ReplayStep ? (unsigned)ReplayStep->GameCycle : ~0UL);
		InitReplay = 0;
	}

	if (!ReplayStep) {
		SetMessage(_("End of replay"));
		GameObserve = false;
		return;
	}

	if (NextLogCycle != ~0UL && NextLogCycle != GameCycle) {
		return;
	}

	do {
		DoNextReplay();
	} while (ReplayStep && (NextLogCycle == ~0UL || NextLogCycle == GameCycle));

	if (!ReplayStep) {
		SetMessage(_("End of replay"));
		GameObserve = false;
	}
}

/**
**  Replay user commands from log each cycle, single player games
*/
void SinglePlayerReplayEachCycle()
{
	if (ReplayGameType == ReplaySinglePlayer) {
		ReplayEachCycle();
	}
}

/**
**  Replay user commands from log each cycle, multiplayer games
*/
void MultiPlayerReplayEachCycle()
{
	if (ReplayGameType == ReplayMultiPlayer) {
		ReplayEachCycle();
	}
}

/**
**  Register Ccl functions with lua
*/
void ReplayCclRegister()
{
	lua_register(Lua, "Log", CclLog);
	lua_register(Lua, "ReplayLog", CclReplayLog);
}

//@}
