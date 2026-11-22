#include "JumpPads.h"

#include <cmath>
#include <cstdlib>

namespace mo
{
	namespace
	{
		// 청사진 그래프 마디 값(B_Launcher_Push Target Method · 원본 기준 — 11-22 그래프 글 weapons_launchers.txt, make_launchers.py GRAPH_GUARDS 가 지킨다)
		constexpr double kPushTargetXYScale = 2.1;		// 수평 = 앞방향 × |Target.xy| × 2.1
		constexpr double kPushTargetZScale = 2.0;		// 수직 = clamp(Target.z × 2, 250, 850)
		constexpr double kPushTargetZMin = 250.0;
		constexpr double kPushTargetZMax = 850.0;
		constexpr double kPushClampMin = -12000.0;		// ClampVectorSize(-12000, 12000)
		constexpr double kPushClampMax = 12000.0;
		constexpr double kPi = 3.14159265358979323846;
		constexpr double kSmallNumber = 1.e-8;			// UE_SMALL_NUMBER — GetClampedToSize 의 0 벡터 갈래

		size_t SplitFields(const char* p, const char* end, std::string* out, size_t maxFields)
		{
			size_t n = 0;
			while (p < end && *p != '#')
			{
				while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) ++p;
				if (p >= end || *p == '#') break;
				const char* start = p;
				while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '#') ++p;
				if (n == maxFields) return maxFields + 1;	// 칸이 넘침 — 틀린 줄
				out[n++].assign(start, p);
			}
			return n;
		}

		bool ToNumber(const std::string& s, double& out)
		{
			if (s.empty()) return false;
			char* end = nullptr;
			out = std::strtod(s.c_str(), &end);
			return end == s.c_str() + s.size() && std::isfinite(out);
		}

		bool ToFlag(const std::string& s, bool& out)
		{
			if (s == "0") out = false;
			else if (s == "1") out = true;
			else return false;
			return true;
		}

		bool ToKind(const std::string& s, ELauncherKind& out)
		{
			if (s == "up") out = ELauncherKind::Up;
			else if (s == "push") out = ELauncherKind::Push;
			else return false;
			return true;
		}
	}

	ELaunchersLoad ParseLaunchers(const char* text, size_t size, std::vector<Launcher>& out, size_t& outBadLine)
	{
		constexpr size_t kFields = 15;
		std::vector<Launcher> launchers;
		outBadLine = 0;
		size_t lineNo = 0;
		const char* p = text;
		const char* const end = text + size;
		while (p < end)
		{
			const char* lineEnd = p;
			while (lineEnd < end && *lineEnd != '\n') ++lineEnd;
			++lineNo;
			std::string f[kFields];
			const size_t n = SplitFields(p, lineEnd, f, kFields);
			p = lineEnd < end ? lineEnd + 1 : end;
			if (n == 0) continue;

			Launcher l;
			double v[10] = {};
			bool ok = n == kFields && f[0] == "LAUNCHER" && ToKind(f[1], l.kind) && !f[2].empty();
			for (size_t i = 0; ok && i < 6; ++i) ok = ToNumber(f[3 + i], v[i]);
			ok = ok && ToFlag(f[9], l.useTarget);
			for (size_t i = 0; ok && i < 4; ++i) ok = ToNumber(f[10 + i], v[6 + i]);
			ok = ok && ToFlag(f[14], l.noFallEntry);
			if (ok)
			{
				l.name = f[2];
				l.sphereCenter = Vec3(v[0], v[1], v[2]);
				l.sphereRadius = v[3];
				l.upOverride = v[4];
				l.multiplier = v[5];
				l.target = Vec3(v[6], v[7], v[8]);
				l.minVerticalVelocity = v[9];
				ok = l.sphereRadius > 0;
			}
			if (!ok)
			{
				outBadLine = lineNo;
				return ELaunchersLoad::BadLine;
			}
			if (launchers.size() == kMaxLaunchers) return ELaunchersLoad::TooMany;
			launchers.push_back(l);
		}
		if (launchers.empty()) return ELaunchersLoad::NoLaunchers;
		out.swap(launchers);
		return ELaunchersLoad::Ok;
	}

	const char* LaunchersLoadName(ELaunchersLoad result)
	{
		switch (result)
		{
		case ELaunchersLoad::Ok: return "ok";
		case ELaunchersLoad::BadLine: return "bad-line";
		case ELaunchersLoad::NoLaunchers: return "no-launchers";
		case ELaunchersLoad::TooMany: return "too-many";
		}
		return "?";
	}

	const char* LauncherKindName(ELauncherKind kind)
	{
		switch (kind)
		{
		case ELauncherKind::Up: return "up";
		case ELauncherKind::Push: return "push";
		}
		return "?";
	}

	bool LauncherOverlaps(const Launcher& launcher, const Vec3& capsuleCenter, double capsuleRadius, double capsuleHalfHeight)
	{
		const double axisHalf = capsuleHalfHeight > capsuleRadius ? capsuleHalfHeight - capsuleRadius : 0.0;
		const Vec3& c = launcher.sphereCenter;
		double z = c.z;
		if (z < capsuleCenter.z - axisHalf) z = capsuleCenter.z - axisHalf;
		if (z > capsuleCenter.z + axisHalf) z = capsuleCenter.z + axisHalf;
		const double dx = c.x - capsuleCenter.x, dy = c.y - capsuleCenter.y, dz = c.z - z;
		const double reach = launcher.sphereRadius + capsuleRadius;
		return dx * dx + dy * dy + dz * dz < reach * reach;
	}

	uint8_t LauncherMask(const Launcher* launchers, size_t count, const Vec3& capsuleCenter, double capsuleRadius, double capsuleHalfHeight)
	{
		uint8_t mask = 0;
		for (size_t i = 0; i < count && i < kMaxLaunchers; ++i)
		{
			if (LauncherOverlaps(launchers[i], capsuleCenter, capsuleRadius, capsuleHalfHeight)) mask |= (uint8_t)(1u << i);
		}
		return mask;
	}

	bool LauncherLaunch(const Launcher& launcher, const Vec3& velocity, const Vec3& lastUpdateVelocity, double yawDegrees, Vec3& outLaunch)
	{
		// 두 자식 그래프 Overlap 머리 — Do Not Activate on Fall Entry AND (GetLastUpdateVelocity.Z < MinimumVerticalVelocityAllowed) 면 안 쏘고 거짓
		if (launcher.noFallEntry && lastUpdateVelocity.z < launcher.minVerticalVelocity) return false;
		// GetActorForwardVector — Lyra 몸은 조종 요만 따른다(LyraCharacter.cpp:75-77, 고개 숙임 · 기울기는 안 씀) → 수평 단위 벡터
		const double yaw = yawDegrees * (kPi / 180.0);
		const double fx = std::cos(yaw), fy = std::sin(yaw);
		Vec3 launch;
		bool xyOverride = false, zOverride = false;
		if (launcher.kind == ELauncherKind::Up)
		{
			// B_Launcher_Up — MakeVector(앞.x, 앞.y, UpOverride) · LaunchCharacter(XY 덮지 않음, Z 덮음)
			launch = Vec3(fx, fy, launcher.upOverride);
			zOverride = true;
		}
		else if (launcher.useTarget)
		{
			// B_Launcher_Push Target Method — 앞방향 × (VSizeXY(Target) × 2.1) · Z = clamp(Target.z × 2, 250, 850) · ClampVectorSize(±12000) · 셋 다 더함
			const double xy = std::sqrt(launcher.target.x * launcher.target.x + launcher.target.y * launcher.target.y) * kPushTargetXYScale;
			double z = launcher.target.z * kPushTargetZScale;
			if (z < kPushTargetZMin) z = kPushTargetZMin;
			if (z > kPushTargetZMax) z = kPushTargetZMax;
			const Vec3 raw(fx * xy, fy * xy, z);
			// FVector::GetClampedToSize — 크기 안이어도 방향 × 크기로 다시 만든다(반올림까지 같게)
			double size = std::sqrt(raw.x * raw.x + raw.y * raw.y + raw.z * raw.z);
			const Vec3 dir = size > kSmallNumber ? raw * (1.0 / size) : Vec3();
			if (size < kPushClampMin) size = kPushClampMin;
			if (size > kPushClampMax) size = kPushClampMax;
			launch = dir * size;
		}
		else
		{
			// B_Launcher_Push 목표 안 씀 — MakeVector(앞.x, 앞.y, UpOverride) × Multiplier · 셋 다 더함(L_Expanse 엔 없는 갈래 — 8개 다 useTarget)
			launch = Vec3(fx, fy, launcher.upOverride) * launcher.multiplier;
		}
		// ACharacter::LaunchCharacter(Character.cpp:1499-1520) — 덮지 않는 축은 GetVelocity() 를 더한다
		if (!xyOverride)
		{
			launch.x += velocity.x;
			launch.y += velocity.y;
		}
		if (!zOverride) launch.z += velocity.z;
		outLaunch = launch;
		return true;
	}

	LauncherTickResult TickWithLaunchers(CMoveSim& sim, MoveState& state, const Vec3& input, float deltaTime, double yawDegrees, const LauncherSet& launchers, double capsuleRadius)
	{
		LauncherTickResult result;
		if (launchers.count == 0)
		{
			sim.Tick(state, input, deltaTime);
			return result;
		}
		const bool pending = state.launchPending;
		const Vec3 launch = state.pendingLaunch;
		state.launchPending = false;
		state.pendingLaunch = Vec3();
		sim.Tick(state, input, deltaTime, pending ? &launch : nullptr);
		result.launchedThisTick = pending;

		const uint8_t now = LauncherMask(launchers.items, launchers.count, state.location, capsuleRadius, sim.HalfHeightOf(state));
		const uint8_t began = (uint8_t)(now & ~state.launcherMask);
		state.launcherMask = now;
		for (size_t i = 0; i < launchers.count && i < kMaxLaunchers; ++i)
		{
			if (!(began & (1u << i))) continue;
			++result.begins;
			Vec3 out;
			// 겹침 사건 때 GetVelocity · GetLastUpdateVelocity = 이동 끝 속도(이동 묶음이 닫힌 뒤) — j-2 기록: 이동 뒤 속도로 셈한 발사가 51번 다 맞음
			if (!LauncherLaunch(launchers.items[i], state.velocity, state.velocity, yawDegrees, out))
			{
				++result.refused;
				continue;
			}
			// 여럿이 같은 걸음에 시작하면 Launch 가 PendingLaunchVelocity 를 덮는다 — 사건 차례는 모르므로 표 차례 마지막
			// (L_Expanse 는 가장 가까운 두 구 칸도 중심 사이 1,437cm(수평 960cm)라 몸 하나가 둘에 못 든다)
			state.launchPending = true;
			state.pendingLaunch = out;
			result.fired = (int)i;
		}
		return result;
	}

	void ResetLaunchers(MoveState& state, const LauncherSet& launchers, double capsuleRadius, double capsuleHalfHeight)
	{
		state.launchPending = false;
		state.pendingLaunch = Vec3();
		state.launcherMask = launchers.count ? LauncherMask(launchers.items, launchers.count, state.location, capsuleRadius, capsuleHalfHeight) : 0;
	}
}
