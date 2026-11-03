#pragma once

#include "GameProtocol.h"

#include <cstdio>
#include <string>

// 점수 기록 한 줄 — 서버가 파일에 적는 것과 봇이 받아 적는 것을 바이트로 비교한다(플랜 2단계 완료 기준).
namespace mo
{
	// 방 1 은 준 이름 그대로, 나머지는 뒤에 .room<번호>
	inline std::string RoomRecordPath(const std::string& base, uint32_t roomId)
	{
		return roomId == 1 ? base : base + ".room" + std::to_string(roomId);
	}

	inline std::string KillRecordLine(const wire::Kill& kill)
	{
		char line[128];
		std::snprintf(line, sizeof(line), "KILL,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", kill.tick, kill.attacker, kill.target,
			kill.teamScoreA, kill.teamScoreB, kill.attackerKills, kill.targetDeaths, kill.weapon, kill.weakSpot);
		return line;
	}

	inline std::string ResultRecordLines(const wire::Result& result)
	{
		char line[128];
		std::snprintf(line, sizeof(line), "RESULT,%u,%u,%u,%u,%u\n", result.tick, result.targetScore, result.teamScoreA, result.teamScoreB, result.winningTeam);
		std::string text = line;
		for (int i = 0; i < result.count; ++i)
		{
			const wire::PlayerResult& player = result.players[i];
			std::snprintf(line, sizeof(line), "SCORE,%u,%u,%u,%u\n", player.id, player.team, player.kills, player.deaths);
			text += line;
		}
		return text;
	}
}
