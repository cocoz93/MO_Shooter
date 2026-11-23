#include "Teleports.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace mo
{
	namespace
	{
		constexpr double kPi = 3.14159265358979323846;
		constexpr double kAxisTolerance = 1.e-6;	// 단위 벡터 · 수평 축 검사 — 덤프는 12자리라 이보다 훨씬 작게 맞는다

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

		bool ToIndex(const std::string& s, int& out)
		{
			double v = 0;
			if (!ToNumber(s, v) || v < 0 || v >= (double)kMaxTeleports || v != std::floor(v)) return false;
			out = (int)v;
			return true;
		}

		bool ToFlag(const std::string& s, bool& out)
		{
			if (s == "0") out = false;
			else if (s == "1") out = true;
			else return false;
			return true;
		}

		bool ToVec(const std::string* f, Vec3& out)
		{
			double v[3];
			for (int i = 0; i < 3; ++i)
			{
				if (!ToNumber(f[i], v[i])) return false;
			}
			out = Vec3(v[0], v[1], v[2]);
			return true;
		}

		double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		double Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }
		bool IsUnit(const Vec3& v) { return std::fabs(Length(v) - 1.0) <= kAxisTolerance; }
		bool IsFinite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

		// 장치 한 줄의 모양 검사 — 파일 읽기(ParseTeleports)와 와이어 읽기(TeleportFromWire)가 같이 쓴다. 칸의 z 축은 파일에만 있어 그쪽이 따로 본다
		bool ShapeOk(const Teleporter& t)
		{
			bool ok = IsFinite(t.boxCenter) && IsFinite(t.boxExtent) && IsFinite(t.root) && IsFinite(t.exit) && std::isfinite(t.launchMultiplier) && std::isfinite(t.offsetZMax);
			// 칸이 서 있어야 겹침 셈(TeleportOverlaps)이 맞다 — x · y 축 = 수평 단위 벡터
			ok = ok && IsUnit(t.boxAxisX) && IsUnit(t.boxAxisY) && std::fabs(t.boxAxisX.z) <= kAxisTolerance && std::fabs(t.boxAxisY.z) <= kAxisTolerance &&
				t.boxExtent.x > 0 && t.boxExtent.y > 0 && t.boxExtent.z > 0 && IsUnit(t.rootForward) && IsUnit(t.exitForward) && t.offsetZMax >= 0;
			// 「들어간 자리 그대로」 — 보정(MatchTeleportExit)이 나갈 면 위 판정을 쓰려면 뿌리 앞방향이 수평이어야 한다(L_Expanse 문 포털 둘 = z 0)
			if (ok && t.keepRelative) ok = std::fabs(t.rootForward.z) <= kAxisTolerance;
			return ok && !t.name.empty() && t.name.size() <= kMaxTeleportName;
		}

		void ToArray(const Vec3& v, double* out) { out[0] = v.x; out[1] = v.y; out[2] = v.z; }
		Vec3 FromArray(const double* v) { return Vec3(v[0], v[1], v[2]); }
	}

	ETeleportsLoad ParseTeleports(const char* text, size_t size, TeleportTable& out, size_t& outBadLine)
	{
		constexpr size_t kFields = 36;		// TELEPORT 차례 이름 짝 + 벡터 9개(27) + 낱값 5
		TeleportTable table;
		size_t pawnLines = 0;
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

			if (f[0] == "TELEPORT_PAWN")
			{
				if (n != 3 || !ToNumber(f[1], table.meshBaseZ) || !ToNumber(f[2], table.standHalfHeight) || !(table.standHalfHeight > 0))
				{
					outBadLine = lineNo;
					return ETeleportsLoad::BadLine;
				}
				++pawnLines;
				continue;
			}

			Teleporter t;
			int index = -1;
			Vec3 boxZ;
			bool ok = n == kFields && f[0] == "TELEPORT" && ToIndex(f[1], index) && !f[2].empty() && f[2].size() <= kMaxTeleportName && ToIndex(f[3], t.target);
			ok = ok && ToVec(f + 4, t.boxCenter) && ToVec(f + 7, t.boxAxisX) && ToVec(f + 10, t.boxAxisY) && ToVec(f + 13, boxZ) && ToVec(f + 16, t.boxExtent);
			ok = ok && ToVec(f + 19, t.root) && ToVec(f + 22, t.rootForward) && ToVec(f + 25, t.exit) && ToVec(f + 28, t.exitForward);
			ok = ok && ToFlag(f[31], t.keepRelative) && ToFlag(f[32], t.launchOnTeleport) && ToNumber(f[33], t.launchMultiplier) &&
				ToFlag(f[34], t.forceDirection) && ToNumber(f[35], t.offsetZMax);
			if (ok)
			{
				t.name = f[2];
				// 칸이 서 있어야 겹침 셈(TeleportOverlaps)이 맞다 — z 축 = 월드 위(나머지 모양은 ShapeOk)
				ok = std::fabs(boxZ.x) <= kAxisTolerance && std::fabs(boxZ.y) <= kAxisTolerance && std::fabs(boxZ.z - 1.0) <= kAxisTolerance &&
					ShapeOk(t) && index == (int)table.items.size() && t.target != index;
			}
			if (!ok)
			{
				outBadLine = lineNo;
				return ETeleportsLoad::BadLine;
			}
			if (table.items.size() == kMaxTeleports) return ETeleportsLoad::TooMany;
			table.items.push_back(t);
		}
		if (table.items.empty()) return ETeleportsLoad::NoTeleports;
		if (pawnLines != 1) return ETeleportsLoad::NoPawn;
		for (const Teleporter& t : table.items)
		{
			if ((size_t)t.target >= table.items.size()) return ETeleportsLoad::BadLine;	// 짝 번호가 표 밖(줄 번호는 모른다 — 0)
		}
		out.items.swap(table.items);
		out.meshBaseZ = table.meshBaseZ;
		out.standHalfHeight = table.standHalfHeight;
		return ETeleportsLoad::Ok;
	}

	const char* TeleportsLoadName(ETeleportsLoad result)
	{
		switch (result)
		{
		case ETeleportsLoad::Ok: return "ok";
		case ETeleportsLoad::BadLine: return "bad-line";
		case ETeleportsLoad::NoTeleports: return "no-teleports";
		case ETeleportsLoad::TooMany: return "too-many";
		case ETeleportsLoad::NoPawn: return "no-pawn";
		}
		return "?";
	}

	bool TeleportOverlaps(const Teleporter& teleporter, const Vec3& capsuleCenter, double capsuleRadius, double capsuleHalfHeight)
	{
		// 칸 좌표로 옮기면 캡슐 축은 여전히 세로(칸 z 축 = 월드 위) — 축 선분과 상자의 거리는 세 축 따로 남는 몫의 길이
		const double axisHalf = capsuleHalfHeight > capsuleRadius ? capsuleHalfHeight - capsuleRadius : 0.0;
		const Vec3 d = capsuleCenter - teleporter.boxCenter;
		const double ex = std::fmax(std::fabs(Dot(d, teleporter.boxAxisX)) - teleporter.boxExtent.x, 0.0);
		const double ey = std::fmax(std::fabs(Dot(d, teleporter.boxAxisY)) - teleporter.boxExtent.y, 0.0);
		const double ez = std::fmax(std::fabs(d.z) - axisHalf - teleporter.boxExtent.z, 0.0);
		return ex * ex + ey * ey + ez * ez < capsuleRadius * capsuleRadius;
	}

	uint8_t TeleportMask(const TeleportSet& teleports, const Vec3& capsuleCenter, double capsuleRadius, double capsuleHalfHeight)
	{
		uint8_t mask = 0;
		for (size_t i = 0; i < teleports.count && i < kMaxTeleports; ++i)
		{
			if (TeleportOverlaps(teleports.items[i], capsuleCenter, capsuleRadius, capsuleHalfHeight)) mask |= (uint8_t)(1u << i);
		}
		return mask;
	}

	Vec3 TeleportDestination(const Teleporter& teleporter, const Vec3& capsuleCenter, double meshZ)
	{
		if (!teleporter.keepRelative) return teleporter.exit;
		// KeepReleativeEntryUponExit — 메시 세상 자리를 들어간 장치 뿌리 면(법선 = 뿌리 앞방향)에 투영(ProjectPointOnToPlane)하고 뿌리를 뺀다.
		// 위아래만 [0, TeleporterHeight − 100] 로 자르고 돌리지 않은 채 나갈 자리에 더한다(t-1 그래프 · t-2 기록 문 포털 16번)
		const Vec3 entry = capsuleCenter + Vec3(0, 0, meshZ);
		const Vec3& n = teleporter.rootForward;
		const Vec3 onPlane = entry - n * Dot(entry - teleporter.root, n);
		const Vec3 v = onPlane - teleporter.root;
		const double z = std::fmin(std::fmax(v.z, 0.0), teleporter.offsetZMax);
		return teleporter.exit + Vec3(v.x, v.y, z);
	}

	double TeleportExitYaw(const Teleporter& teleporter)
	{
		return std::atan2(teleporter.exitForward.y, teleporter.exitForward.x) * (180.0 / kPi);
	}

	Vec3 TeleportLaunch(const Teleporter& teleporter)
	{
		return teleporter.exitForward * teleporter.launchMultiplier;
	}

	TeleportStep ApplyTeleports(CMoveSim& sim, MoveState& state, const TeleportSet& teleports, double capsuleRadius, Vec3& outLaunch)
	{
		TeleportStep step;
		if (teleports.count == 0) return step;
		const uint8_t now = TeleportMask(teleports, state.location, capsuleRadius, sim.HalfHeightOf(state));
		const uint8_t began = (uint8_t)(now & ~state.teleportMask);
		state.teleportMask = now;
		for (size_t i = 0; i < teleports.count && i < kMaxTeleports; ++i)
		{
			if (!(began & (1u << i))) continue;
			++step.begins;
			// 둘이 같은 걸음에 시작하면 Lyra 사건 차례는 모른다 — 표 차례 첫째(L_Expanse 는 가장 가까운 두 칸도 중심 사이 1,150cm 라 몸 하나가 둘에 못 든다)
			if (step.device < 0) step.device = (int)i;
		}
		if (step.device < 0) return step;

		const Teleporter& t = teleports.items[step.device];
		const double meshZ = teleports.meshBaseZ + (state.crouched ? sim.CrouchMeshLiftZ() : 0.0);
		state.location = TeleportDestination(t, state.location, meshZ);
		sim.OnTeleported(state);
		// Teleport 는 옮긴 자리에서 겹침을 다시 센다 — 들어간 칸은 끝나고, 옮긴 자리 칸만 남는다(L_Expanse 나갈 자리는 다 칸 밖 — MoveCheck TELE_TABLE)
		state.teleportMask = TeleportMask(teleports, state.location, capsuleRadius, sim.HalfHeightOf(state));
		step.yaw = TeleportExitYaw(t);
		step.forceYaw = t.forceDirection;
		if (t.launchOnTeleport)
		{
			step.launch = true;
			outLaunch = TeleportLaunch(t);
		}
		return step;
	}

	void ResetTeleports(MoveState& state, const TeleportSet& teleports, double capsuleRadius, double capsuleHalfHeight)
	{
		state.teleportMask = teleports.count ? TeleportMask(teleports, state.location, capsuleRadius, capsuleHalfHeight) : 0;
	}

	int MatchTeleportExit(const TeleportSet& teleports, const Vec3& location, double capsuleRadius, double tolerance)
	{
		for (size_t i = 0; i < teleports.count && i < kMaxTeleports; ++i)
		{
			const Teleporter& t = teleports.items[i];
			const Vec3 o = location - t.exit;
			if (!t.keepRelative)
			{
				if (Length(o) <= tolerance) return (int)i;
				continue;
			}
			// o = (v.x, v.y, 자른 v.z) · v 는 뿌리 면 위 — 뿌리 앞방향이 수평이라(읽기에서 거름) 자른 z 와 상관없이 o 의 앞방향 몫은 0
			if (o.z < -tolerance || o.z > t.offsetZMax + tolerance) continue;
			const Vec3& n = t.rootForward;
			if (std::fabs(o.x * n.x + o.y * n.y) > tolerance) continue;
			// 옆 몫 상한 — 들어간 캡슐은 칸과 겹쳤으므로 그 중심은 칸 중심에서 수평 (칸 반대각 + 반지름) 안 · 뿌리 면 투영은 뿌리와의 수평 거리를 늘리지 않는다
			const double ext = std::sqrt(t.boxExtent.x * t.boxExtent.x + t.boxExtent.y * t.boxExtent.y);
			const double rootGap = std::sqrt((t.boxCenter.x - t.root.x) * (t.boxCenter.x - t.root.x) + (t.boxCenter.y - t.root.y) * (t.boxCenter.y - t.root.y));
			if (std::sqrt(o.x * o.x + o.y * o.y) > ext + capsuleRadius + rootGap + tolerance) continue;
			return (int)i;
		}
		return -1;
	}

	bool TeleportToWire(const Teleporter& teleporter, size_t index, size_t count, double meshBaseZ, wire::TeleportInfo& out)
	{
		if (teleporter.name.empty() || teleporter.name.size() > kMaxTeleportName || index >= count || count > kMaxTeleports) return false;
		if (teleporter.target < 0 || (size_t)teleporter.target >= count || (size_t)teleporter.target == index) return false;
		out = wire::TeleportInfo();
		out.h.size = (uint16_t)sizeof(out);
		out.h.type = (uint16_t)wire::Msg::S2C_TELEPORT;
		out.index = (uint8_t)index;
		out.count = (uint8_t)count;
		out.target = (uint8_t)teleporter.target;
		out.flags = (uint8_t)((teleporter.keepRelative ? wire::TELEPORT_KEEP_RELATIVE : 0) | (teleporter.launchOnTeleport ? wire::TELEPORT_LAUNCH : 0) |
			(teleporter.forceDirection ? wire::TELEPORT_FORCE_DIRECTION : 0));
		ToArray(teleporter.boxCenter, out.boxCenter);
		ToArray(teleporter.boxAxisX, out.boxAxisX);
		ToArray(teleporter.boxAxisY, out.boxAxisY);
		ToArray(teleporter.boxExtent, out.boxExtent);
		ToArray(teleporter.root, out.root);
		ToArray(teleporter.rootForward, out.rootForward);
		ToArray(teleporter.exit, out.exitLocation);
		ToArray(teleporter.exitForward, out.exitForward);
		out.launchMultiplier = teleporter.launchMultiplier;
		out.offsetZMax = teleporter.offsetZMax;
		out.meshBaseZ = meshBaseZ;
		std::memcpy(out.name, teleporter.name.data(), teleporter.name.size());	// 나머지는 위 초기화로 0 — 0 으로 끝난다
		return true;
	}

	bool TeleportFromWire(const wire::TeleportInfo& in, Teleporter& out, double& outMeshBaseZ)
	{
		if ((in.flags & ~(wire::TELEPORT_KEEP_RELATIVE | wire::TELEPORT_LAUNCH | wire::TELEPORT_FORCE_DIRECTION)) != 0) return false;
		if (in.count == 0 || in.count > kMaxTeleports || in.index >= in.count || in.target >= in.count || in.target == in.index) return false;
		if (!std::isfinite(in.meshBaseZ)) return false;
		size_t nameLength = 0;
		while (nameLength < sizeof(in.name) && in.name[nameLength] != '\0') ++nameLength;
		if (nameLength == 0 || nameLength == sizeof(in.name)) return false;
		Teleporter t;
		t.name.assign(in.name, nameLength);
		t.target = in.target;
		t.boxCenter = FromArray(in.boxCenter);
		t.boxAxisX = FromArray(in.boxAxisX);
		t.boxAxisY = FromArray(in.boxAxisY);
		t.boxExtent = FromArray(in.boxExtent);
		t.root = FromArray(in.root);
		t.rootForward = FromArray(in.rootForward);
		t.exit = FromArray(in.exitLocation);
		t.exitForward = FromArray(in.exitForward);
		t.keepRelative = (in.flags & wire::TELEPORT_KEEP_RELATIVE) != 0;
		t.launchOnTeleport = (in.flags & wire::TELEPORT_LAUNCH) != 0;
		t.launchMultiplier = in.launchMultiplier;
		t.forceDirection = (in.flags & wire::TELEPORT_FORCE_DIRECTION) != 0;
		t.offsetZMax = in.offsetZMax;
		if (!ShapeOk(t)) return false;
		out = t;
		outMeshBaseZ = in.meshBaseZ;
		return true;
	}
}
