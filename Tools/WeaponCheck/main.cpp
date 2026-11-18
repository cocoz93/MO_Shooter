// 무기 대조 도구 — Lyra 기록(weaponrec_*.csv)과 Shared/Weapon 을 맞춰 본다.
//
//   WeaponCheck <weaponrec.csv> <map.mocl>
//
// 기록 줄 형식(쓰는 쪽 = Lyra 플러그인 MOBWeaponRecordSubsystem):
//   TABLE,무기,키,값            실행 파일 안의 무기 값
//   CURVE,무기,곡선,시간,값      엔진 곡선 계산 결과
//   DMG,무기,거리,부위,팀,피해   같은 거리·부위 피해(체력 차)
//   FRAME,무기,구간,프레임,시각,dt,발사,재장전,점프,탄창,여분,열,퍼짐,배수,속도,낙하,첫발   프레임 끝 상태. 입력은 다음 프레임에 적용된다
//         …,사격 능력,재장전 능력,조준 칸 6개,앉음(26번째 칸 — 이동 컴포넌트 IsCrouching = 무기가 읽는 값. 11-18 기록부터)
//   HIT,무기,프레임,뼈,재질,시작xyz,맞은점xyz,거리,내위치xyz   실제 맞힘
//   HPDELTA,무기,프레임,체력차
//   SHAPEBASE / SHAPE            몸 모양(Shared/BodyPose.inl 원본)
//   BOT,상태,프레임,xyz,요,속도xyz,낙하
//   RAY,상태,프레임,반지름,시작xyz,끝xyz,맞은수,종류(0없음1맵2적),뼈,약점,거리,막힘
//
// 값을 일부러 틀리는 시험: 환경 변수 WEAPONCHECK_BREAK = base / weak / delay / cool / reload 에 WEAPONCHECK_BREAK_SIGN = + 또는 -
// 끝 코드: 0 = 전부 통과, 1 = 불일치, 2 = 입력 오류
#include "MapCollision.h"
#include "Weapon.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace mo;

namespace
{
	constexpr int kWeaponCount = (int)EWeapon::Count;

	std::vector<std::string> Split(const std::string& line)
	{
		std::vector<std::string> out;
		std::stringstream ss(line);
		std::string item;
		while (std::getline(ss, item, ',')) out.push_back(item);
		return out;
	}

	double D(const std::string& s) { return std::atof(s.c_str()); }
	int I(const std::string& s) { return std::atoi(s.c_str()); }

	int WeaponIndex(const std::string& name)
	{
		for (int i = 0; i < kWeaponCount; ++i)
		{
			if (name == GetWeaponDef((EWeapon)i).name) return i;
		}
		return -1;
	}

	// 일부러 틀릴 때 고칠 수 있게 표를 복사해 둔다
	struct LocalDefs
	{
		WeaponDef defs[kWeaponCount];
		std::vector<CurveKey> coolKeys[kWeaponCount];

		LocalDefs()
		{
			for (int i = 0; i < kWeaponCount; ++i) defs[i] = GetWeaponDef((EWeapon)i);
			const char* what = std::getenv("WEAPONCHECK_BREAK");
			if (!what) return;
			const char* signText = std::getenv("WEAPONCHECK_BREAK_SIGN");
			const float sign = signText && signText[0] == '-' ? -1.f : 1.f;
			for (int i = 0; i < kWeaponCount; ++i)
			{
				WeaponDef& d = defs[i];
				if (!std::strcmp(what, "base")) d.baseDamage += sign * 0.5f;
				else if (!std::strcmp(what, "weak")) d.weakSpotMultiplier += sign * 0.05f;
				else if (!std::strcmp(what, "delay")) d.fireDelay += sign * 0.03f;
				else if (!std::strcmp(what, "reload")) d.reloadSeconds += sign * 0.1f;
				else if (!std::strcmp(what, "montage")) d.fireMontageSeconds += sign * 0.05f;
				else if (!std::strcmp(what, "poll")) d.autoReloadPollSeconds += sign * 0.05f;
				else if (!std::strcmp(what, "idle")) d.autoReloadIdleSeconds += sign * 0.1f;
				else if (!std::strcmp(what, "cool"))
				{
					coolKeys[i].assign(d.heatToCooldownPerSecond.keys, d.heatToCooldownPerSecond.keys + d.heatToCooldownPerSecond.count);
					for (CurveKey& k : coolKeys[i]) k.value *= 1.f + sign * 0.1f;
					d.heatToCooldownPerSecond.keys = coolKeys[i].data();
				}
			}
			std::printf("BREAK %s %c\n", what, sign > 0 ? '+' : '-');
		}
	};

	struct FrameRow
	{
		double now, dt;
		int fire, reload, jump, mag, spare;
		float heat, spread, mult, speed;
		int falling, first;
		int fireActive, reloadActive;
		// 조준 칸(-Ads 기록부터) — 조준 누름(다음 프레임에 먹는다) · 사격이 읽는 조준 비율 · 조준 표식 수 · 카메라 스택(모드 수 · 맨 앞이 조준인가 · 맨 앞 비중)
		bool hasAds = false;
		int ads = 0, adsTag = 0, camCount = 0, topAds = 0;
		float aimAlpha = 0, topWeight = 1;
		bool hasCrouch = false;
		int crouched = 0;		// 앉음 칸(-Crouch 기록부터 · 칸이 없는 옛 기록은 0 = 앉기 없음)
	};

	struct EquipRow
	{
		int weapon;
		double time;
	};

	struct Segment
	{
		int weapon = 0;
		int seg = 0;
		std::vector<FrameRow> rows;
	};

	struct HitRow { int weapon, frame; std::string bone, mat; Vec3 start, impact, me; double dist; };
	struct HpRow { int weapon, frame; double delta; };
	struct BotRow { Vec3 pos; double yaw; };
	struct RayRow { int state, frame; double radius; Vec3 a, b; int numHits, kind; std::string bone; int weak; double dist; int blocking; };

	const char* CurveName(const std::string& key, const WeaponDef& d, const Curve*& out)
	{
		if (key == "HeatToSpreadCurve") { out = &d.heatToSpread; return "spread"; }
		if (key == "HeatToHeatPerShotCurve") { out = &d.heatToHeatPerShot; return "heat"; }
		if (key == "HeatToCoolDownPerSecondCurve") { out = &d.heatToCooldownPerSecond; return "cool"; }
		if (key == "DistanceDamageFalloff") { out = &d.damageFalloff; return "falloff"; }
		out = nullptr;
		return nullptr;
	}

	// TABLE 한 줄을 표와 맞춘다. 표에 없는 키면 false
	bool CheckTableValue(const WeaponDef& d, const std::string& key, const std::string& value, bool& outSame)
	{
		const auto f = [&](float mine) { outSame = std::fabs(mine - (float)D(value)) < 1e-4f; return true; };
		const auto i = [&](int mine) { outSame = mine == I(value); return true; };
		if (key == "BaseDamage") return f(d.baseDamage);
		if (key == "WeakSpotMultiplier") return f(d.weakSpotMultiplier);
		if (key == "BodyMultiplier") return f(1.f);
		if (key == "FireDelayTimeSecs") return f(d.fireDelay);
		if (key == "AutoRate") return f(d.autoRate);
		if (key == "MagazineSize") return i(d.magazineSize);
		if (key == "MagazineAmmo") return i(d.magazineSize);
		if (key == "SpareAmmo") return i(d.startSpareAmmo);
		if (key == "BulletsPerCartridge") return i(d.bulletsPerCartridge);
		if (key == "bAllowFirstShotAccuracy") return i(d.firstShotAccuracy ? 1 : 0);
		if (key == "SpreadExponent") return f(d.spreadExponent);
		if (key == "SpreadRecoveryCooldownDelay") return f(d.recoveryCooldownDelay);
		if (key == "SpreadAngleMultiplier_Aiming") return f(d.aimingMultiplier);
		if (key == "SpreadAngleMultiplier_StandingStill") return f(d.standingStillMultiplier);
		if (key == "TransitionRate_StandingStill") return f(d.standingStillRate);
		if (key == "StandingStillSpeedThreshold") return f(d.standingStillSpeedThreshold);
		if (key == "StandingStillToMovingSpeedRange") return f(d.standingStillSpeedRange);
		if (key == "SpreadAngleMultiplier_Crouching") return f(d.crouchingMultiplier);
		if (key == "TransitionRate_Crouching") return f(d.crouchingRate);
		if (key == "SpreadAngleMultiplier_JumpingOrFalling") return f(d.jumpFallMultiplier);
		if (key == "TransitionRate_JumpingOrFalling") return f(d.jumpFallRate);
		if (key == "MaxDamageRange") return f(d.maxDamageRange);
		if (key == "BulletTraceSweepRadius") return f(d.sweepRadius);

		// 곡선 — 이름.count / .pre / .post / .N.time / .N.value / .N.interp
		const size_t dot = key.find('.');
		if (dot == std::string::npos) return false;
		const Curve* c = nullptr;
		if (!CurveName(key.substr(0, dot), d, c) || !c) return false;
		const std::string rest = key.substr(dot + 1);
		if (rest == "count") return i((int)c->count);
		if (rest == "pre" || rest == "post") return i(4);	// 언리얼 RCCE_Constant — Curve::Eval 이 끝 값을 쓴다는 전제
		const size_t dot2 = rest.find('.');
		if (dot2 == std::string::npos) return false;
		const uint32_t idx = (uint32_t)I(rest.substr(0, dot2));
		const std::string field = rest.substr(dot2 + 1);
		if (idx >= c->count) { outSame = false; return true; }
		if (field == "time") return f(c->keys[idx].time);
		if (field == "value") return f(c->keys[idx].value);
		if (field == "interp") return i(c->keys[idx].interp == ECurveInterp::Constant ? 1 : 0);
		return false;
	}
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::printf("usage: WeaponCheck <weaponrec.csv> <map.mocl>\n");
		return 2;
	}

	std::ifstream in(argv[1]);
	if (!in)
	{
		std::printf("cannot open %s\n", argv[1]);
		return 2;
	}
	std::ifstream mapIn(argv[2], std::ios::binary);
	std::vector<char> mapBytes((std::istreambuf_iterator<char>(mapIn)), std::istreambuf_iterator<char>());
	CMapCollision mapCollision;
	const EMapLoad loaded = mapCollision.LoadFromMemory(mapBytes.data(), mapBytes.size());
	if (loaded != EMapLoad::Ok)
	{
		std::printf("map load failed (%d)\n", (int)loaded);
		return 2;
	}

	LocalDefs local;
	bool failed = false;

	struct TableCount { int total = 0, unknown = 0, diff = 0; } table[kWeaponCount];
	struct CurveCount { int total = 0; double maxDiff = 0; } curve[kWeaponCount];
	struct DamageCount { int total = 0, over = 0, nonZero = 0, allyTotal = 0, allyNonZero = 0; double maxDiff = 0; } damage[kWeaponCount];
	std::vector<Segment> segments;
	std::vector<EquipRow> equips;
	// 표에 없는 값 — 몽타주 길이·재생 속도·재장전 신호 위치. 기록의 INFO 줄
	struct MontageInfo { double fireLength = -1, fireRate = -1, reloadNotify = -1, reloadRate = -1; } montage[kWeaponCount];
	std::vector<std::vector<std::string>> hitLike;
	std::map<std::pair<int, int>, BotRow> bots;
	std::vector<RayRow> rays;

	std::string line;
	while (std::getline(in, line))
	{
		if (!line.empty() && line.back() == '\r') line.pop_back();
		const std::vector<std::string> c = Split(line);
		if (c.empty()) continue;
		const std::string& t = c[0];
		if (t == "TABLE" && c.size() >= 4)
		{
			const int w = WeaponIndex(c[1]);
			if (w < 0 || c[2] == "InstanceClass") continue;
			++table[w].total;
			bool same = false;
			if (!CheckTableValue(local.defs[w], c[2], c[3], same))
			{
				++table[w].unknown;
				std::printf("TABLE unknown %s %s=%s\n", c[1].c_str(), c[2].c_str(), c[3].c_str());
			}
			else if (!same)
			{
				++table[w].diff;
				std::printf("TABLE diff %s %s=%s\n", c[1].c_str(), c[2].c_str(), c[3].c_str());
			}
		}
		else if (t == "CURVE" && c.size() >= 5)
		{
			const int w = WeaponIndex(c[1]);
			const Curve* cv = nullptr;
			if (w < 0 || !CurveName(c[2], local.defs[w], cv) || !cv) continue;
			++curve[w].total;
			curve[w].maxDiff = std::max(curve[w].maxDiff, std::fabs((double)cv->Eval((float)D(c[3])) - D(c[4])));
		}
		else if (t == "DMG" && c.size() >= 6)
		{
			const int w = WeaponIndex(c[1]);
			if (w < 0) continue;
			const bool ally = c[4] == "ally";
			const bool weak = c[3] == "weak";
			const double recorded = D(c[5]);
			const double mine = ComputeDamage(local.defs[w], D(c[2]), weak, ally);
			DamageCount& dc = damage[w];
			++dc.total;
			if (ally)
			{
				++dc.allyTotal;
				if (recorded != 0) ++dc.allyNonZero;
			}
			if (recorded != 0) ++dc.nonZero;
			const double diff = std::fabs(mine - recorded);
			dc.maxDiff = std::max(dc.maxDiff, diff);
			if (diff >= 0.01)
			{
				if (dc.over < 5) std::printf("DMG diff %s dist=%s part=%s team=%s lyra=%.4f mine=%.4f\n", c[1].c_str(), c[2].c_str(), c[3].c_str(), c[4].c_str(), recorded, mine);
				++dc.over;
			}
		}
		else if (t == "EQUIP" && c.size() >= 4)
		{
			equips.push_back({ WeaponIndex(c[1]), D(c[3]) });
		}
		else if (t == "INFO" && c.size() >= 4 && WeaponIndex(c[1]) >= 0)
		{
			MontageInfo& mi = montage[WeaponIndex(c[1])];
			if (c[2] == "fireMontage.length") mi.fireLength = D(c[3]);
			else if (c[2] == "fireMontage.playRate") mi.fireRate = D(c[3]);
			else if (c[2] == "reloadMontage.playRate") mi.reloadRate = D(c[3]);
			else if (c[2] == "reloadMontage.notify" && c[3].rfind("AN_Reload_C@", 0) == 0) mi.reloadNotify = D(c[3].substr(12));
		}
		else if (t == "FRAME" && c.size() >= 19)
		{
			const int w = WeaponIndex(c[1]);
			const int seg = I(c[2]);
			if (segments.empty() || segments.back().weapon != w || segments.back().seg != seg)
			{
				Segment s;
				s.weapon = w;
				s.seg = seg;
				segments.push_back(s);
			}
			FrameRow r;
			r.now = D(c[4]);
			r.dt = D(c[5]);
			r.fire = I(c[6]);
			r.reload = I(c[7]);
			r.jump = I(c[8]);
			r.mag = I(c[9]);
			r.spare = I(c[10]);
			r.heat = (float)D(c[11]);
			r.spread = (float)D(c[12]);
			r.mult = (float)D(c[13]);
			r.speed = (float)D(c[14]);
			r.falling = I(c[15]);
			r.first = I(c[16]);
			r.fireActive = I(c[17]);
			r.reloadActive = I(c[18]);
			if (c.size() >= 25)
			{
				r.hasAds = true;
				r.ads = I(c[19]);
				r.aimAlpha = (float)D(c[20]);
				r.adsTag = I(c[21]);
				r.camCount = I(c[22]);
				r.topAds = I(c[23]);
				r.topWeight = (float)D(c[24]);
			}
			if (c.size() >= 26)
			{
				r.hasCrouch = true;
				r.crouched = I(c[25]);
			}
			segments.back().rows.push_back(r);
		}
		else if ((t == "HIT" && c.size() >= 15) || (t == "HPDELTA" && c.size() >= 4))
		{
			hitLike.push_back(c);
		}
		else if (t == "BOT" && c.size() >= 7)
		{
			BotRow b;
			b.pos = Vec3(D(c[3]), D(c[4]), D(c[5]));
			b.yaw = D(c[6]);
			bots[{ I(c[1]), I(c[2]) }] = b;
		}
		else if (t == "RAY" && c.size() >= 16)
		{
			RayRow r;
			r.state = I(c[1]);
			r.frame = I(c[2]);
			r.radius = D(c[3]);
			r.a = Vec3(D(c[4]), D(c[5]), D(c[6]));
			r.b = Vec3(D(c[7]), D(c[8]), D(c[9]));
			r.numHits = I(c[10]);
			r.kind = I(c[11]);
			r.bone = c[12];
			r.weak = I(c[13]);
			r.dist = D(c[14]);
			r.blocking = I(c[15]);
			rays.push_back(r);
		}
	}

	// ─── 1. 표 ─────────────────────────────────────────────
	for (int w = 0; w < kWeaponCount; ++w)
	{
		const WeaponDef& d = local.defs[w];
		std::printf("TABLE   %-8s items=%d unknown=%d diff=%d | curve samples=%d maxDiff=%.6f\n", d.name, table[w].total, table[w].unknown, table[w].diff, curve[w].total, curve[w].maxDiff);
		if (table[w].total == 0 || table[w].unknown != 0 || table[w].diff != 0 || curve[w].total == 0 || curve[w].maxDiff > 1e-3) failed = true;
	}

	// ─── 2. 피해 표본 ──────────────────────────────────────
	for (int w = 0; w < kWeaponCount; ++w)
	{
		const DamageCount& dc = damage[w];
		std::printf("DAMAGE  %-8s samples=%d nonZero=%d allySamples=%d allyNonZero=%d maxDiff=%.4f over0.01=%d\n", local.defs[w].name, dc.total, dc.nonZero, dc.allyTotal, dc.allyNonZero, dc.maxDiff, dc.over);
		if (dc.total == 0 || dc.nonZero == 0 || dc.over != 0 || dc.allyNonZero != 0) failed = true;
	}

	// ─── 몽타주 값: 표 ↔ 기록의 INFO 줄 ─────────────────────
	for (int w = 0; w < kWeaponCount; ++w)
	{
		const MontageInfo& mi = montage[w];
		const WeaponDef& d = local.defs[w];
		const bool missing = mi.fireLength <= 0 || mi.fireRate <= 0 || mi.reloadNotify <= 0 || mi.reloadRate <= 0;
		const double fireExpected = missing ? 0 : mi.fireLength / mi.fireRate;
		const double reloadExpected = missing ? 0 : mi.reloadNotify / mi.reloadRate;
		const double fireDiff = std::fabs(fireExpected - d.fireMontageSeconds);
		const double reloadDiff = std::fabs(reloadExpected - d.reloadSeconds);
		std::printf("MONTAGE %-8s fire %.6f / %.2f = %.6f (표 %.6f) | reload %.6f / %.2f = %.6f (표 %.6f)\n", d.name, mi.fireLength, mi.fireRate, fireExpected, d.fireMontageSeconds, mi.reloadNotify, mi.reloadRate, reloadExpected, d.reloadSeconds);
		if (missing || fireDiff > 0.00001 || reloadDiff > 0.00001) failed = true;
		// 서버 애니 상태가 몽타주를 트는 속도(9단계 조각 4) — 표의 속도 칸이 기록의 재생 속도와 같아야 한다
		const double rateDiff = std::max(std::fabs(mi.fireRate - d.autoRate), std::fabs(mi.reloadRate - d.reloadRate));
		std::printf("MONTAGERATE %-8s fire 기록 %.4f 표 %.4f | reload 기록 %.4f 표 %.4f\n", d.name, mi.fireRate, d.autoRate, mi.reloadRate, d.reloadRate);
		if (rateDiff > 0.00001) failed = true;
	}

	// ─── 3. 발사 순환 ──────────────────────────────────────
	struct CycleCount
	{
		int frames = 0, lyraShots = 0, simShots = 0, magDiff = 0, spareDiff = 0, firstDiff = 0, lyraReloads = 0, simReloads = 0;
		double heatMax = 0, spreadMax = 0, multMax = 0;
		std::map<int, int> gaps;
		std::map<int, int> simGaps;
		std::map<int, int> reloadFrames;
		int firstBadFrame = -1, firstBadSeg = -1;
		// 조준 — 카메라 스택 이식(CAimCamera) ↔ Lyra 기록. 개수 = 조준 비율이 0 보다 큰 프레임 · 섞이는 중(모드 둘) · 다 섞이기 전에 뗀 횟수
		int aimFrames = 0, aimPositive = 0, aimBlending = 0, aimQuickRelease = 0, aimPress = 0;
		int aimCountDiff = 0, aimTopDiff = 0, aimTagDiff = 0;
		double aimAlphaMax = 0, aimTopWeightMax = 0;
		int aimFirstBadSeg = -1, aimFirstBadFrame = -1;
		int multFirstBadSeg = -1, multFirstBadFrame = -1;	// 퍼짐 배율이 0.01 넘게 어긋난 첫 줄
		// 앉기(앉음 칸이 있는 기록만) — 앉은 프레임 · 앉음 바뀜 · 바뀐 뒤 30프레임(앉기 몫이 FInterpTo 5 로 따라가는 중) · 앉아서 나간 탄. 배율 차는 앉은 프레임 · 바뀐 뒤 30프레임만 따로
		int crouchRows = 0, crouchFrames = 0, crouchToggles = 0, crouchNearToggle = 0, crouchShots = 0;
		double crouchMultMax = 0, crouchNearMultMax = 0;
	} cycle[kWeaponCount];

	// 프레임 안 차례(ads1 기록, 실측): 컨트롤러 입력(조준 능력 켜고 끄기) → 무기 틱(퍼짐) → 기록 도구 → 카메라 갱신.
	// - 근거: 뗀 줄 다음 줄에 조준 표식이 꺼지고(tagDiff 0), 카메라 칸은 그다음 줄에야 3인칭을 넣는다 → 줄마다 적힌 카메라 값은 앞 프레임 갱신 결과다
	// - 그래서 카메라는 앞 줄의 표식(= 앞 프레임 능력 상태)으로 한 번 갱신해 이 줄 카메라 칸과 대조하고, 이 줄 퍼짐은 그 값을 읽는다(0).
	//   1 = 한 프레임 더 늦은 값을 읽는 셈 — 대조가 이 차례를 실제로 가리는지 보려고 남긴다
	int aimLag = 0;
	if (const char* lagText = std::getenv("WEAPONCHECK_AIM_LAG")) aimLag = std::atoi(lagText);

	for (const Segment& s : segments)
	{
		if (s.weapon < 0 || s.rows.size() < 2) continue;
		CycleCount& cc = cycle[s.weapon];
		CWeaponCycle sim;
		sim.Init(local.defs[s.weapon]);
		// 앞 구간에서 넘어온 재장전·발사가 끝난 뒤부터 잰다 — 그 전 일은 이 구간 입력으로 설명되지 않는다
		const bool hasAds = s.rows[0].hasAds;
		size_t startRow = 0;
		while (startRow + 1 < s.rows.size() && (s.rows[startRow].fireActive || s.rows[startRow].reloadActive)) ++startRow;
		const FrameRow& r0 = s.rows[startRow];
		sim.SetAmmoForCheck(r0.mag, r0.spare);
		// 퍼짐 배율의 몫(서 있기 · 공중 · 조준)은 발사와 무관하게 움직인다 → 구간 첫 줄(늘 땅 · 속도 0, ads1 · gym 12구간 전부)에서 몫을 맞추고
		// 대조 시작 줄까지 배율만 굴린 뒤 열을 시작 줄 값으로 덮는다. 시작 줄에서 바로 맞추면 그 줄이 공중일 때 공중 몫을 서 있기 몫으로 넣게 된다(ads1 소총 3구간)
		// 첫 줄 가정 = 공중 몫 1 · 앉기 몫 1(앉기 없음) — 기존과 같다
		const FrameRow& first = s.rows[0];
		CAimCamera cam;
		float firstMult = first.mult;
		if (hasAds)
		{
			cam.SetForCheck(first.camCount, first.topAds != 0, first.topWeight);
			firstMult /= 1.f + (local.defs[s.weapon].aimingMultiplier - 1.f) * std::clamp(first.aimAlpha, 0.f, 1.f);
		}
		CWeaponSpread& warm = sim.SpreadForCheck();
		warm.SetForCheck(local.defs[s.weapon], first.heat, std::max(firstMult, 0.01f));
		for (size_t i = 1; i <= startRow; ++i)
		{
			const FrameRow& prev = s.rows[i - 1];
			const FrameRow& cur = s.rows[i];
			WeaponMotion motion;
			motion.speed = cur.speed;
			motion.falling = cur.falling != 0;
			motion.crouching = cur.crouched != 0;		// 무기 Tick 이 읽는 IsCrouching — 속도 · 낙하와 같은 프레임 끝 값
			const float dt = std::fabs((float)cur.dt - 1.f / 60.f) < 1e-5f ? 1.f / 60.f : (float)cur.dt;
			if (hasAds)
			{
				const float alphaBefore = cam.GetAimingAlpha();
				const float camDt = std::fabs((float)prev.dt - 1.f / 60.f) < 1e-5f ? 1.f / 60.f : (float)prev.dt;
				cam.Tick(prev.adsTag > 0, camDt);
				motion.aimingAlpha = aimLag > 0 ? alphaBefore : cam.GetAimingAlpha();
			}
			warm.Tick(dt, motion);
		}
		warm.SetHeatForCheck(r0.heat);
		double equipAt = -1;
		for (const EquipRow& e : equips)
		{
			if (e.weapon == s.weapon && e.time <= r0.now) equipAt = std::max(equipAt, e.time);
		}
		if (equipAt < 0)
		{
			std::printf("CYCLE   장착 시각 없음: %s seg%d\n", local.defs[s.weapon].name, s.seg);
			failed = true;
			continue;
		}
		const double sinceEquip = r0.now - equipAt;
		const double pollLeft = 0.25 - std::fmod(sinceEquip, 0.25);
		sim.SetAutoReloadPhaseForCheck((float)pollLeft, (float)sinceEquip);

		int lastShot = -1;
		int lastSimShot = -1;
		int lastCrouchToggle = -1000;
		int pendingReload = -1;
		int prevMag = r0.mag;
		for (size_t i = startRow + 1; i < s.rows.size(); ++i)
		{
			const FrameRow& prev = s.rows[i - 1];
			const FrameRow& cur = s.rows[i];
			WeaponInput input;
			input.fireHeld = prev.fire != 0;
			input.reloadPressed = prev.reload != 0;
			WeaponMotion motion;
			motion.speed = cur.speed;
			motion.falling = cur.falling != 0;
			motion.crouching = cur.crouched != 0;		// 무기 Tick 이 읽는 IsCrouching — 속도 · 낙하와 같은 프레임 끝 값
			const float dt = std::fabs((float)cur.dt - 1.f / 60.f) < 1e-5f ? 1.f / 60.f : (float)cur.dt;
			if (hasAds)
			{
				// 이 줄 카메라 칸 = 앞 프레임 갱신 결과. 그 갱신이 본 능력 상태 = 앞 줄 조준 표식, 시간 = 앞 줄 프레임 시간
				const float alphaBefore = cam.GetAimingAlpha();
				const bool quick = prev.ads && !cur.ads && cam.IsTopAds() && cam.GetCount() >= 2;
				const float camDt = std::fabs((float)prev.dt - 1.f / 60.f) < 1e-5f ? 1.f / 60.f : (float)prev.dt;
				cam.Tick(prev.adsTag > 0, camDt);
				motion.aimingAlpha = aimLag > 0 ? alphaBefore : cam.GetAimingAlpha();

				++cc.aimFrames;
				if (cur.aimAlpha > 0.f) ++cc.aimPositive;
				if (cur.camCount >= 2) ++cc.aimBlending;
				if (quick) ++cc.aimQuickRelease;
				if (cur.ads && !prev.ads) ++cc.aimPress;
				const bool badCount = cam.GetCount() != cur.camCount;
				const bool badTop = (cam.IsTopAds() ? 1 : 0) != cur.topAds;
				const double alphaDiff = std::fabs(cam.GetAimingAlpha() - cur.aimAlpha);
				const double topDiff = std::fabs(cam.GetTopWeight() - cur.topWeight);
				if (badCount) ++cc.aimCountDiff;
				if (badTop) ++cc.aimTopDiff;
				if ((cur.adsTag > 0 ? 1 : 0) != prev.ads) ++cc.aimTagDiff;
				cc.aimAlphaMax = std::max(cc.aimAlphaMax, alphaDiff);
				cc.aimTopWeightMax = std::max(cc.aimTopWeightMax, topDiff);
				if ((badCount || badTop || alphaDiff > 1e-4 || topDiff > 1e-4) && cc.aimFirstBadFrame < 0)
				{
					cc.aimFirstBadFrame = (int)i;
					cc.aimFirstBadSeg = s.seg;
				}
			}
			const WeaponEvents ev = sim.Tick(dt, input, motion);

			++cc.frames;
			if (ev.fired)
			{
				++cc.simShots;
				if (lastSimShot >= 0) ++cc.simGaps[(int)i - lastSimShot];
				lastSimShot = (int)i;
			}
			if (ev.reloaded) ++cc.simReloads;
			if (cur.mag < prevMag)
			{
				cc.lyraShots += prevMag - cur.mag;
				if (lastShot >= 0) ++cc.gaps[(int)i - lastShot];
				lastShot = (int)i;
			}
			if (prev.reload && pendingReload < 0 && prev.mag < local.defs[s.weapon].magazineSize && prev.spare > 0) pendingReload = (int)i;
			if (cur.mag > prevMag)
			{
				++cc.lyraReloads;
				if (pendingReload >= 0) ++cc.reloadFrames[(int)i - pendingReload];
				pendingReload = -1;
			}
			prevMag = cur.mag;

			const bool badMag = sim.GetMagazine() != cur.mag;
			const bool badSpare = sim.GetSpare() != cur.spare;
			if (badMag) ++cc.magDiff;
			if (badSpare) ++cc.spareDiff;
			if ((badMag || badSpare) && cc.firstBadFrame < 0)
			{
				cc.firstBadFrame = (int)i;
				cc.firstBadSeg = s.seg;
			}
			const CWeaponSpread& sp = sim.GetSpread();
			cc.heatMax = std::max(cc.heatMax, (double)std::fabs(sp.GetHeat() - cur.heat));
			cc.spreadMax = std::max(cc.spreadMax, (double)std::fabs(sp.GetSpreadAngle() - cur.spread));
			cc.multMax = std::max(cc.multMax, (double)std::fabs(sp.GetCalculatedMultiplier() - cur.mult));
			if (std::fabs(sp.GetCalculatedMultiplier() - cur.mult) > 0.01f && cc.multFirstBadFrame < 0)
			{
				cc.multFirstBadFrame = (int)i;
				cc.multFirstBadSeg = s.seg;
			}
			if ((sp.HasFirstShotAccuracy() ? 1 : 0) != cur.first) ++cc.firstDiff;
			if (cur.hasCrouch)
			{
				const double multDiff = std::fabs(sp.GetCalculatedMultiplier() - cur.mult);
				++cc.crouchRows;
				if (cur.crouched != prev.crouched)
				{
					++cc.crouchToggles;
					lastCrouchToggle = (int)i;
				}
				if (cur.crouched)
				{
					++cc.crouchFrames;
					if (cur.mag < prev.mag) cc.crouchShots += prev.mag - cur.mag;
					cc.crouchMultMax = std::max(cc.crouchMultMax, multDiff);
				}
				if ((int)i - lastCrouchToggle < 30)
				{
					++cc.crouchNearToggle;
					cc.crouchNearMultMax = std::max(cc.crouchNearMultMax, multDiff);
				}
			}
		}
	}
	for (int w = 0; w < kWeaponCount; ++w)
	{
		const CycleCount& cc = cycle[w];
		std::printf("CYCLE   %-8s frames=%d shots lyra=%d sim=%d | magDiff=%d spareDiff=%d firstBad=seg%d:f%d | reloads lyra=%d sim=%d | heatMax=%.4f spreadMax=%.4f multMax=%.4f firstShotDiff=%d\n", local.defs[w].name, cc.frames, cc.lyraShots, cc.simShots, cc.magDiff, cc.spareDiff, cc.firstBadSeg, cc.firstBadFrame, cc.lyraReloads, cc.simReloads, cc.heatMax, cc.spreadMax, cc.multMax, cc.firstDiff);
		std::printf("        gaps lyra:");
		for (const auto& g : cc.gaps) std::printf(" %d:%d", g.first, g.second);
		std::printf(" | sim:");
		for (const auto& g : cc.simGaps) std::printf(" %d:%d", g.first, g.second);
		std::printf(" | reload frames lyra:");
		for (const auto& g : cc.reloadFrames) std::printf(" %d:%d", g.first, g.second);
		std::printf("\n");
		if (cc.lyraShots == 0 || cc.magDiff != 0 || cc.spareDiff != 0 || cc.heatMax > 0.01 || cc.spreadMax > 0.01 || cc.multMax > 0.01 || cc.firstDiff != 0) failed = true;
		// 앉음 칸이 있는 기록에만 찍는다(옛 기록 출력은 그대로). 앉기를 안 섞은 기록은 앉은 프레임 0 이 정상이라 개수로 실패시키지 않는다 — 배율 차는 위 multMax 가 이미 본다
		if (cc.crouchRows > 0)
			std::printf("CROUCH  %-8s rows=%d crouched=%d toggles=%d nearToggle=%d crouchedShots=%d | multMax crouched=%.4f nearToggle=%.4f\n", local.defs[w].name, cc.crouchRows, cc.crouchFrames, cc.crouchToggles, cc.crouchNearToggle, cc.crouchShots, cc.crouchMultMax, cc.crouchNearMultMax);
		if (cc.aimFrames > 0)
		{
			std::printf("AIM     %-8s lag=%d frames=%d alpha>0=%d blending=%d presses=%d quickRelease=%d | countDiff=%d topDiff=%d tagDiff=%d alphaMax=%.6f topWeightMax=%.6f firstBad=seg%d:f%d | multFirstBad=seg%d:f%d\n", local.defs[w].name, aimLag, cc.aimFrames, cc.aimPositive, cc.aimBlending, cc.aimPress, cc.aimQuickRelease, cc.aimCountDiff, cc.aimTopDiff, cc.aimTagDiff, cc.aimAlphaMax, cc.aimTopWeightMax, cc.aimFirstBadSeg, cc.aimFirstBadFrame, cc.multFirstBadSeg, cc.multFirstBadFrame);
			// 개수가 0 이면 그 갈래를 안 본 것이다 — 조준 비율이 선 프레임 · 섞이는 중 · 빨리 뗌이 다 있어야 통과
			if (cc.aimPositive == 0 || cc.aimBlending == 0 || cc.aimQuickRelease == 0 || cc.aimCountDiff != 0 || cc.aimTopDiff != 0 || cc.aimTagDiff != 0 || cc.aimAlphaMax > 1e-4 || cc.aimTopWeightMax > 1e-4) failed = true;
		}
	}

	// ─── 4. 실제 맞힘: 표 계산 합 ↔ 체력 차 ─────────────────
	{
		struct HitCount { int hits = 0, hp = 0, weak = 0, over = 0; double maxDiff = 0, sumLyra = 0, sumMine = 0, maxStartDist = 0, maxStartHoriz = 0; } hc[kWeaponCount];
		double pending = 0;
		int pendingWeapon = -1;
		for (const std::vector<std::string>& c : hitLike)
		{
			const int w = WeaponIndex(c[1]);
			if (w < 0) continue;
			if (c[0] == "HIT")
			{
				const Vec3 start(D(c[5]), D(c[6]), D(c[7]));
				const Vec3 me(D(c[12]), D(c[13]), D(c[14]));
				const Vec3 fromMe = start - me;
				const double dist = D(c[11]);
				const bool weak = c[4].find("WeakSpot") != std::string::npos;
				pending += ComputeDamage(local.defs[w], dist, weak, false);
				pendingWeapon = w;
				HitCount& h = hc[w];
				++h.hits;
				if (weak) ++h.weak;
				h.maxStartDist = std::max(h.maxStartDist, std::sqrt(fromMe.x * fromMe.x + fromMe.y * fromMe.y + fromMe.z * fromMe.z));
				h.maxStartHoriz = std::max(h.maxStartHoriz, std::sqrt(fromMe.x * fromMe.x + fromMe.y * fromMe.y));
			}
			else
			{
				HitCount& h = hc[w];
				++h.hp;
				const double recorded = D(c[3]);
				const double diff = std::fabs(recorded - pending);
				h.maxDiff = std::max(h.maxDiff, diff);
				h.sumLyra += recorded;
				h.sumMine += pending;
				if (diff >= 0.01)
				{
					if (h.over < 5) std::printf("HIT diff %s frame=%s lyra=%.4f mine=%.4f\n", c[1].c_str(), c[2].c_str(), recorded, pending);
					++h.over;
				}
				pending = 0;
				pendingWeapon = -1;
			}
		}
		(void)pendingWeapon;
		for (int w = 0; w < kWeaponCount; ++w)
		{
			const HitCount& h = hc[w];
			std::printf("HITS    %-8s hits=%d weak=%d hpChanges=%d sum lyra=%.2f mine=%.2f maxDiff=%.4f over0.01=%d | startFromMe max=%.1f horiz=%.1f\n", local.defs[w].name, h.hits, h.weak, h.hp, h.sumLyra, h.sumMine, h.maxDiff, h.over, h.maxStartDist, h.maxStartHoriz);
			if (h.hits == 0 || h.hp == 0 || h.over != 0) failed = true;
		}
	}

	// ─── 5. 몸·맵 광선 ─────────────────────────────────────
	{
		struct RayCount
		{
			int total = 0, lyra[3] = {}, mine[3] = {}, agree = 0;
			int bodyEither = 0, bodyBoth = 0, weakSame = 0, boneSame = 0, lyraWeak = 0;
			int mapBoth = 0, missNoBot = 0;
			double bodyDistMax = 0, bodyDistSum = 0, mapDistMax = 0;
			int multi = 0;
		};
		std::map<std::pair<int, double>, RayCount> table2;
		// 몸 판정이 갈린 광선 — 스치는 수준인가(반지름을 키우면 닿나), 특정 부위에 몰리나
		struct MissCount
		{
			int lyraOnly = 0, mineOnly = 0;
			int within[5] = {};	// Lyra 만 맞았을 때 내 몸에 닿게 되는 추가 반지름 1·2·3·5·10cm
			std::map<std::string, int> lyraBones, mineBones;
		};
		std::map<int, MissCount> misses;
		// ResolveBullet(선 먼저, 그다음 구)이 선·구 각각의 결과와 어긋나지 않나
		int resolveLineRows = 0, resolveLineBad = 0, resolveSweepRows = 0, resolveSweepBad = 0, resolveUsedSweep = 0, resolveLineOverMap = 0;
		size_t shapeCount = 0;
		const BodyShape* shapes = GetBodyShapes(shapeCount);
		for (const RayRow& r : rays)
		{
			RayCount& rc = table2[{ r.state, r.radius }];
			const auto it = bots.find({ r.state, r.frame });
			if (it == bots.end())
			{
				++rc.missNoBot;
				continue;
			}
			++rc.total;
			if (r.numHits > 1) ++rc.multi;

			BodyPlacement body;
			body.center = it->second.pos;
			body.yawDegrees = it->second.yaw;
			const Vec3 seg = r.b - r.a;
			const double len = std::sqrt(seg.x * seg.x + seg.y * seg.y + seg.z * seg.z);

			double mapDist = 1e30;
			if (r.radius > 0)
			{
				CapsuleHit ch;
				if (mapCollision.SweepCapsule(r.a, r.b, r.radius, r.radius, MAP_BLOCK_WEAPON, ch)) mapDist = ch.distance;
			}
			else
			{
				RayHit rh;
				if (mapCollision.Raycast(r.a, r.b, MAP_BLOCK_WEAPON, rh)) mapDist = rh.distance;
			}
			BodyHit bh;
			const bool bodyHit = TraceBody(body, r.a, r.b, r.radius, bh);
			const double bodyDist = bodyHit ? bh.time * len : 1e30;
			int mineKind = 0;
			if (bodyHit && bodyDist <= mapDist) mineKind = 2;
			else if (mapDist < 1e29) mineKind = 1;

			BulletResult bullet;
			ResolveBullet(mapCollision, &body, 1, r.a, r.b, r.radius, bullet);
			if (r.radius <= 0)
			{
				++resolveLineRows;
				if ((int)bullet.kind != mineKind) ++resolveLineBad;
			}
			else
			{
				++resolveSweepRows;
				if (mineKind == 2 && bullet.kind != EBulletHit::Body) ++resolveSweepBad;
				if (bullet.usedSweep) ++resolveUsedSweep;
				if (mineKind == 1 && bullet.kind == EBulletHit::Body) ++resolveLineOverMap;
			}

			const int lyraKind = std::min(std::max(r.kind, 0), 2);
			++rc.lyra[lyraKind];
			if ((lyraKind == 2) != (mineKind == 2))
			{
				MissCount& mc = misses[r.state];
				if (lyraKind == 2)
				{
					++mc.lyraOnly;
					++mc.lyraBones[r.bone];
					static const double kExtra[5] = { 1, 2, 3, 5, 10 };
					for (int e = 0; e < 5; ++e)
					{
						BodyHit grown;
						if (TraceBody(body, r.a, r.b, r.radius + kExtra[e], grown))
						{
							++mc.within[e];
						}
					}
				}
				else
				{
					++mc.mineOnly;
					++mc.mineBones[shapes[bh.shape].bone];
				}
			}
			++rc.mine[mineKind];
			if (mineKind == lyraKind) ++rc.agree;
			if (lyraKind == 2 && r.weak) ++rc.lyraWeak;
			if (lyraKind == 2 || mineKind == 2) ++rc.bodyEither;
			if (lyraKind == 2 && mineKind == 2)
			{
				++rc.bodyBoth;
				if ((bh.weakSpot ? 1 : 0) == r.weak) ++rc.weakSame;
				if (r.bone == shapes[bh.shape].bone) ++rc.boneSame;
				const double diff = std::fabs(bodyDist - r.dist);
				rc.bodyDistMax = std::max(rc.bodyDistMax, diff);
				rc.bodyDistSum += diff;
			}
			if (lyraKind == 1 && mineKind == 1)
			{
				++rc.mapBoth;
				rc.mapDistMax = std::max(rc.mapDistMax, std::fabs(mapDist - r.dist));
			}
		}
		int total = 0;
		std::printf("RESOLVE line rows=%d bad=%d | sweep rows=%d bad=%d usedSweep=%d lineBodyOverSweepMap=%d\n", resolveLineRows, resolveLineBad, resolveSweepRows, resolveSweepBad, resolveUsedSweep, resolveLineOverMap);
		if (resolveLineRows == 0 || resolveSweepRows == 0 || resolveUsedSweep == 0 || resolveLineBad != 0 || resolveSweepBad != 0) failed = true;
		for (const auto& kv : misses)
		{
			const MissCount& mc = kv.second;
			std::printf("RAYMISS state=%d lyraOnly=%d mineOnly=%d | Lyra 만 맞음 중 내 몸까지 +1cm=%d +2cm=%d +3cm=%d +5cm=%d +10cm=%d\n", kv.first, mc.lyraOnly, mc.mineOnly, mc.within[0], mc.within[1], mc.within[2], mc.within[3], mc.within[4]);
			std::printf("        lyra bones:");
			for (const auto& b : mc.lyraBones) std::printf(" %s:%d", b.first.c_str(), b.second);
			std::printf("\n        mine bones:");
			for (const auto& b : mc.mineBones) std::printf(" %s:%d", b.first.c_str(), b.second);
			std::printf("\n");
		}
		for (const auto& kv : table2)
		{
			const RayCount& rc = kv.second;
			total += rc.total;
			std::printf("RAYS    state=%d r=%.1f n=%d lyra[none/map/body]=%d/%d/%d mine=%d/%d/%d agree=%.2f%% | body either=%d both=%d(%.1f%%) weak same=%d/%d(lyraWeak=%d) bone same=%d avgDist=%.2f maxDist=%.2f | map both=%d maxDist=%.3f | lyraMultiHit=%d noBot=%d\n", kv.first.first, kv.first.second, rc.total, rc.lyra[0], rc.lyra[1], rc.lyra[2], rc.mine[0], rc.mine[1], rc.mine[2], rc.total ? 100.0 * rc.agree / rc.total : 0.0, rc.bodyEither, rc.bodyBoth, rc.bodyEither ? 100.0 * rc.bodyBoth / rc.bodyEither : 0.0, rc.weakSame, rc.bodyBoth, rc.lyraWeak, rc.boneSame, rc.bodyBoth ? rc.bodyDistSum / rc.bodyBoth : 0.0, rc.bodyDistMax, rc.mapBoth, rc.mapDistMax, rc.multi, rc.missNoBot);
		}
		if (total == 0) failed = true;
	}

	// ─── 6. 퍼짐 방향 — 원뿔 안에 드는지, 분포가 지수를 따르는지 ──
	{
		CSpreadRng rng(20261102);
		const Vec3 aim(0.6, 0.64, 0.48);
		const double aimLen = std::sqrt(aim.x * aim.x + aim.y * aim.y + aim.z * aim.z);
		double maxOver = 0;
		double sum = 0;
		const int n = 200000;
		for (int i = 0; i < n; ++i)
		{
			const Vec3 d = SpreadDirection(aim, 10.f, 1.f, 1.f, rng);
			const double cosv = (d.x * aim.x + d.y * aim.y + d.z * aim.z) / aimLen;
			const double deg = std::acos(std::min(std::max(cosv, -1.0), 1.0)) * 180.0 / 3.14159265358979;
			maxOver = std::max(maxOver, deg - 5.0);
			sum += deg;
		}
		// 지수 1 → 각도가 0~5 도 고른 분포 → 평균 2.5 도
		std::printf("CONE    samples=%d meanDeg=%.3f (기대 2.5) overHalf=%.6f\n", n, sum / n, maxOver);
		if (std::fabs(sum / n - 2.5) > 0.02 || maxOver > 1e-6) failed = true;
	}

	// ─── 7. 입력 번호로 정한 퍼짐 — 번호가 1씩 늘어도 분포가 치우치지 않는지, 번호 · 탄 번호마다 달라지는지 ──
	{
		const Vec3 aim(0.6, 0.64, 0.48);
		const double aimLen = std::sqrt(aim.x * aim.x + aim.y * aim.y + aim.z * aim.z);
		double sumDeg = 0, maxOver = 0;
		int quadrant[4] = { 0, 0, 0, 0 };
		int sameAsNext = 0, sameAsNextBullet = 0;
		const int n = 200000;
		std::vector<double> degs((size_t)n);
		const auto direction = [&](uint32_t seq, uint32_t bullet)
		{
			CSpreadRng rng(SpreadSeed(seq, bullet));
			return SpreadDirection(aim, 10.f, 1.f, 1.f, rng);
		};
		const Vec3 center = aim * (1.0 / aimLen);
		const double flat = std::sqrt(center.x * center.x + center.y * center.y);
		const Vec3 right(-center.y / flat, center.x / flat, 0);
		const Vec3 up(center.y * right.z - center.z * right.y, center.z * right.x - center.x * right.z, center.x * right.y - center.y * right.x);
		for (int i = 1; i <= n; ++i)
		{
			const Vec3 d = direction((uint32_t)i, 0);
			const double cosv = std::min(std::max(d.x * center.x + d.y * center.y + d.z * center.z, -1.0), 1.0);
			const double deg = std::acos(cosv) * 180.0 / 3.14159265358979;
			sumDeg += deg;
			degs[(size_t)(i - 1)] = deg;
			maxOver = std::max(maxOver, deg - 5.0);
			const double r = d.x * right.x + d.y * right.y + d.z * right.z;
			const double u = d.x * up.x + d.y * up.y + d.z * up.z;
			++quadrant[(r >= 0 ? 1 : 0) + (u >= 0 ? 2 : 0)];
			const Vec3 next = direction((uint32_t)i + 1, 0);
			const Vec3 nextBullet = direction((uint32_t)i, 1);
			if (d.x == next.x && d.y == next.y && d.z == next.z) ++sameAsNext;
			if (d.x == nextBullet.x && d.y == nextBullet.y && d.z == nextBullet.z) ++sameAsNextBullet;
		}
		int quadrantMin = n, quadrantMax = 0;
		for (int q : quadrant)
		{
			quadrantMin = std::min(quadrantMin, q);
			quadrantMax = std::max(quadrantMax, q);
		}
		// 이웃 번호끼리 벌어진 각도의 상관 — 번호를 안 섞으면 xorshift 첫 값이 이웃끼리 거의 같아 여기서 걸린다. 평균 · 사분면은 이걸 못 본다.
		// 기준 0.02 = 표본 20만의 상관계수 표준오차(약 0.0022)의 약 9배. 소총 사격 간격이 9입력이라 9칸도 본다
		const auto lagCorrelation = [&](int lag)
		{
			double sumA = 0, sumB = 0, sumAA = 0, sumBB = 0, sumAB = 0;
			const int m = n - lag;
			for (int i = 0; i < m; ++i)
			{
				const double a = degs[(size_t)i], b = degs[(size_t)(i + lag)];
				sumA += a;
				sumB += b;
				sumAA += a * a;
				sumBB += b * b;
				sumAB += a * b;
			}
			return (sumAB - sumA * sumB / m) / std::sqrt((sumAA - sumA * sumA / m) * (sumBB - sumB * sumB / m));
		};
		const double corr1 = lagCorrelation(1), corr9 = lagCorrelation(9);
		std::printf("SEEDED  samples=%d meanDeg=%.3f (기대 2.5) overHalf=%.6f 사분면 %d~%d (기대 %d) 다음번호같음=%d 다음탄같음=%d 이웃상관 1칸=%.4f 9칸=%.4f\n", n, sumDeg / n, maxOver, quadrantMin, quadrantMax, n / 4, sameAsNext, sameAsNextBullet, corr1, corr9);
		if (std::fabs(sumDeg / n - 2.5) > 0.02 || maxOver > 1e-6 || quadrantMax - quadrantMin > n / 50 || sameAsNext != 0 || sameAsNextBullet != 0 || std::fabs(corr1) > 0.02 || std::fabs(corr9) > 0.02) failed = true;
	}

	std::printf("%s\n", failed ? "RESULT FAIL" : "RESULT PASS");
	return failed ? 1 : 0;
}
