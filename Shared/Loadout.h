#pragma once

#include "Weapon.h"

#include <array>
#include <cstddef>
#include <cstdint>

// 가방 — Lyra 빠른 칸(ULyraQuickBarComponent, NumSlots = 3)과 그 칸의 무기(weapon-pickup-plan.md W-2).
// 서버(Room)와 예측(CSelfPrediction)이 이 함수들로만 바꾼다 — 같은 입력 · 같은 받침 결과면 두 쪽 가방이 같다.
// 탄은 무기마다 CWeaponCycle 이 든다. 가방은 「무엇을 가졌고 어느 칸에 있나 · 어느 칸을 들었나」만 든다
namespace mo
{
	constexpr size_t kQuickSlotCount = 3;
	constexpr uint8_t kNoWeapon = 0xFF;		// 빈 칸 · 「앞 입력 번호 없음」

	using WeaponCycles = std::array<CWeaponCycle, (size_t)EWeapon::Count>;
	static_assert((size_t)EWeapon::Count <= 3 && kQuickSlotCount * 2 <= 8, "와이어 빠른 칸(PackSlots)은 칸마다 2비트 한 바이트, 무기 셋 · 칸 넷까지");

	// 입력의 무기 번호를 받은 결과
	enum class ESwitchResult : uint8_t
	{
		Same,			// 앞 입력과 같은 번호 — 아무것도 안 한다(줍기로 서버가 바꾼 무기를 클라의 옛 번호가 되돌리지 않게)
		Switched,		// 바꿨다
		AlreadyHeld,	// 번호가 바뀌었지만 이미 든 무기
		NotOwned,		// 번호가 바뀌었지만 안 가진 무기 — Lyra 영웅 ChangeQuickbarSlot 은 빈 칸이면 신호를 안 보낸다
		Invalid,		// 무기 번호 범위 밖 — 앞 입력과 같아도 입력마다 이것이다
	};

	// 받침이 무기를 준 결과 — B_WeaponSpawner 그래프 GiveWeapon(C:\tmp\mob\pickup0\ws_flow.txt:131-272, 원본 기준 · W-0 실측과 같음)
	enum class EGiveResult : uint8_t
	{
		NewEquipped,		// 없던 무기 — 빈 첫 칸에 넣고(탄창 가득 · 예비 기본값) 바로 들었다
		NewNotEquipped,		// 없던 무기 — 든 무기가 재장전 중(태그 Event.Movement.Reload)이라 넣기만 했다
		Ammo,				// 가진 무기 — 예비를 기본값까지 채웠다(탄창 · 든 무기 그대로)
		Full,				// 가진 무기 — 예비가 이미 기본값 이상. 실패 — 받침이 안 꺼진다
		NoSlot,				// 빈 칸이 없다 — 실패. 무기 셋 · 칸 셋이라 안 생긴다(세기만)
	};
	inline bool IsGiveTaken(EGiveResult r) { return r == EGiveResult::NewEquipped || r == EGiveResult::NewNotEquipped || r == EGiveResult::Ammo; }
	const char* GiveResultName(EGiveResult r);

	class CLoadout
	{
	public:
		// 부활 · 경기 시작 — 영웅 InitialInventoryItems = [ID_Pistol] → [권총, 빈, 빈] · 칸 0 듦(W-0 실측). 세 무기 탄을 처음 값으로.
		// allWeapons = 시험 전용 --testStartAllWeapons(MatchRules) — [권총, 소총, 샷건] · 칸 0 듦(무기 줍기 전 동작)
		void Reset(bool allWeapons, WeaponCycles& cycles);

		// 입력 하나의 무기 번호. 앞 입력과 달라졌고 · 가진 무기이고 · 든 무기와 다를 때만 바꾸고 그 무기를 꺼낸다(Equip).
		// 서버가 채운 빈 입력은 부르지 않는다 — 앞 입력 번호를 바꾸면 무기가 튄다(weapon-pickup-plan.md 「미리 본 위험」)
		ESwitchResult OnInputWeapon(uint8_t inputWeapon, WeaponCycles& cycles);

		// 받침 하나가 이 무기를 준다. 새 무기를 들었으면 Held() 가 바뀐다
		EGiveResult Give(EWeapon weapon, WeaponCycles& cycles);

		uint8_t Held() const { return _slots[_activeSlot]; }		// 든 무기 번호
		uint8_t ActiveSlot() const { return _activeSlot; }
		uint8_t SlotWeapon(size_t slot) const { return _slots[slot]; }
		bool Owns(uint8_t weapon) const;
		int SlotOf(uint8_t weapon) const;		// 없으면 -1

		// 와이어 빠른 칸(PlayerState::quickSlots) — 칸 i 가 비트 2i · 2i+1, 0 = 빈 칸 · 1 + 무기 번호
		uint8_t PackSlots() const;
		static uint8_t UnpackSlot(uint8_t packed, size_t slot);		// 그 칸의 무기 번호. 빈 칸이면 kNoWeapon

		// 서버가 알린 줍기를 그대로 옮긴다 — 예측의 Give 결과가 서버 소식과 다를 때만 쓴다(부르는 쪽이 세고 고친다). 칸에 무기를 넣고 hold 면 그 칸을 든다
		void ForceGive(uint8_t weapon, size_t slot, bool hold);

	private:
		std::array<uint8_t, kQuickSlotCount> _slots = { 0, kNoWeapon, kNoWeapon };
		uint8_t _activeSlot = 0;
		uint8_t _lastInputWeapon = kNoWeapon;
	};
}
