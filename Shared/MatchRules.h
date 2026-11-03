#pragma once

#include "GameProtocol.h"
#include "MapCollision.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

// 경기 규칙 — 값은 Lyra 에셋의 실행값(MOBWeaponDumpSubsystem 이 뽑은 Saved/MOB/weapons_match.txt).
// 서버와 클라가 같이 쓴다 — 클라는 부활 뒤 이동 막음까지 똑같이 돌려야 위치가 맞는다.
namespace mo
{
	struct MatchRules
	{
		double warmupSeconds = 30.0;		// Phase_Warmup.WaitForPlayersDuration
		double respawnSeconds = 5.0;		// GA_AutoRespawn.RespawnDelayDuration
		double postGameSeconds = 10.0;		// Phase_PostGame.NewGameTime
		double spawnImmunitySeconds = 2.75;	// GA_SpawnEffect — SpawnMontage(AM_MM_Pistol_Spawn) 재생 길이만큼 GE_SpawnIn 이 걸린다
		double spawnMoveLockFraction = 0.75;	// GA_SpawnEffect.EnableInputAfterTimeFraction — 이동 입력만 막힌다(태그 규칙이 막는 것은 사망뿐)

		// B_TeamDeathMatchScoring — 경기 전 대기가 끝날 때 캐릭터 수로 정한다
		int bigMatchPlayers = 6;
		int bigMatchTargetScore = 30;
		double bigMatchSeconds = 1200.0;
		int smallMatchTargetScore = 15;
		double smallMatchSeconds = 720.0;

		double fallKillZ = -8000.0;			// 내 결정 — Lyra 의 KillZ 는 -1048575 라 사실상 없다. 맵 맨 아래(-5577)보다 충분히 낮게, 떨어지면 돌아올 길이 없어서
		double maxHealth = 100.0;			// 파일럿에서 체력 신호로 확인
	};

	inline uint32_t SecondsToTicks(double seconds)
	{
		return (uint32_t)std::ceil(seconds * wire::kTickRate);
	}

	// 이동 막음은 틱이 아니라 입력 개수로 센다 — 클라가 같은 입력을 돌려 보면 같은 결과가 나오게
	inline int SpawnMoveLockInputs(const MatchRules& rules)
	{
		return (int)SecondsToTicks(rules.spawnImmunitySeconds * rules.spawnMoveLockFraction);
	}

	struct SpawnPoint
	{
		Vec3 location;
		float yawDegrees;
	};

	// L_Expanse 의 LyraPlayerStart 20곳
	const SpawnPoint* GetExpanseStarts(size_t& outCount);
}
