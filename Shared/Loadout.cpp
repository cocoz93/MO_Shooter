#include "Loadout.h"

namespace mo
{
	const char* GiveResultName(EGiveResult r)
	{
		switch (r)
		{
		case EGiveResult::NewEquipped: return "new";
		case EGiveResult::NewNotEquipped: return "newNoSwitch";
		case EGiveResult::Ammo: return "ammo";
		case EGiveResult::Full: return "full";
		case EGiveResult::NoSlot: return "noSlot";
		}
		return "?";
	}

	void CLoadout::Reset(bool allWeapons, WeaponCycles& cycles)
	{
		_slots = { (uint8_t)EWeapon::Pistol, kNoWeapon, kNoWeapon };
		if (allWeapons) _slots = { (uint8_t)EWeapon::Pistol, (uint8_t)EWeapon::Rifle, (uint8_t)EWeapon::Shotgun };
		_activeSlot = 0;
		_lastInputWeapon = kNoWeapon;
		// 안 가진 무기의 탄도 처음 값으로 둔다 — 주울 때 다시 처음 값으로 넣으므로 보이지 않는다
		for (size_t i = 0; i < cycles.size(); ++i) cycles[i].Init((EWeapon)i);
	}

	bool CLoadout::Owns(uint8_t weapon) const
	{
		return SlotOf(weapon) >= 0;
	}

	int CLoadout::SlotOf(uint8_t weapon) const
	{
		if (weapon == kNoWeapon) return -1;
		for (size_t i = 0; i < _slots.size(); ++i)
		{
			if (_slots[i] == weapon) return (int)i;
		}
		return -1;
	}

	ESwitchResult CLoadout::OnInputWeapon(uint8_t inputWeapon, WeaponCycles& cycles)
	{
		// 범위 밖 번호는 입력마다 거부한다(권위 시험이 보낸 수만큼 센다) — 「앞 입력 번호」로도 안 남긴다
		if (inputWeapon >= (uint8_t)EWeapon::Count) return ESwitchResult::Invalid;
		if (inputWeapon == _lastInputWeapon) return ESwitchResult::Same;
		_lastInputWeapon = inputWeapon;
		const int slot = SlotOf(inputWeapon);
		if (slot < 0) return ESwitchResult::NotOwned;
		if (slot == (int)_activeSlot) return ESwitchResult::AlreadyHeld;
		_activeSlot = (uint8_t)slot;
		cycles[inputWeapon].Equip();
		return ESwitchResult::Switched;
	}

	uint8_t CLoadout::PackSlots() const
	{
		uint8_t packed = 0;
		for (size_t i = 0; i < _slots.size(); ++i)
		{
			const uint8_t code = _slots[i] == kNoWeapon ? 0 : (uint8_t)(_slots[i] + 1);
			packed |= (uint8_t)(code << (2 * i));
		}
		return packed;
	}

	uint8_t CLoadout::UnpackSlot(uint8_t packed, size_t slot)
	{
		const uint8_t code = (uint8_t)((packed >> (2 * slot)) & 0x3);
		return code == 0 ? kNoWeapon : (uint8_t)(code - 1);
	}

	void CLoadout::ForceGive(uint8_t weapon, size_t slot, bool hold)
	{
		if (slot >= _slots.size() || weapon >= (uint8_t)EWeapon::Count) return;
		_slots[slot] = weapon;
		if (hold) _activeSlot = (uint8_t)slot;
	}

	EGiveResult CLoadout::Give(EWeapon weapon, WeaponCycles& cycles)
	{
		const uint8_t w = (uint8_t)weapon;
		if (Owns(w))
		{
			// GiveAmmoForDuplicatedWeapons — 「기본 예비 − 지금 예비 > 0」 일 때만 그만큼 더한다(탄창은 그대로)
			return cycles[w].TopUpSpare() > 0 ? EGiveResult::Ammo : EGiveResult::Full;
		}
		// GetNextFreeItemSlot — 비어 있는 첫 칸
		int freeSlot = -1;
		for (size_t i = 0; i < _slots.size() && freeSlot < 0; ++i)
		{
			if (_slots[i] == kNoWeapon) freeSlot = (int)i;
		}
		if (freeSlot < 0) return EGiveResult::NoSlot;
		// 든 무기가 재장전 중인지는 넣기 전에 본다 — 넣는 무기는 아직 안 들었다
		const bool reloading = cycles[Held()].IsReloading();
		_slots[(size_t)freeSlot] = w;
		cycles[w].Init(weapon);		// 새 아이템 — InventoryFragment_SetStats 의 탄창 · 예비
		if (reloading) return EGiveResult::NewNotEquipped;	// IgnorePickUpWhenReload
		_activeSlot = (uint8_t)freeSlot;	// SetActiveSlotIndex — 꺼내기는 Init 이 이미 했다(Init 이 Equip 을 부른다)
		return EGiveResult::NewEquipped;
	}
}
