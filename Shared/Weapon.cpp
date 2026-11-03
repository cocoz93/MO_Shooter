#include "Weapon.h"

#include <algorithm>
#include <cmath>

namespace mo
{
	namespace
	{
		constexpr float kSmallNumber = 1.e-8f;		// 언리얼 SMALL_NUMBER
		constexpr float kKindaSmallNumber = 1.e-4f;	// 언리얼 KINDA_SMALL_NUMBER
		constexpr double kPi = 3.141592653589793238462643383279502884;

		bool IsNearlyEqual(float a, float b, float tolerance)
		{
			return std::fabs(a - b) <= tolerance;
		}

		// 언리얼 FMath::FInterpTo
		float InterpTo(float current, float target, float deltaTime, float speed)
		{
			if (speed <= 0.f) return target;
			const float dist = target - current;
			if (dist * dist < kSmallNumber) return target;
			const float alpha = std::min(std::max(deltaTime * speed, 0.f), 1.f);
			return current + dist * alpha;
		}

		// 언리얼 FMath::GetMappedRangeValueClamped
		float MapRangeClamped(float inA, float inB, float outA, float outB, float value)
		{
			const float divisor = inB - inA;
			const float pct = divisor == 0.f ? 0.f : (value - inA) / divisor;
			const float t = std::min(std::max(pct, 0.f), 1.f);
			return outA + t * (outB - outA);
		}

		// ─── 수치 표 — 값은 Lyra 실행값(Tools/LyraPilot/record_weapons.ps1 의 TABLE 줄과 대조) ───

		const CurveKey kPistolSpread[] = { { 0.f, 2.5f, ECurveInterp::Linear }, { 8.f, 12.f, ECurveInterp::Linear } };
		const CurveKey kPistolHeat[] = { { 0.f, 12.f, ECurveInterp::Linear } };
		const CurveKey kPistolCool[] = { { 0.f, 10.f, ECurveInterp::Linear } };
		const CurveKey kPistolFalloff[] = { { 0.f, 1.f, ECurveInterp::Linear }, { 2000.f, 1.f, ECurveInterp::Constant }, { 2001.f, 0.5f, ECurveInterp::Constant }, { 25000.f, 0.5f, ECurveInterp::Linear } };

		const CurveKey kRifleSpread[] = { { 0.f, 2.5f, ECurveInterp::Linear }, { 0.085443f, 3.f, ECurveInterp::Linear }, { 7.f, 8.f, ECurveInterp::Linear }, { 12.f, 15.f, ECurveInterp::Linear } };
		const CurveKey kRifleHeat[] = { { 0.f, 1.f, ECurveInterp::Linear }, { 4.f, 1.f, ECurveInterp::Linear }, { 5.f, 1.25f, ECurveInterp::Linear } };
		const CurveKey kRifleCool[] = { { 0.f, 4.f, ECurveInterp::Linear } };
		const CurveKey kRifleFalloff[] = { { 0.f, 1.f, ECurveInterp::Linear }, { 2800.f, 1.f, ECurveInterp::Linear }, { 2801.f, 0.5f, ECurveInterp::Linear } };

		const CurveKey kShotgunSpread[] = { { 0.f, 6.f, ECurveInterp::Linear }, { 6.f, 9.5f, ECurveInterp::Linear }, { 18.f, 20.f, ECurveInterp::Linear } };
		const CurveKey kShotgunHeat[] = { { 1.f, 6.f, ECurveInterp::Linear } };
		const CurveKey kShotgunCool[] = { { 0.f, 6.f, ECurveInterp::Linear } };
		const CurveKey kShotgunFalloff[] = { { 0.f, 1.f, ECurveInterp::Linear }, { 600.f, 1.f, ECurveInterp::Linear }, { 640.f, 0.7f, ECurveInterp::Linear }, { 2000.f, 0.7f, ECurveInterp::Linear }, { 2001.f, 0.5f, ECurveInterp::Linear } };

		template <size_t N>
		Curve MakeCurve(const CurveKey(&keys)[N])
		{
			Curve c;
			c.keys = keys;
			c.count = (uint32_t)N;
			return c;
		}

		WeaponDef MakePistol()
		{
			WeaponDef d{};
			d.name = "pistol";
			d.baseDamage = 18.f;
			d.weakSpotMultiplier = 2.f;
			d.fireDelay = 0.12f;
			d.autoRate = 1.f;
			d.magazineSize = 12;
			d.startSpareAmmo = 48;
			d.bulletsPerCartridge = 1;
			d.maxDamageRange = 25000.f;
			d.spreadExponent = 1.f;
			d.sweepRadius = 6.f;
			d.firstShotAccuracy = true;
			d.aimingMultiplier = 0.65f;
			d.standingStillMultiplier = 0.9f;
			d.standingStillRate = 6.f;
			d.standingStillSpeedThreshold = 80.f;
			d.standingStillSpeedRange = 20.f;
			d.crouchingMultiplier = 0.65f;
			d.crouchingRate = 5.f;
			d.jumpFallMultiplier = 1.25f;
			d.jumpFallRate = 5.f;
			d.recoveryCooldownDelay = 0.2f;
			d.autoReloadPollSeconds = 0.25f;
			d.autoReloadIdleSeconds = 0.66f;
			d.fireMontageSeconds = 0.666667f / 1.f;
			d.reloadSeconds = 1.631531f / 1.500000f;
			d.heatToSpread = MakeCurve(kPistolSpread);
			d.heatToHeatPerShot = MakeCurve(kPistolHeat);
			d.heatToCooldownPerSecond = MakeCurve(kPistolCool);
			d.damageFalloff = MakeCurve(kPistolFalloff);
			return d;
		}

		WeaponDef MakeRifle()
		{
			WeaponDef d{};
			d.name = "rifle";
			d.baseDamage = 12.f;
			d.weakSpotMultiplier = 1.5f;
			d.fireDelay = 0.12f;
			d.autoRate = 1.f;
			d.magazineSize = 30;
			d.startSpareAmmo = 60;
			d.bulletsPerCartridge = 1;
			d.maxDamageRange = 25000.f;
			d.spreadExponent = 0.8f;
			d.sweepRadius = 5.5f;
			d.firstShotAccuracy = true;
			d.aimingMultiplier = 0.65f;
			d.standingStillMultiplier = 0.8f;
			d.standingStillRate = 5.f;
			d.standingStillSpeedThreshold = 20.f;
			d.standingStillSpeedRange = 20.f;
			d.crouchingMultiplier = 0.6f;
			d.crouchingRate = 5.f;
			d.jumpFallMultiplier = 1.6f;
			d.jumpFallRate = 5.f;
			d.recoveryCooldownDelay = 0.15f;
			d.autoReloadPollSeconds = 0.25f;
			d.autoReloadIdleSeconds = 0.66f;
			d.fireMontageSeconds = 0.533f / 1.f;
			d.reloadSeconds = 1.74658f;
			d.heatToSpread = MakeCurve(kRifleSpread);
			d.heatToHeatPerShot = MakeCurve(kRifleHeat);
			d.heatToCooldownPerSecond = MakeCurve(kRifleCool);
			d.damageFalloff = MakeCurve(kRifleFalloff);
			return d;
		}

		WeaponDef MakeShotgun()
		{
			WeaponDef d{};
			d.name = "shotgun";
			d.baseDamage = 12.f;
			d.weakSpotMultiplier = 1.75f;
			d.fireDelay = 0.5f;
			d.autoRate = 2.2f;
			d.magazineSize = 8;
			d.startSpareAmmo = 16;
			d.bulletsPerCartridge = 9;
			d.maxDamageRange = 25000.f;
			d.spreadExponent = 1.f;
			d.sweepRadius = 0.5f;
			d.firstShotAccuracy = false;
			d.aimingMultiplier = 0.8f;
			d.standingStillMultiplier = 0.9f;
			d.standingStillRate = 2.2f;
			d.standingStillSpeedThreshold = 80.f;
			d.standingStillSpeedRange = 20.f;
			d.crouchingMultiplier = 0.8f;
			d.crouchingRate = 5.f;
			d.jumpFallMultiplier = 1.75f;
			d.jumpFallRate = 2.5f;
			d.recoveryCooldownDelay = 0.5f;
			d.autoReloadPollSeconds = 0.25f;
			d.autoReloadIdleSeconds = 0.66f;
			d.fireMontageSeconds = 0.666667f / 2.2f;
			d.reloadSeconds = 2.243124f;
			d.heatToSpread = MakeCurve(kShotgunSpread);
			d.heatToHeatPerShot = MakeCurve(kShotgunHeat);
			d.heatToCooldownPerSecond = MakeCurve(kShotgunCool);
			d.damageFalloff = MakeCurve(kShotgunFalloff);
			return d;
		}

		const WeaponDef kWeapons[] = { MakePistol(), MakeRifle(), MakeShotgun() };

#include "BodyPose.inl"

		double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		Vec3 Cross(const Vec3& a, const Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
		double Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }

		// 원점 기준 선분(p + t·d, t 0~1)과 축 Z 캡슐(축 반길이 h, 반지름 r)이 처음 만나는 곳
		bool SegmentCapsuleZ(const Vec3& p, const Vec3& d, double h, double r, double& outT, Vec3& outNormal)
		{
			const auto spherePart = [&](double cz, double& t, Vec3& n)
			{
				const Vec3 m(p.x, p.y, p.z - cz);
				const double a = Dot(d, d);
				const double b = Dot(m, d);
				const double c = Dot(m, m) - r * r;
				if (a <= 0) return false;
				const double disc = b * b - a * c;
				if (disc < 0) return false;
				const double hit = (-b - std::sqrt(disc)) / a;
				t = hit;
				const Vec3 at = m + d * hit;
				const double len = Length(at);
				n = len > 0 ? at * (1.0 / len) : Vec3(0, 0, 1);
				return true;
			};

			double best = 2.0;
			Vec3 bestNormal(0, 0, 1);

			// 옆면
			const double a = d.x * d.x + d.y * d.y;
			if (a > 1e-20)
			{
				const double b = p.x * d.x + p.y * d.y;
				const double c = p.x * p.x + p.y * p.y - r * r;
				const double disc = b * b - a * c;
				if (disc >= 0)
				{
					const double t = (-b - std::sqrt(disc)) / a;
					const double z = p.z + d.z * t;
					if (std::fabs(z) <= h && t < best)
					{
						const Vec3 at(p.x + d.x * t, p.y + d.y * t, 0);
						const double len = Length(at);
						best = t;
						bestNormal = len > 0 ? at * (1.0 / len) : Vec3(1, 0, 0);
					}
				}
			}

			// 양 끝 반구 — 옆면과 맞닿는 바깥쪽 절반만
			for (const double cz : { h, -h })
			{
				double t;
				Vec3 n;
				if (!spherePart(cz, t, n)) continue;
				const double z = p.z + d.z * t;
				const bool outerPart = cz > 0 ? z >= h : z <= -h;
				if (outerPart && t < best)
				{
					best = t;
					bestNormal = n;
				}
			}

			// 처음부터 안에 있다
			const double clampedZ = std::min(std::max(p.z, -h), h);
			const Vec3 fromAxis(p.x, p.y, p.z - clampedZ);
			if (Dot(fromAxis, fromAxis) <= r * r)
			{
				outT = 0;
				const double len = Length(fromAxis);
				outNormal = len > 0 ? fromAxis * (1.0 / len) : Vec3(0, 0, 1);
				return true;
			}

			if (best < 0 || best > 1) return false;
			outT = best;
			outNormal = bestNormal;
			return true;
		}

		// 원점 기준 선분과 축 정렬 상자(반길이 ex, ey, ez)
		bool SegmentBox(const Vec3& p, const Vec3& d, double ex, double ey, double ez, double& outT, Vec3& outNormal)
		{
			double tMin = 0;
			double tMax = 1;
			Vec3 normal(0, 0, 0);
			const double pos[3] = { p.x, p.y, p.z };
			const double dir[3] = { d.x, d.y, d.z };
			const double ext[3] = { ex, ey, ez };
			for (int i = 0; i < 3; ++i)
			{
				if (std::fabs(dir[i]) < 1e-20)
				{
					if (pos[i] < -ext[i] || pos[i] > ext[i]) return false;
					continue;
				}
				const double inv = 1.0 / dir[i];
				double t0 = (-ext[i] - pos[i]) * inv;
				double t1 = (ext[i] - pos[i]) * inv;
				double sign = -1;
				if (t0 > t1)
				{
					std::swap(t0, t1);
					sign = 1;
				}
				if (t0 > tMin)
				{
					tMin = t0;
					normal = Vec3(i == 0 ? sign : 0, i == 1 ? sign : 0, i == 2 ? sign : 0);
				}
				tMax = std::min(tMax, t1);
				if (tMin > tMax) return false;
			}
			outT = tMin;
			outNormal = normal;
			return true;
		}
	}

	float Curve::Eval(float time) const
	{
		if (count == 0) return 0.f;
		if (time <= keys[0].time) return keys[0].value;
		if (time >= keys[count - 1].time) return keys[count - 1].value;
		uint32_t right = 1;
		while (right < count && keys[right].time <= time) ++right;
		const CurveKey& k1 = keys[right - 1];
		const CurveKey& k2 = keys[right];
		const float diff = k2.time - k1.time;
		if (diff > 0.f && k1.interp != ECurveInterp::Constant)
		{
			const float alpha = (time - k1.time) / diff;
			return k1.value + alpha * (k2.value - k1.value);
		}
		return k1.value;
	}

	float Curve::MinTime() const { return count > 0 ? keys[0].time : 0.f; }
	float Curve::MaxTime() const { return count > 0 ? keys[count - 1].time : 0.f; }

	float Curve::MinValue() const
	{
		float v = count > 0 ? keys[0].value : 0.f;
		for (uint32_t i = 1; i < count; ++i) v = std::min(v, keys[i].value);
		return v;
	}

	float Curve::MaxValue() const
	{
		float v = count > 0 ? keys[0].value : 0.f;
		for (uint32_t i = 1; i < count; ++i) v = std::max(v, keys[i].value);
		return v;
	}

	const WeaponDef& GetWeaponDef(EWeapon weapon)
	{
		return kWeapons[(size_t)weapon];
	}

	float ComputeDamage(const WeaponDef& def, double distance, bool weakSpot, bool sameTeam)
	{
		if (sameTeam) return 0.f;
		const float attenuation = def.damageFalloff.count > 0 ? def.damageFalloff.Eval((float)distance) : 1.f;
		const float material = weakSpot ? def.weakSpotMultiplier : 1.f;
		return std::max(def.baseDamage * attenuation * material, 0.f);
	}

	// ─── 퍼짐 ─────────────────────────────────────────────

	float CWeaponSpread::ClampHeat(float heat) const
	{
		const float minHeat = std::min(std::min(_def->heatToHeatPerShot.MinTime(), _def->heatToCooldownPerSecond.MinTime()), _def->heatToSpread.MinTime());
		const float maxHeat = std::max(std::max(_def->heatToHeatPerShot.MaxTime(), _def->heatToCooldownPerSecond.MaxTime()), _def->heatToSpread.MaxTime());
		return std::min(std::max(heat, minHeat), maxHeat);
	}

	void CWeaponSpread::OnEquipped(const WeaponDef& def)
	{
		_def = &def;
		const float minHeat = std::min(std::min(def.heatToHeatPerShot.MinTime(), def.heatToCooldownPerSecond.MinTime()), def.heatToSpread.MinTime());
		const float maxHeat = std::max(std::max(def.heatToHeatPerShot.MaxTime(), def.heatToCooldownPerSecond.MaxTime()), def.heatToSpread.MaxTime());
		_heat = (minHeat + maxHeat) * 0.5f;
		_spreadAngle = def.heatToSpread.Eval(_heat);
		_multiplier = 1.f;
		_standing = 1.f;
		_crouching = 1.f;
		_jumpFall = 1.f;
		_firstShot = false;
	}

	void CWeaponSpread::SetForCheck(const WeaponDef& def, float heat, float standingStillMultiplier)
	{
		_def = &def;
		_heat = heat;
		_spreadAngle = def.heatToSpread.Eval(heat);
		_standing = standingStillMultiplier;
		_multiplier = standingStillMultiplier;
		_crouching = 1.f;
		_jumpFall = 1.f;
		_firstShot = false;
	}

	void CWeaponSpread::Tick(float deltaTime, const WeaponMotion& motion)
	{
		const WeaponDef& d = *_def;

		// 언리얼 UpdateSpread 는 「마지막 발사 후 SpreadRecoveryCooldownDelay 가 지났으면」 식힌다고 쓰였지만,
		// 그 시각(LastFireTime, LyraRangedWeaponInstance.h:194)을 적는 코드가 없어 경기 시작 직후를 빼면 늘 식는다. 쏘는 중에도 식는다.
		const float cooldown = d.heatToCooldownPerSecond.Eval(_heat);
		_heat = ClampHeat(_heat - cooldown * deltaTime);
		_spreadAngle = d.heatToSpread.Eval(_heat);
		const bool minSpread = IsNearlyEqual(_spreadAngle, d.heatToSpread.MinValue(), kKindaSmallNumber);

		const float standingTarget = MapRangeClamped(d.standingStillSpeedThreshold, d.standingStillSpeedThreshold + d.standingStillSpeedRange, d.standingStillMultiplier, 1.f, motion.speed);
		_standing = InterpTo(_standing, standingTarget, deltaTime, d.standingStillRate);
		const bool standingAtMin = IsNearlyEqual(_standing, d.standingStillMultiplier, d.standingStillMultiplier * 0.1f);

		const float crouchTarget = motion.crouching ? d.crouchingMultiplier : 1.f;
		_crouching = InterpTo(_crouching, crouchTarget, deltaTime, d.crouchingRate);
		const bool crouchAtTarget = IsNearlyEqual(_crouching, crouchTarget, 0.05f);

		const float jumpTarget = motion.falling ? d.jumpFallMultiplier : 1.f;
		_jumpFall = InterpTo(_jumpFall, jumpTarget, deltaTime, d.jumpFallRate);
		const bool jumpIsOne = IsNearlyEqual(_jumpFall, 1.f, 0.05f);

		const float aiming = MapRangeClamped(0.f, 1.f, 1.f, d.aimingMultiplier, motion.aimingAlpha);
		const bool aimingAtTarget = IsNearlyEqual(aiming, d.aimingMultiplier, kKindaSmallNumber);

		_multiplier = aiming * _standing * _crouching * _jumpFall;

		// 조건에 「조준 중」이 들어 있어 조준이 없으면 첫발 정확은 켜지지 않는다
		_firstShot = d.firstShotAccuracy && standingAtMin && crouchAtTarget && jumpIsOne && aimingAtTarget && minSpread;
	}

	void CWeaponSpread::AddSpread()
	{
		const float heatPerShot = _def->heatToHeatPerShot.Eval(_heat);
		_heat = ClampHeat(_heat + heatPerShot);
		_spreadAngle = _def->heatToSpread.Eval(_heat);
	}

	// ─── 발사 순환 ─────────────────────────────────────────

	void CWeaponCycle::Init(EWeapon weapon)
	{
		Init(GetWeaponDef(weapon));
	}

	void CWeaponCycle::Init(const WeaponDef& def)
	{
		_def = &def;
		_magazine = _def->magazineSize;
		_spare = _def->startSpareAmmo;
		Equip();
	}

	void CWeaponCycle::Equip()
	{
		_spread.OnEquipped(*_def);
		ResetCycleForCheck();
	}

	void CWeaponCycle::StartReload()
	{
		_reloadLeft = _def->reloadSeconds;
	}

	WeaponEvents CWeaponCycle::Tick(float deltaTime, const WeaponInput& input, const WeaponMotion& motion)
	{
		WeaponEvents ev;

		// 입력 처리 — 발사. 재장전보다 먼저다(Weapon.h 주석)
		if (input.fireHeld && !_fireActive && _magazine >= 1)
		{
			--_magazine;
			ev.spreadAngle = _spread.GetSpreadAngle();
			ev.spreadMultiplier = _spread.GetCalculatedMultiplier();
			_spread.AddSpread();
			_fireActive = true;
			_fireLeft = _def->fireDelay;
			_fireMontageLeft = _def->fireMontageSeconds;
			_reloadLeft = 0;	// 진행 중이던 재장전 취소
			ev.fired = true;
		}

		// 입력 처리 — 재장전. 발사 능력이 켜져 있으면 막힌다(Ability.ActivateFail.TagsBlocked)
		if (input.reloadPressed && !_fireActive && _reloadLeft <= 0 && _magazine < _def->magazineSize && _spare > 0)
		{
			StartReload();
			ev.reloadStarted = true;
		}

		_spread.Tick(deltaTime, motion);

		// 타이머 — FireDelay 는 발사한 틱의 다음 틱부터, 몽타주는 발사한 틱부터 준다
		if (_fireActive)
		{
			if (!ev.fired) _fireLeft -= deltaTime;
			_fireMontageLeft -= deltaTime;
			if (_fireLeft <= 0 || _fireMontageLeft <= 0) _fireActive = false;
		}
		if (_reloadLeft > 0)
		{
			_reloadLeft -= deltaTime;
			if (_reloadLeft <= 0)
			{
				const int32_t numToAdd = std::min(_def->magazineSize, _spare + _magazine) - _magazine;
				_magazine += numToAdd;
				_spare -= numToAdd;
				ev.reloaded = true;
			}
		}

		// 자동 재장전 검사 — 탄창이 비었고 여분이 있고 마지막 활동(장착·발사) 후 충분히 지났으면 재장전 입력을 보낸다.
		// _pollLeft 는 주기를 더해 격자를 유지하고, 작은 허용치는 float 누적 오차로 한 틱 밀리는 것을 막는다
		_idleSeconds = ev.fired ? 0.f : _idleSeconds + deltaTime;
		_pollLeft -= deltaTime;
		if (_pollLeft <= 1e-4f)
		{
			_pollLeft += _def->autoReloadPollSeconds;
			if (_magazine == 0 && _spare > 0 && _idleSeconds >= _def->autoReloadIdleSeconds && _reloadLeft <= 0)
			{
				StartReload();
				ev.reloadStarted = true;
			}
		}
		return ev;
	}

	// ─── 난수와 퍼짐 방향 ──────────────────────────────────

	float CSpreadRng::NextFloat()
	{
		// xorshift32
		uint32_t x = _state;
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		_state = x;
		return (float)((x >> 8) * (1.0 / 16777216.0));
	}

	Vec3 SpreadDirection(const Vec3& aimDir, float spreadAngleDegrees, float multiplier, float exponent, CSpreadRng& rng)
	{
		const float halfDegrees = spreadAngleDegrees * multiplier * 0.5f;
		const double aimLen = Length(aimDir);
		const Vec3 forward = aimLen > 0 ? aimDir * (1.0 / aimLen) : Vec3(1, 0, 0);
		if (halfDegrees <= 0.f) return forward;

		const float fromCenter = std::pow(rng.NextFloat(), exponent);
		const double angleFromCenter = (double)fromCenter * halfDegrees * kPi / 180.0;
		const double angleAround = (double)rng.NextFloat() * 2.0 * kPi;

		// 언리얼 DirQuat * AroundQuat(롤) * FromCenterQuat(요) 를 전방 벡터에 적용한 것과 같다
		// = 조준 방향을 전방으로 하는 좌표계(오른쪽·위)에서 (cos A, sin A·cos R, sin A·sin R)
		const Vec3 worldUp(0, 0, 1);
		Vec3 right = Cross(worldUp, forward);
		double rightLen = Length(right);
		if (rightLen < 1e-9)
		{
			right = Vec3(0, 1, 0);	// 똑바로 위·아래를 볼 때 — 언리얼 Rotation() 은 요 0 으로 친다
			rightLen = 1;
		}
		right = right * (1.0 / rightLen);
		const Vec3 up = Cross(forward, right);
		const double sinA = std::sin(angleFromCenter);
		return forward * std::cos(angleFromCenter) + right * (sinA * std::cos(angleAround)) + up * (sinA * std::sin(angleAround));
	}

	// ─── 몸 판정 ───────────────────────────────────────────

	const BodyShape* GetBodyShapes(size_t& outCount)
	{
		outCount = sizeof(kBodyShapes) / sizeof(kBodyShapes[0]);
		return kBodyShapes;
	}

	bool TraceBody(const BodyPlacement& body, const Vec3& start, const Vec3& end, double radius, BodyHit& out)
	{
		const double yaw = body.yawDegrees * kPi / 180.0;
		const double cosYaw = std::cos(yaw);
		const double sinYaw = std::sin(yaw);

		// 세계 → 캐릭터 요 0 좌표
		const auto toLocal = [&](const Vec3& v) { return Vec3(v.x * cosYaw + v.y * sinYaw, -v.x * sinYaw + v.y * cosYaw, v.z); };
		const auto toWorld = [&](const Vec3& v) { return Vec3(v.x * cosYaw - v.y * sinYaw, v.x * sinYaw + v.y * cosYaw, v.z); };

		const Vec3 localStart = toLocal(start - body.center);
		const Vec3 localDir = toLocal(end - start);

		bool found = false;
		double best = 2.0;
		for (uint32_t i = 0; i < sizeof(kBodyShapes) / sizeof(kBodyShapes[0]); ++i)
		{
			const BodyShape& s = kBodyShapes[i];
			const Vec3 axisX(s.axisX[0], s.axisX[1], s.axisX[2]);
			const Vec3 axisY(s.axisY[0], s.axisY[1], s.axisY[2]);
			const Vec3 axisZ(s.axisZ[0], s.axisZ[1], s.axisZ[2]);
			const Vec3 rel = localStart - Vec3(s.center[0], s.center[1], s.center[2]);
			const Vec3 p(Dot(rel, axisX), Dot(rel, axisY), Dot(rel, axisZ));
			const Vec3 d(Dot(localDir, axisX), Dot(localDir, axisY), Dot(localDir, axisZ));

			double t = 0;
			Vec3 n;
			bool hit = false;
			switch (s.kind)
			{
			case EBodyShape::Sphere:
				hit = SegmentCapsuleZ(p, d, 0.0, (double)s.a + radius, t, n);
				break;
			case EBodyShape::Sphyl:
				hit = SegmentCapsuleZ(p, d, (double)s.b * 0.5, (double)s.a + radius, t, n);
				break;
			case EBodyShape::Box:
				hit = SegmentBox(p, d, (double)s.a * 0.5 + radius, (double)s.b * 0.5 + radius, (double)s.c * 0.5 + radius, t, n);
				break;
			}
			if (hit && t < best)
			{
				best = t;
				found = true;
				out.time = t;
				out.shape = i;
				out.weakSpot = s.weakSpot;
				const Vec3 worldNormal = toWorld(axisX * n.x + axisY * n.y + axisZ * n.z);
				const Vec3 at = start + (end - start) * t;
				out.point = at - worldNormal * radius;
			}
		}
		return found;
	}

	// ─── 한 발 ─────────────────────────────────────────────

	void ResolveBullet(const CMapCollision& map, const BodyPlacement* bodies, size_t bodyCount, const Vec3& start, const Vec3& end, double sweepRadius, BulletResult& out)
	{
		out = BulletResult();
		const double length = Length(end - start);

		const auto firstBody = [&](double radius, double& inOutTime, BulletResult& result)
		{
			bool found = false;
			for (size_t i = 0; i < bodyCount; ++i)
			{
				BodyHit hit;
				if (TraceBody(bodies[i], start, end, radius, hit) && hit.time < inOutTime)
				{
					inOutTime = hit.time;
					found = true;
					result.kind = EBulletHit::Body;
					result.point = hit.point;
					result.bodyIndex = (uint32_t)i;
					result.shape = hit.shape;
					result.weakSpot = hit.weakSpot;
					result.distance = Length(hit.point - start);
				}
			}
			return found;
		};

		// 선
		RayHit ray;
		const bool mapHit = map.Raycast(start, end, MAP_BLOCK_WEAPON, ray);
		const double mapTime = mapHit ? (length > 0 ? ray.distance / length : 0) : 2.0;
		BulletResult lineResult;
		double lineTime = mapTime;
		if (firstBody(0.0, lineTime, lineResult))
		{
			out = lineResult;
			return;
		}
		if (mapHit)
		{
			lineResult.kind = EBulletHit::Map;
			lineResult.distance = ray.distance;
			lineResult.point = start + (end - start) * mapTime;
		}

		// 구 — 선에서 몸을 못 맞혔을 때만
		if (sweepRadius > 0)
		{
			CapsuleHit sweep;
			const bool mapSweep = map.SweepCapsule(start, end, sweepRadius, sweepRadius, MAP_BLOCK_WEAPON, sweep);
			double sweepTime = mapSweep ? sweep.time : 2.0;
			BulletResult sweepResult;
			if (firstBody(sweepRadius, sweepTime, sweepResult))
			{
				sweepResult.usedSweep = true;
				out = sweepResult;
				return;
			}
		}
		out = lineResult;
	}
}
