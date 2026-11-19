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

		// 사람끼리 막기(컨텐츠 F) — Lyra 는 산 사람 캡슐끼리 막고 미끄러뜨린다(F-1 실측). 늘 켠다(D83 · 11-17 사용자 판정 「옵션빼고 기본으로 막자」).
		// 끄기는 시험 전용 — 서버 · 봇 --testNoPawnBlock. 회귀 봇 한 판이 서버 ↔ 같은 코드 위치 차이 0 을 엄격히 보려고 둔다(봇끼리 부딪히면 남 자리를 늦게 알아 갈린다)
		bool pawnBlock = true;

		// 처음 무기(무기 줍기 W-2) — Lyra 는 권총 하나만 준다(영웅 InitialInventoryItems, D97 · W-0 실측). 세 무기로 시작은 시험 전용 —
		// 서버 · 봇 --testStartAllWeapons. 세 무기를 전제로 한 기존 시험이 기준선을 지키게 하려고 둔다(Loadout.h)
		bool startAllWeapons = false;
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
