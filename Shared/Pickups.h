#pragma once

#include "MapCollision.h"

#include <cstddef>
#include <string>
#include <vector>

// 받침(밟으면 집히는 자리) — 값은 리포 밖 파일(서버 --pads=, Tools/LyraPilot/make_pads.py 가 Lyra 실행 기록 · 에셋 덤프에서 만든다).
// Epic 맵 값이라 리포에 넣지 않는다(11-06 라이선스 판단). 지금은 체력 받침만(ALyraWeaponSpawner + B_AbilitySpawner).
// 무기 받침(범위 밖 · 5단계)이 오면 줄에 종류 칸을 더한다.
namespace mo
{
	// S2C_PICKUP 의 받침 번호가 1바이트라서(B-3)
	constexpr size_t kMaxPickupPads = 255;

	struct PickupPad
	{
		std::string name;			// Lyra 액터 이름 — 다리가 이 이름으로 맵의 받침 액터를 찾는다
		Vec3 location;				// 닿음 캡슐 중심 = 액터 위치
		double radius = 0;			// 닿음 캡슐(CollisionVolume)
		double halfHeight = 0;
		double heal = 0;			// 회복량 — 효과 GE_InstantHeal_* 의 BaseHeal
		double respawnSeconds = 0;	// CoolDownTime — 실행값(BeginPlay 가 WeaponPickupData_*.SpawnCoolDownSeconds 로 덮은 값)
		double recheckSeconds = 0;	// CheckExistingOverlapDelay — 다시 켜진 뒤 이미 서 있는 사람을 다시 보는 때
	};

	enum class EPadsLoad
	{
		Ok,
		BadLine,		// 칸 수 · 숫자 · 범위가 틀린 줄
		NoPads,			// PAD 줄이 하나도 없음
		TooMany,		// kMaxPickupPads 넘음
	};

	// 줄 = "PAD 이름 x y z 반지름 반높이 회복량 다시생김초 다시검사초" (# 뒤는 주석, 빈 줄 무시).
	// 틀린 줄이 하나라도 있으면 아무것도 넘기지 않는다 — 받침 하나가 빠진 채 판이 돌면 Lyra 와 다른 맵이 된다.
	// outBadLine = 틀린 줄 번호(1부터, Ok 면 0)
	EPadsLoad ParsePickupPads(const char* text, size_t size, std::vector<PickupPad>& out, size_t& outBadLine);
	const char* PadsLoadName(EPadsLoad result);
}
