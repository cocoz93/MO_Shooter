#include "Weapon.h"

#include "AnimClips.h"
#include "AnimPose.h"

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
			d.reloadRate = 1.5f;
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
			d.meleeAbilitySeconds = 1.14999998f / 1.20000005f - 0.400000006f;	// AM_MM_Pistol_Melee 길이 ÷ 배율 − 빼기(클립 묶음 자산 값) → 34 입력
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
			d.reloadRate = 1.f;
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
			d.meleeAbilitySeconds = 1.06666672f / 1.10000002f - 0.400000006f;	// AM_MM_Rifle_Melee → 35 입력
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
			d.reloadRate = 1.f;
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
			d.meleeAbilitySeconds = 1.16666663f / 1.10000002f - 0.300000012f;	// AM_MM_Shotgun_Melee → 46 입력
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
	}

		// 원점 기준 선분(p + t·d, t 0~1)과 축 Z 캡슐(축 반길이 h, 반지름 r)이 처음 만나는 곳 — Weapon.h 에 내놓음(수류탄 폭발 선이 같이 씀)
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

	namespace
	{
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

		// 구 스윕 ↔ 상자(반길이 ex, ey, ez, 구 반지름 r). 면만 밀어 둔 상자는 변 · 꼭짓점에서 실제보다 넓다.
		// 민 상자의 들어간 점이 변 · 꼭짓점 구역이면 그 변의 캡슐로 다시 잰다 — 움직이는 구와 상자의 정석(Ericson, Real-Time Collision Detection 5.5.7)
		bool SegmentRoundedBox(const Vec3& p, const Vec3& d, double ex, double ey, double ez, double r, double& outT, Vec3& outNormal)
		{
			double t = 0;
			Vec3 n;
			if (!SegmentBox(p, d, ex + r, ey + r, ez + r, t, n)) return false;

			const double pos[3] = { p.x, p.y, p.z };
			const double dir[3] = { d.x, d.y, d.z };
			const double half[3] = { ex, ey, ez };
			double sign[3];
			bool beyond[3];
			int beyondCount = 0;
			for (int i = 0; i < 3; ++i)
			{
				const double at = pos[i] + dir[i] * t;
				sign[i] = at >= 0 ? 1.0 : -1.0;
				beyond[i] = std::fabs(at) > half[i];
				if (beyond[i]) ++beyondCount;
			}
			if (beyondCount <= 1)
			{
				outT = t;
				outNormal = n;
				return true;
			}

			// 두 축이 밖이면 남은 축 방향의 변 하나, 세 축이 다 밖이면 꼭짓점에서 나가는 변 셋
			double best = 2.0;
			Vec3 bestNormal(0, 0, 1);
			for (int axis = 0; axis < 3; ++axis)
			{
				if (beyondCount == 2 && beyond[axis]) continue;
				const int u = (axis + 1) % 3;
				const int v = (axis + 2) % 3;
				const Vec3 edgeStart(pos[u] - sign[u] * half[u], pos[v] - sign[v] * half[v], pos[axis]);
				const Vec3 edgeDir(dir[u], dir[v], dir[axis]);
				double edgeT = 0;
				Vec3 edgeNormal;
				if (!SegmentCapsuleZ(edgeStart, edgeDir, half[axis], r, edgeT, edgeNormal) || edgeT >= best) continue;
				best = edgeT;
				double world[3];
				world[u] = edgeNormal.x;
				world[v] = edgeNormal.y;
				world[axis] = edgeNormal.z;
				bestNormal = Vec3(world[0], world[1], world[2]);
			}
			if (best > 1.0) return false;
			outT = best;
			outNormal = bestNormal;
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

	void CWeaponSpread::SetHeatForCheck(float heat)
	{
		_heat = heat;
		_spreadAngle = _def->heatToSpread.Eval(heat);
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

	// ─── 조준 카메라 섞임 ─────────────────────────────────────

	namespace
	{
		// 카메라 자산 값(원본 기준) — CM_ThirdPerson: BlendTime 0.5 · EaseOut · 지수 4 / CM_ThirdPersonADS: 0.22 · EaseOut · 4
		constexpr float kThirdPersonBlendTime = 0.5f;
		constexpr float kAdsBlendTime = 0.22f;
		constexpr float kCameraBlendExponent = 4.f;

		// 언리얼 FMath::InterpEaseOut(0, 1, alpha, exp) — Lerp(0, 1, x) 는 x 그대로
		float EaseOut01(float alpha, float exponent)
		{
			return 1.f - std::pow(1.f - alpha, exponent);
		}
	}

	void CAimCamera::Reset()
	{
		_modes[MODE_THIRD_PERSON] = Mode{ kThirdPersonBlendTime, kCameraBlendExponent, 0.f, 0.f };
		_modes[MODE_ADS] = Mode{ kAdsBlendTime, kCameraBlendExponent, 0.f, 0.f };
		_count = 0;
		_init = true;
	}

	// ULyraCameraMode::SetBlendWeight — 섞기 함수의 역으로 진행률을 맞춘다(지수의 역수로 같은 EaseOut)
	void CAimCamera::SetWeight(Mode& m, float weight) const
	{
		m.weight = std::clamp(weight, 0.f, 1.f);
		const float invExponent = m.exponent > 0.f ? 1.f / m.exponent : 1.f;
		m.alpha = EaseOut01(m.weight, invExponent);
	}

	// ULyraCameraMode::UpdateBlending
	void CAimCamera::UpdateBlending(Mode& m, float deltaTime) const
	{
		if (m.blendTime > 0.f)
		{
			m.alpha += deltaTime / m.blendTime;
			m.alpha = std::min(m.alpha, 1.f);
		}
		else
		{
			m.alpha = 1.f;
		}
		const float exponent = m.exponent > 0.f ? m.exponent : 1.f;
		m.weight = EaseOut01(m.alpha, exponent);
	}

	// ULyraCameraModeStack::PushCameraMode
	void CAimCamera::Push(EMode mode)
	{
		if (_count > 0 && _stack[0] == mode) return;

		int existing = -1;
		float contribution = 1.f;
		for (int i = 0; i < _count; ++i)
		{
			if (_stack[i] == mode)
			{
				existing = i;
				contribution *= _modes[mode].weight;
				break;
			}
			contribution *= (1.f - _modes[_stack[i]].weight);
		}
		if (existing >= 0)
		{
			for (int i = existing; i + 1 < _count; ++i) _stack[i] = _stack[i + 1];
			--_count;
		}
		else
		{
			contribution = 0.f;
		}

		Mode& m = _modes[mode];
		const bool shouldBlend = m.blendTime > 0.f && _count > 0;
		SetWeight(m, shouldBlend ? contribution : 1.f);

		for (int i = _count; i > 0; --i) _stack[i] = _stack[i - 1];
		_stack[0] = (uint8_t)mode;
		++_count;

		SetWeight(_modes[_stack[_count - 1]], 1.f);	// 맨 끝은 늘 100%
	}

	void CAimCamera::Tick(bool adsActive, float deltaTime)
	{
		if (!_init) Reset();
		Push(adsActive ? MODE_ADS : MODE_THIRD_PERSON);

		// ULyraCameraModeStack::UpdateStack — 다 섞인 모드 아래는 버린다
		for (int i = 0; i < _count; ++i)
		{
			Mode& m = _modes[_stack[i]];
			UpdateBlending(m, deltaTime);
			if (m.weight >= 1.f)
			{
				_count = i + 1;
				break;
			}
		}
	}

	void CAimCamera::SetForCheck(int count, bool topAds, float topWeight)
	{
		Reset();
		if (count <= 0) return;
		const EMode top = topAds ? MODE_ADS : MODE_THIRD_PERSON;
		_stack[0] = (uint8_t)top;
		_count = 1;
		if (count >= 2)
		{
			const EMode bottom = topAds ? MODE_THIRD_PERSON : MODE_ADS;
			_stack[1] = (uint8_t)bottom;
			_count = 2;
			SetWeight(_modes[bottom], 1.f);
		}
		SetWeight(_modes[top], _count >= 2 ? topWeight : 1.f);
	}

	// ULyraCameraModeStack::GetBlendInfo 의 맨 끝 → LyraRangedWeaponInstance.cpp:203
	float CAimCamera::GetAimingAlpha() const
	{
		if (_count == 0) return 0.f;
		const uint8_t bottom = _stack[_count - 1];
		return bottom == MODE_ADS ? _modes[bottom].weight : 0.f;
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

	int32_t CWeaponCycle::TopUpSpare()
	{
		const int32_t add = _def->startSpareAmmo - _spare;
		if (add <= 0) return 0;
		_spare += add;
		return add;
	}

	void CWeaponCycle::StartReload()
	{
		_reloadLeft = _def->reloadSeconds;
	}

	WeaponEvents CWeaponCycle::Tick(float deltaTime, const WeaponInput& input, const WeaponMotion& motion)
	{
		WeaponEvents ev;

		// 근접이 켜지는 입력 — 도는 재장전 능력이 끊긴다(근접의 끊기 태그). 근접 처리가 이 순환보다 먼저다(Weapon.h 주석)
		if (input.meleeStarted && _reloadLeft > 0)
		{
			_reloadLeft = 0;
			ev.reloadCancelledByMelee = true;
		}

		// 입력 처리 — 발사. 재장전보다 먼저다(Weapon.h 주석). 근접이 켜져 있으면 막힌다(근접의 막기 태그)
		if (input.fireHeld && !input.meleeActive && !input.dashActive && !_fireActive && _magazine >= 1)
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

		// 수류탄을 던진 입력 — 도는 재장전을 끊는다(수류탄 끊기 태그). 쥔 사격 뒤 — Lyra 는 쥔 입력 고리를 먼저 돌아
		// 같은 프레임 Q + 사격은 사격이 먼저다(G-0-c). 그 발이 이미 재장전을 끊었으면 수류탄 몫으로 안 센다. 근접과 같은 입력이면 근접이 먼저 센다
		if (input.grenadeThrown && _reloadLeft > 0)
		{
			_reloadLeft = 0;
			ev.reloadCancelledByGrenade = true;
		}

		// 대시가 켜지는 입력 — 켜져 있던 사격(방금 쏜 발 포함)과 도는 재장전을 끊는다(대시의 끊기 태그 Ability.Type.Action). 타이머 앞이라 끊긴 재장전은 탄이 안 들어간다
		const bool dashBlocks = input.dashActive || input.dashStarted;
		if (input.dashStarted)
		{
			if (_fireActive)
			{
				_fireActive = false;
				_fireLeft = 0;
				_fireMontageLeft = 0;
				ev.fireCancelledByDash = true;
			}
			if (_reloadLeft > 0)
			{
				_reloadLeft = 0;
				ev.reloadCancelledByDash = true;
			}
		}

		// 입력 처리 — 재장전. 발사 능력 · 근접 능력 · 대시가 켜져 있으면 막힌다(Ability.ActivateFail.TagsBlocked)
		if (input.reloadPressed && !_fireActive && !input.meleeActive && !dashBlocks && _reloadLeft <= 0 && _magazine < _def->magazineSize && _spare > 0)
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
			if (_magazine == 0 && _spare > 0 && _idleSeconds >= _def->autoReloadIdleSeconds && _reloadLeft <= 0 && !input.meleeActive && !dashBlocks)	// 근접 · 대시 중 검사는 재장전 능력이 막혀 그냥 지나간다(원본 기준)
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

	uint32_t SpreadSeed(uint32_t inputSeq, uint32_t bulletIndex)
	{
		// 입력 번호는 1씩 늘어난다. 그대로 넣으면 이웃 번호의 xorshift 첫 값이 거의 같아 이어 쏜 탄이 비슷한 거리로 벌어진다(이웃 번호 상관 0.9996, 실측 WeaponCheck). 그래서 MurmurHash3 마무리 식으로 섞는다
		uint32_t h = inputSeq * 0x9E3779B1u + bulletIndex;
		h ^= h >> 16;
		h *= 0x85EBCA6Bu;
		h ^= h >> 13;
		h *= 0xC2B2AE35u;
		h ^= h >> 16;
		return h;
	}

	Vec3 AimDirection(float yawDegrees, float pitchDegrees)
	{
		const double yaw = yawDegrees * kPi / 180.0;
		const double pitch = pitchDegrees * kPi / 180.0;
		return Vec3(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch));
	}

	// ─── 몸 판정 ───────────────────────────────────────────

	const BodyShape* GetBodyShapes(size_t& outCount)
	{
		outCount = sizeof(kBodyShapes) / sizeof(kBodyShapes[0]);
		return kBodyShapes;
	}

	bool TraceBody(const BodyPlacement& body, const Vec3& start, const Vec3& end, double radius, BodyHit& out)
	{
		if (body.skip) return false;

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
		// 부위 하나 — 좌표는 캐릭터 요 0 기준
		const auto testShape = [&](uint32_t index, EBodyShape kind, bool weakSpot, const Vec3& center, const Vec3& axisX, const Vec3& axisY, const Vec3& axisZ, double a, double b, double c)
		{
			const Vec3 rel = localStart - center;
			const Vec3 p(Dot(rel, axisX), Dot(rel, axisY), Dot(rel, axisZ));
			const Vec3 d(Dot(localDir, axisX), Dot(localDir, axisY), Dot(localDir, axisZ));

			double t = 0;
			Vec3 n;
			bool hit = false;
			switch (kind)
			{
			case EBodyShape::Sphere:
				hit = SegmentCapsuleZ(p, d, 0.0, a + radius, t, n);
				break;
			case EBodyShape::Sphyl:
				hit = SegmentCapsuleZ(p, d, b * 0.5, a + radius, t, n);
				break;
			case EBodyShape::Box:
				// 자세 판정은 엔진과 같게 둥근 상자로 잰다. 고정 포즈 판정은 Tools/WeaponCheck 기준선이 이 근사로 잡혀 있어 그대로 둔다
				hit = (body.pose && radius > 0) ? SegmentRoundedBox(p, d, a * 0.5, b * 0.5, c * 0.5, radius, t, n)
					: SegmentBox(p, d, a * 0.5 + radius, b * 0.5 + radius, c * 0.5 + radius, t, n);
				break;
			}
			if (hit && t < best)
			{
				best = t;
				found = true;
				out.time = t;
				out.shape = index;
				out.weakSpot = weakSpot;
				const Vec3 worldNormal = toWorld(axisX * n.x + axisY * n.y + axisZ * n.z);
				const Vec3 at = start + (end - start) * t;
				out.point = at - worldNormal * radius;
			}
		};

		if (body.pose)
		{
			for (uint32_t i = 0; i < body.shapeCount; ++i)
			{
				const ClipBody& s = body.shapes[i];
				const BodyPose& pose = body.pose[i];
				testShape(i, (EBodyShape)s.kind, s.weakSpot, Vec3(pose.center[0], pose.center[1], pose.center[2]),
					Vec3(pose.axisX[0], pose.axisX[1], pose.axisX[2]), Vec3(pose.axisY[0], pose.axisY[1], pose.axisY[2]), Vec3(pose.axisZ[0], pose.axisZ[1], pose.axisZ[2]), s.a, s.b, s.c);
			}
			return found;
		}

		for (uint32_t i = 0; i < sizeof(kBodyShapes) / sizeof(kBodyShapes[0]); ++i)
		{
			const BodyShape& s = kBodyShapes[i];
			testShape(i, s.kind, s.weakSpot, Vec3(s.center[0], s.center[1], s.center[2]),
				Vec3(s.axisX[0], s.axisX[1], s.axisX[2]), Vec3(s.axisY[0], s.axisY[1], s.axisY[2]), Vec3(s.axisZ[0], s.axisZ[1], s.axisZ[2]), s.a, s.b, s.c);
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

	// ─── 근접 ──────────────────────────────────────────────

	void ResolveMelee(const CMapCollision& map, const MeleeTarget* targets, size_t targetCount, const Vec3& hand, const Vec3& forward, MeleeResult& out)
	{
		out = MeleeResult();
		const Vec3 delta = forward * kMeleeDistance;
		const Vec3 end = hand + delta;
		double best = 2.0;
		bool found = false;
		for (size_t i = 0; i < targetCount; ++i)
		{
			const MeleeTarget& target = targets[i];
			// 메시 몸체 — 사격의 구 판정과 같은 길(몸체를 구 반지름만큼 부풀려 선분으로, 상자는 둥근 상자)
			BodyHit bodyHit;
			if (TraceBody(target.body, hand, end, kMeleeRadius, bodyHit) && bodyHit.time < best)
			{
				best = bodyHit.time;
				found = true;
				out.targetIndex = (uint32_t)i;
				out.capsule = false;
				out.shape = bodyHit.shape;
				out.impactPoint = bodyHit.point;
			}
			// 캡슐 — 세운 캡슐(축 반길이 = 반높이 − 반지름)을 구 반지름만큼 부풀려 선분으로
			double t = 0;
			Vec3 normal;
			const double axisHalf = std::max(target.capsuleHalfHeight - target.capsuleRadius, 0.0);
			if (SegmentCapsuleZ(hand - target.body.center, delta, axisHalf, target.capsuleRadius + kMeleeRadius, t, normal) && t < best)
			{
				best = t;
				found = true;
				out.targetIndex = (uint32_t)i;
				out.capsule = true;
				out.shape = 0;
				out.impactPoint = hand + delta * t - normal * kMeleeRadius;
			}
		}
		if (!found) return;
		out.distance = best * kMeleeDistance;
		out.location = hand + delta * best;
		if (targets[out.targetIndex].sameTeam)
		{
			out.kind = EMeleeHit::SameTeam;
			return;
		}
		RayHit wall;
		out.kind = map.Raycast(hand, out.impactPoint, MAP_BLOCK_WEAPON, wall) ? EMeleeHit::Blocked : EMeleeHit::Hit;
	}

	uint32_t MeleeAbilityInputs(const WeaponDef& def, float tickSeconds)
	{
		const float inputs = std::ceil(def.meleeAbilitySeconds / tickSeconds - 1e-3f);	// 작은 허용치 = float 나눗셈이 정수 바로 위로 넘는 것을 막는다
		return inputs < 1.f ? 1u : (uint32_t)inputs;
	}

	Vec3 MeleePushForce(const Vec3& forward, double hitDistance)
	{
		const double distance = (double)(float)hitDistance;		// FHitResult::Distance
		const float strength = (float)(kMeleePushStrength * ((distance - 0.0) / (kMeleeDistance - 0.0)));	// NormalizeToRange(값, 0, 350) × Strength → 작업 인자 float
		return forward * (double)strength;		// ConstantForce->Force = WorldDirection * Strength
	}

	Vec3 MeleeHandWorld(const Vec3& location, float yawDegrees, const double localHand[3])
	{
		const double yaw = yawDegrees * kPi / 180.0;
		const double cosYaw = std::cos(yaw);
		const double sinYaw = std::sin(yaw);
		return location + Vec3(localHand[0] * cosYaw - localHand[1] * sinYaw, localHand[0] * sinYaw + localHand[1] * cosYaw, localHand[2]);
	}
}
