// 서버 시험 클라 — 서버를 TCP 로 붙어서 본다. 서버 코드를 부르지 않는다.
//
// 서버는 --debug --warmup=0 으로 띄운다(경기 전 대기 중에는 피해가 없다).
//   ServerProbe <map.mocl> <move.csv> [--port=7791] [--rows=N] [--breakMirror] [--breakSpread] [--breakDamage]
//   ServerProbe <map.mocl> <move.csv> --rules [--port=7791]		서버는 --debug --warmup=3 --targetScore=2
//   ServerProbe <map.mocl> <move.csv> --checkStarts=<weapons_match.txt>		서버 없이
//   ServerProbe <map.mocl> <move.csv> --authority --serverLog=<서버 출력 파일>	서버는 --warmup=0 --exitWhenEmpty, --debug 없이
//   ServerProbe <map.mocl> <move.csv> --rewind --serverLog=<서버 출력 파일>	서버는 --debug --rewindShadow --warmup=0 --exitWhenEmpty
//   ServerProbe <map.mocl> <move.csv> --pads --serverLog=<서버 출력 파일>	서버는 --debug --warmup=0 --exitWhenEmpty --pads=<받침 파일>
//      → 출력을 Tools/LyraPilot/check_pads.py 가 Lyra 받침 판 기록과 대조한다
//   ServerProbe <map_gym.mocl> <move.csv> --melee=<cprobe_<tag>.log> --serverLog=<서버 출력 파일>	서버는 --debug --warmup=0 --exitWhenEmpty [--clips=<클립 파일>]
//      → Lyra 연습장 근접 칸을 서버에 돌려 결과 · 대상 · 피해 · 밀린 거리 · 손을 견준다(7번, 컨텐츠 E-3). 기록 표는 같은 이름의 .csv
//
// 1. 재생: Lyra 이동 기록의 입력에 무기 입력을 섞어 보내고, 돌아온 상태를 같은 공용 코드(Shared)를 여기서 돌린 결과와 비교한다.
//    서버가 입력을 틱마다 하나씩 빠짐없이 같은 순서로 썼다면 float 까지 똑같다. Lyra 와의 차이도 같이 적는다.
//    탄도 같은 시드로 굴려 맞은 점까지 비교한다 — 서버가 쏜 첫 접속이어야 난수가 맞으므로 재생을 먼저 돌린다.
// 2. 사격: 세 명을 세워 놓고 쏴서 연사 간격 · 피해 수치 · 체력 합 · 같은 팀 피해 0 을 본다.
// 3. 표: 서버가 쓰는 이동 값 ↔ 기록 머리의 Lyra 실행값.
//
// 4. 경기 규칙(--rules): 대기 중 피해 없음 · 시작 때 모두 다시 세움 · 부활 무적·이동 막음 · 사망 중 입력 거절 · 부활 시간 · 낙사 · 승리 · 경기 끝 사격 막힘 · 결과.
// 5. 시작 자리(--checkStarts): 표 ↔ 덤프, 자리마다 맵과 겹치나 · 떨어뜨렸을 때 어디 서나.
//
// 일부러 틀리기: --breakMirror 는 여기 쪽 가속을, --breakSpread 는 여기 쪽 퍼짐을 「이번 발의 열이 더해진 뒤」 값으로 바꿔 1번이,
// --breakDamage 는 여기 쪽 기대 피해를 바꿔 2번이 실패해야 한다.
#include "GameProtocol.h"
#include "Grenade.h"
#include "MapCollision.h"
#include "MatchRules.h"
#include "Movement.h"
#include "NetClient.h"
#include "Weapon.h"

#include <timeapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winmm.lib")

using namespace mo;

namespace
{
	using Clock = std::chrono::steady_clock;
	constexpr float kTick = 1.f / 60.f;
	constexpr int kModeWalking = 1;
	constexpr double kPi = 3.14159265358979323846;

	double Length(const Vec3& v)
	{
		return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
	}

	Vec3 AimDirection(float yawDegrees, float pitchDegrees)
	{
		const double yaw = yawDegrees * kPi / 180.0;
		const double pitch = pitchDegrees * kPi / 180.0;
		return Vec3(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch));
	}

	struct Row
	{
		int seg = 0, ev = 0, modeBefore = 0;
		float ix = 0, iy = 0;
		Vec3 p, v, after;
	};

	bool ReadMoveCsv(const char* path, std::vector<Row>& rows, std::map<std::string, double>& params, size_t limit)
	{
		FILE* file = std::fopen(path, "rb");
		if (!file) return false;
		char line[4096];
		while (std::fgets(line, sizeof(line), file))
		{
			if (line[0] == '#')
			{
				char name[128];
				double value = 0;
				if (std::sscanf(line, "#param %127s %lf", name, &value) == 2) params[name] = value;
				continue;
			}
			if (line[0] < '0' || line[0] > '9') continue;
			if (rows.size() >= limit) continue;
			char* s = line;
			const auto D = [&]() { const double v = std::strtod(s, &s); if (*s == ',') ++s; return v; };
			Row r;
			D();
			r.seg = (int)D();
			r.ev = (int)D();
			D();
			r.ix = (float)D();
			r.iy = (float)D();
			D();
			r.p.x = D(); r.p.y = D(); r.p.z = D();
			r.v.x = D(); r.v.y = D(); r.v.z = D();
			r.modeBefore = (int)D();
			D();
			D(); D(); D();
			D(); D(); D(); D(); D(); D(); D();
			r.after.x = D(); r.after.y = D(); r.after.z = D();
			rows.push_back(r);
		}
		std::fclose(file);
		return true;
	}

	bool LoadMap(const char* path, CMapCollision& map)
	{
		FILE* file = std::fopen(path, "rb");
		if (!file) return false;
		std::fseek(file, 0, SEEK_END);
		const long size = std::ftell(file);
		std::fseek(file, 0, SEEK_SET);
		std::vector<char> data((size_t)size);
		const size_t read = std::fread(data.data(), 1, data.size(), file);
		std::fclose(file);
		return read == data.size() && map.LoadFromMemory(data.data(), data.size()) == EMapLoad::Ok;
	}

	bool ConnectAndHello(CNetClient& client, int port, wire::Welcome& welcome, std::vector<wire::Shot>* shots = nullptr)
	{
		(void)shots;
		bool connected = false;
		for (int i = 0; i < 300 && !connected; ++i)
		{
			connected = client.Connect("127.0.0.1", port);
			if (!connected) std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		if (!connected) return false;
		wire::Hello hello = wire::Hello();
		hello.protocolVersion = wire::kProtocolVersion;
		if (!client.SendPacket(hello, wire::Msg::C2S_HELLO)) return false;
		bool got = false;
		for (int i = 0; i < 3000 && !got; ++i)
		{
			client.Poll([&](const wire::Header& header, const char* data, int size)
			{
				if ((wire::Msg)header.type == wire::Msg::S2C_WELCOME && size == (int)sizeof(welcome))
				{
					std::memcpy(&welcome, data, sizeof(welcome));
					got = true;
				}
			});
			if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return got;
	}

	bool ReadPlayer(const char* data, int size, uint32_t id, wire::PlayerState& out)
	{
		if (size < (int)offsetof(wire::State, players)) return false;
		uint8_t count = 0;
		std::memcpy(&count, data + offsetof(wire::State, count), 1);
		for (int i = 0; i < count; ++i)
		{
			wire::PlayerState p;
			std::memcpy(&p, data + offsetof(wire::State, players) + sizeof(wire::PlayerState) * i, sizeof(p));
			if (p.id == id)
			{
				out = p;
				return true;
			}
		}
		return false;
	}

	// ─── 표 ────────────────────────────────────────────────
	bool CheckParams(const MoveParams& p, const std::map<std::string, double>& values)
	{
		struct Item { const char* name; double mine; };
		const Item items[] = {
			{ "MaxWalkSpeed", p.maxWalkSpeed }, { "MaxAcceleration", p.maxAcceleration }, { "GroundFriction", p.groundFriction },
			{ "BrakingDecelerationWalking", p.brakingDecelerationWalking }, { "BrakingDecelerationFalling", p.brakingDecelerationFalling },
			{ "BrakingFriction", p.brakingFriction }, { "BrakingFrictionFactor", p.brakingFrictionFactor }, { "bUseSeparateBrakingFriction", p.useSeparateBrakingFriction ? 1.0 : 0.0 },
			{ "BrakingSubStepTime", p.brakingSubStepTime }, { "GravityZ", p.gravityZ }, { "JumpZVelocity", p.jumpZVelocity }, { "AirControl", p.airControl },
			{ "AirControlBoostMultiplier", p.airControlBoostMultiplier }, { "AirControlBoostVelocityThreshold", p.airControlBoostVelocityThreshold }, { "FallingLateralFriction", p.fallingLateralFriction },
			{ "MaxStepHeight", p.maxStepHeight }, { "WalkableFloorZ", p.walkableFloorZ }, { "PerchRadiusThreshold", p.perchRadiusThreshold }, { "PerchAdditionalHeight", p.perchAdditionalHeight },
			{ "MaxSimulationTimeStep", p.maxSimulationTimeStep }, { "MaxSimulationIterations", (double)p.maxSimulationIterations }, { "MaxJumpApexAttemptsPerSimulation", (double)p.maxJumpApexAttemptsPerSimulation },
			{ "bApplyGravityWhileJumping", p.applyGravityWhileJumping ? 1.0 : 0.0 }, { "bDontFallBelowJumpZVelocityDuringJump", p.dontFallBelowJumpZVelocityDuringJump ? 1.0 : 0.0 },
			{ "MaxDepenetrationWithGeometry", p.maxDepenetrationWithGeometry }, { "TerminalVelocity", p.terminalVelocity }, { "JumpMaxHoldTime", p.jumpMaxHoldTime }, { "JumpMaxCount", (double)p.jumpMaxCount },
			{ "MinAnalogWalkSpeed", p.minAnalogWalkSpeed }, { "CapsuleRadius", p.capsuleRadius }, { "CapsuleHalfHeight", p.capsuleHalfHeight },
		};
		// 앉기 인자 — 앉기 기록(컨텐츠 D-1, move_expanse_crouch.csv)부터 머리에 실린다. 그 전 기록에는 없으니 있을 때만 본다
		//   bCanWalkOffLedgesWhenCrouching 은 서버에 칸이 없다 — 앉아도 턱에서 떨어질 수 있다(1)고 보고 옮겼으므로 1 과 대조한다
		const Item crouchItems[] = {
			{ "MaxWalkSpeedCrouched", p.maxWalkSpeedCrouched }, { "CrouchedHalfHeight", p.crouchedHalfHeight }, { "bCanWalkOffLedgesWhenCrouching", 1.0 },
		};
		int checked = 0, missing = 0, diff = 0;
		const auto check = [&](const Item& item)
		{
			const auto it = values.find(item.name);
			if (it == values.end())
			{
				++missing;
				std::printf("PARAM 기록에 없음 %s\n", item.name);
				return;
			}
			++checked;
			if (std::fabs((float)it->second - (float)item.mine) > 0.f)
			{
				++diff;
				std::printf("PARAM 다름 %s lyra=%.8f mine=%.8f\n", item.name, it->second, item.mine);
			}
		};
		for (const Item& item : items) check(item);
		if (values.count("CrouchedHalfHeight"))
		{
			for (const Item& item : crouchItems) check(item);
		}
		else
		{
			std::printf("PARAM 앉기 인자 건너뜀 — 앉기 전 기록\n");
		}
		std::printf("PARAM checked=%d missing=%d diff=%d\n", checked, missing, diff);
		return checked > 0 && missing == 0 && diff == 0;
	}

	// ─── 1. 재생 ───────────────────────────────────────────
	struct Expected
	{
		float x, y, z, vx, vy, vz;
		int16_t magazine, spare;
		uint8_t weapon;
		uint8_t reloading;
		bool falling;
		Vec3 lyra;
		bool hasLyra;
	};

	// 재현 가능한 무기 입력 각본. 기록의 움직임과 섞어 움직이면서 쏠 때 퍼짐·재장전 분기까지 지나가게 한다
	class CWeaponScript
	{
	public:
		explicit CWeaponScript(uint32_t seed) : _rng(seed) {}

		void Next(uint8_t& buttons, uint8_t& weapon)
		{
			if (_fireLeft <= 0)
			{
				_fireHeld = !_fireHeld;
				_fireLeft = 10 + (int)(_rng.NextFloat() * (_fireHeld ? 120.f : 60.f));
			}
			--_fireLeft;
			if (_fireHeld) buttons |= wire::BUTTON_FIRE;
			if (_rng.NextFloat() < 1.f / 200.f) buttons |= wire::BUTTON_RELOAD;
			if (_rng.NextFloat() < 1.f / 500.f) _weapon = (uint8_t)((_weapon + 1) % (int)EWeapon::Count);
			weapon = _weapon;
		}

	private:
		CSpreadRng _rng;
		bool _fireHeld = false;
		int _fireLeft = 0;
		uint8_t _weapon = 0;
	};

	struct ExpectedBullet
	{
		float x, y, z;
		uint8_t hit;
	};

	bool RunReplay(int port, const CMapCollision& map, const MoveParams& baseParams, const std::vector<Row>& rows, bool breakMirror, bool breakSpread)
	{
		MoveParams params = baseParams;
		if (breakMirror) params.maxAcceleration += 1.f;
		CMoveSim sim(map, params);
		MoveState state;
		state.maxSpeed = params.maxWalkSpeed;
		std::array<CWeaponCycle, (size_t)EWeapon::Count> weapons;
		for (size_t i = 0; i < weapons.size(); ++i) weapons[i].Init((EWeapon)i);
		CWeaponFireGate fireGate;
		uint8_t currentWeapon = 0;
		bool jumpHeld = false;

		CNetClient client;
		wire::Welcome welcome = wire::Welcome();
		if (!ConnectAndHello(client, port, welcome))
		{
			std::printf("REPLAY 접속 실패\n");
			return false;
		}
		if (!welcome.debugAllowed)
		{
			std::printf("REPLAY 서버가 --debug 없이 떴다\n");
			return false;
		}

		std::vector<Expected> expected(rows.size() + 128);
		std::vector<bool> known(expected.size(), false);
		CWeaponScript script(20261102);
		std::vector<ExpectedBullet> expectedBullets;
		size_t bulletCursor = 0;
		uint64_t bulletsCompared = 0, bulletsDiff = 0;
		double bulletMax = 0;

		uint32_t seq = 0;
		size_t index = 0;
		int currentSeg = -1;
		uint64_t compared = 0, posDiff = 0, velDiff = 0, ammoDiff = 0, weaponDiff = 0, flagDiff = 0;
		double posMax = 0, velMax = 0;
		uint64_t firstBadSeq = 0;
		uint32_t lastChecked = 0;
		uint64_t statesSeen = 0, fired = 0, reloaded = 0;
		double lyraMax = 0, lyraSum = 0;
		uint64_t lyraCount = 0;
		bool open = true;
		const int kLead = 6;	// 0.1초 앞서 보낸다 — 서버가 입력을 굶지 않게

		const auto onPacket = [&](const wire::Header& header, const char* data, int size)
		{
			if ((wire::Msg)header.type == wire::Msg::S2C_SHOT)
			{
				wire::Shot shot = wire::Shot();
				std::memcpy(&shot, data, std::min(size, (int)sizeof(shot)));
				for (int b = 0; b < shot.count; ++b)
				{
					// 입력을 먼저 돌리고 보내므로 기대값이 언제나 먼저 있다
					if (bulletCursor >= expectedBullets.size())
					{
						++bulletsDiff;
						continue;
					}
					const ExpectedBullet& e = expectedBullets[bulletCursor++];
					const wire::ShotBullet& got = shot.bullets[b];
					const double dx = (double)got.x - e.x, dy = (double)got.y - e.y, dz = (double)got.z - e.z;
					const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
					bulletMax = std::max(bulletMax, d);
					++bulletsCompared;
					if (d > 0 || got.hit != e.hit) ++bulletsDiff;
				}
				return;
			}
			if ((wire::Msg)header.type != wire::Msg::S2C_STATE) return;
			++statesSeen;
			wire::PlayerState p;
			if (!ReadPlayer(data, size, welcome.playerId, p)) return;
			if (p.lastSeq <= lastChecked || p.lastSeq >= known.size() || !known[p.lastSeq]) return;
			lastChecked = p.lastSeq;
			const Expected& e = expected[p.lastSeq];
			++compared;
			const double dp = std::sqrt((double)(p.x - e.x) * (p.x - e.x) + (double)(p.y - e.y) * (p.y - e.y) + (double)(p.z - e.z) * (p.z - e.z));
			const double dv = std::sqrt((double)(p.vx - e.vx) * (p.vx - e.vx) + (double)(p.vy - e.vy) * (p.vy - e.vy) + (double)(p.vz - e.vz) * (p.vz - e.vz));
			posMax = std::max(posMax, dp);
			velMax = std::max(velMax, dv);
			bool bad = false;
			if (dp > 0) { ++posDiff; bad = true; }
			if (dv > 0) { ++velDiff; bad = true; }
			if (p.magazine != e.magazine || p.spare != e.spare) { ++ammoDiff; bad = true; }
			if (p.weapon != e.weapon) { ++weaponDiff; bad = true; }
			if (((p.flags & wire::STATE_FALLING) != 0) != e.falling || ((p.flags & wire::STATE_RELOADING) != 0) != (e.reloading != 0)) { ++flagDiff; bad = true; }
			if (bad && firstBadSeq == 0) firstBadSeq = p.lastSeq;
		};

		timeBeginPeriod(1);
		const auto start = Clock::now();
		uint64_t inputsSent = 0;
		while (index < rows.size() && open)
		{
			const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
			const uint64_t target = (uint64_t)(elapsed * 60.0) + kLead;
			while (inputsSent < target && index < rows.size())
			{
				const Row& r = rows[index];
				if (r.seg != currentSeg)
				{
					// 구간 시작 = Lyra 기록과 같은 자리로 옮긴다
					currentSeg = r.seg;
					wire::DebugPlace place = wire::DebugPlace();
					place.seq = ++seq;
					place.x = r.p.x; place.y = r.p.y; place.z = r.p.z;
					place.vx = r.v.x; place.vy = r.v.y; place.vz = r.v.z;
					place.hp = 100.f;
					place.falling = r.modeBefore == kModeWalking ? 0 : 1;
					client.SendPacket(place, wire::Msg::C2S_DEBUG_PLACE);

					state = MoveState();
					state.location = r.p;
					state.velocity = r.v;
					state.mode = place.falling ? EMoveMode::Falling : EMoveMode::Walking;
					state.maxSpeed = params.maxWalkSpeed;
					sim.RefreshFloor(state);
					jumpHeld = false;
					// 번호만 쓰고 비교하지 않는다 — 옮긴 틱의 상태는 입력 전이다
				}

				wire::Input input = wire::Input();
				input.seq = ++seq;
				input.moveX = r.ix;
				input.moveY = r.iy;
				input.yaw = 90.f;
				input.pitch = 80.f;		// 하늘로 쏜다 — 사격 판정은 2번에서 본다
				input.aimX = (float)state.location.x;	// 정직한 클라처럼 내 자리(움직이기 전)에서 — 기록 자리는 같은 코드와 최대 67cm 다르다
				input.aimY = (float)state.location.y;
				input.aimZ = (float)state.location.z + 60.f;
				if (r.ev == 1) jumpHeld = true;
				else if (r.ev == 2) jumpHeld = false;
				if (jumpHeld) input.buttons |= wire::BUTTON_JUMP;
				script.Next(input.buttons, input.weapon);
				// 서버와 같은 순서로 여기서도 돌린다(Room.cpp ApplyInput). 서버 답이 앞지르지 않게 보내기 전에 돌린다
				if (input.weapon != currentWeapon)
				{
					currentWeapon = input.weapon;
					weapons[currentWeapon].Equip();
				}
				if (r.ev == 1) sim.PressJump(state);
				else if (r.ev == 2) sim.ReleaseJump(state);
				state.maxSpeed = params.maxWalkSpeed;
				sim.Tick(state, Vec3(input.moveX, input.moveY, 0), kTick);
				WeaponMotion motion;
				motion.speed = (float)Length(state.velocity);
				motion.falling = state.mode == EMoveMode::Falling;
				WeaponInput weaponInput;
				weaponInput.fireHeld = (input.buttons & wire::BUTTON_FIRE) != 0;
				weaponInput.reloadPressed = (input.buttons & wire::BUTTON_RELOAD) != 0;
				fireGate.NextInput();
				if (weaponInput.fireHeld && fireGate.IsBlocked((EWeapon)currentWeapon, weapons[currentWeapon].IsFireActive())) weaponInput.fireHeld = false;
				const WeaponEvents events = weapons[currentWeapon].Tick(kTick, weaponInput, motion);
				if (events.fired)
				{
					fireGate.OnFired((EWeapon)currentWeapon, weapons[currentWeapon].GetDef(), kTick);
					++fired;
					const WeaponDef& def = GetWeaponDef((EWeapon)currentWeapon);
					const Vec3 aimStart(input.aimX, input.aimY, input.aimZ);
					const Vec3 aimDir = AimDirection(input.yaw, input.pitch);
					const float angle = breakSpread ? weapons[currentWeapon].GetSpread().GetSpreadAngle() : events.spreadAngle;
					const float multiplier = breakSpread ? weapons[currentWeapon].GetSpread().GetCalculatedMultiplier() : events.spreadMultiplier;
					for (int b = 0; b < def.bulletsPerCartridge; ++b)
					{
						CSpreadRng bulletRng(SpreadSeed(input.seq, (uint32_t)b));
						const Vec3 dir = SpreadDirection(aimDir, angle, multiplier, def.spreadExponent, bulletRng);
						const Vec3 end = aimStart + dir * (double)def.maxDamageRange;
						BulletResult result;
						ResolveBullet(map, nullptr, 0, aimStart, end, def.sweepRadius, result);
						const Vec3 point = result.kind == EBulletHit::None ? end : result.point;
						ExpectedBullet eb;
						eb.x = (float)point.x; eb.y = (float)point.y; eb.z = (float)point.z;
						eb.hit = (uint8_t)(result.kind == EBulletHit::None ? wire::EShotHit::None : result.kind == EBulletHit::Map ? wire::EShotHit::Map : wire::EShotHit::Body);
						expectedBullets.push_back(eb);
					}
				}
				if (events.reloaded) ++reloaded;

				if (seq >= expected.size())
				{
					expected.resize(seq + 1024);
					known.resize(expected.size(), false);
				}
				Expected& e = expected[seq];
				e.x = (float)state.location.x; e.y = (float)state.location.y; e.z = (float)state.location.z;
				e.vx = (float)state.velocity.x; e.vy = (float)state.velocity.y; e.vz = (float)state.velocity.z;
				e.magazine = (int16_t)weapons[currentWeapon].GetMagazine();
				e.spare = (int16_t)weapons[currentWeapon].GetSpare();
				e.weapon = currentWeapon;
				e.reloading = weapons[currentWeapon].IsReloading() ? 1 : 0;
				e.falling = state.mode == EMoveMode::Falling;
				known[seq] = true;

				if (!client.SendPacket(input, wire::Msg::C2S_INPUT))
				{
					open = false;
					break;
				}

				const double dx = state.location.x - r.after.x, dy = state.location.y - r.after.y, dz = state.location.z - r.after.z;
				const double lyraDist = std::sqrt(dx * dx + dy * dy + dz * dz);
				lyraMax = std::max(lyraMax, lyraDist);
				lyraSum += lyraDist;
				++lyraCount;

				++inputsSent;
				++index;
			}
			if (!client.Poll(onPacket)) open = false;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		for (int i = 0; i < 2000 && open && lastChecked < seq; ++i)
		{
			if (!client.Poll(onPacket)) open = false;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		for (int i = 0; i < 100 && open; ++i)	// 마지막 입력의 탄 알림까지
		{
			if (!client.Poll(onPacket)) open = false;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		timeEndPeriod(1);
		client.Close();

		std::printf("REPLAY rows=%zu segs=%d inputsSent=%llu statesSeen=%llu compared=%llu lastChecked=%u lastSeq=%u\n", rows.size(), currentSeg + 1, (unsigned long long)inputsSent, (unsigned long long)statesSeen, (unsigned long long)compared, lastChecked, seq);
		std::printf("REPLAY 서버 ↔ 같은 코드: posDiff=%llu posMax=%.5f velDiff=%llu velMax=%.5f ammoDiff=%llu weaponDiff=%llu flagDiff=%llu firstBadSeq=%llu | 각본 발사=%llu 재장전=%llu\n",
			(unsigned long long)posDiff, posMax, (unsigned long long)velDiff, velMax, (unsigned long long)ammoDiff, (unsigned long long)weaponDiff, (unsigned long long)flagDiff, (unsigned long long)firstBadSeq, (unsigned long long)fired, (unsigned long long)reloaded);
		std::printf("REPLAY 탄 서버 ↔ 같은 코드: 기대=%zu compared=%llu diff=%llu max=%.5f\n", expectedBullets.size(), (unsigned long long)bulletsCompared, (unsigned long long)bulletsDiff, bulletMax);
		std::printf("REPLAY 같은 코드 ↔ Lyra 기록(참고): max=%.3f avg=%.3f (구간 첫 자리만 맞추고 이어 달림)\n", lyraMax, lyraCount ? lyraSum / (double)lyraCount : 0.0);

		return compared > 0 && lastChecked == seq && fired > 0 && reloaded > 0 && posDiff == 0 && velDiff == 0 && ammoDiff == 0 && weaponDiff == 0 && flagDiff == 0
			&& bulletsCompared > 0 && bulletsCompared == expectedBullets.size() && bulletsDiff == 0;
	}

	// ─── 2. 사격 ───────────────────────────────────────────
	struct ShotClient
	{
		CNetClient net;
		wire::Welcome welcome = wire::Welcome();
		std::vector<wire::Shot> shots;
		std::vector<wire::Damage> damages;
		std::vector<wire::Spawn> spawns;
		std::vector<wire::Kill> kills;
		std::vector<wire::Match> matches;
		std::vector<wire::Melee> melees;
		std::vector<wire::Pickup> pickups;
		std::vector<wire::Grenade> grenades;		// S2C_GRENADE — 던짐 · 터짐(⑧ 경기 끝 수류탄)
		std::vector<wire::PlayerState> history;
		std::vector<uint32_t> historyTicks;		// history 와 같은 칸 — 그 상태가 어느 틱 것인가
		wire::Result result = wire::Result();
		bool hasResult = false;
		wire::PlayerState self = wire::PlayerState();
		bool hasSelf = false;
		uint32_t seq = 0;

		void Pump()
		{
			net.Poll([this](const wire::Header& header, const char* data, int size)
			{
				switch ((wire::Msg)header.type)
				{
				case wire::Msg::S2C_STATE:
					if (ReadPlayer(data, size, welcome.playerId, self))
					{
						hasSelf = true;
						history.push_back(self);
						uint32_t stateTick = 0;
						std::memcpy(&stateTick, data + offsetof(wire::State, tick), sizeof(stateTick));
						historyTicks.push_back(stateTick);
					}
					break;
				case wire::Msg::S2C_SHOT:
				{
					wire::Shot shot = wire::Shot();
					std::memcpy(&shot, data, std::min(size, (int)sizeof(shot)));
					shots.push_back(shot);
					break;
				}
				case wire::Msg::S2C_DAMAGE:
				{
					wire::Damage damage = wire::Damage();
					std::memcpy(&damage, data, std::min(size, (int)sizeof(damage)));
					damages.push_back(damage);
					break;
				}
				case wire::Msg::S2C_SPAWN:
				{
					wire::Spawn spawn = wire::Spawn();
					std::memcpy(&spawn, data, std::min(size, (int)sizeof(spawn)));
					spawns.push_back(spawn);
					break;
				}
				case wire::Msg::S2C_KILL:
				{
					wire::Kill kill = wire::Kill();
					std::memcpy(&kill, data, std::min(size, (int)sizeof(kill)));
					kills.push_back(kill);
					break;
				}
				case wire::Msg::S2C_MATCH:
				{
					wire::Match match = wire::Match();
					std::memcpy(&match, data, std::min(size, (int)sizeof(match)));
					matches.push_back(match);
					break;
				}
				case wire::Msg::S2C_MELEE:
				{
					wire::Melee melee = wire::Melee();
					std::memcpy(&melee, data, std::min(size, (int)sizeof(melee)));
					melees.push_back(melee);
					break;
				}
				case wire::Msg::S2C_PICKUP:
				{
					wire::Pickup pickup = wire::Pickup();
					std::memcpy(&pickup, data, std::min(size, (int)sizeof(pickup)));
					pickups.push_back(pickup);
					break;
				}
				case wire::Msg::S2C_GRENADE:
				{
					wire::Grenade grenade = wire::Grenade();
					std::memcpy(&grenade, data, std::min(size, (int)sizeof(grenade)));
					grenades.push_back(grenade);
					break;
				}
				case wire::Msg::S2C_RESULT:
					std::memcpy(&result, data, std::min(size, (int)sizeof(result)));
					hasResult = true;
					break;
				default:
					break;
				}
			});
		}

		void Place(const Vec3& at, float hp, uint8_t flags = 0, uint8_t ammoWeapon = 0, int16_t magazine = 0, int16_t spare = 0)
		{
			wire::DebugPlace place = wire::DebugPlace();
			place.seq = ++seq;
			place.x = at.x; place.y = at.y; place.z = at.z;
			place.hp = hp;
			place.flags = flags;
			place.ammoWeapon = ammoWeapon;
			place.magazine = magazine;
			place.spare = spare;
			net.SendPacket(place, wire::Msg::C2S_DEBUG_PLACE);
		}
	};

	void PumpFor(ShotClient* clients, int count, double seconds)
	{
		const auto until = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
		while (Clock::now() < until)
		{
			for (int i = 0; i < count; ++i) clients[i].Pump();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	// 맵 안에서 두 사람이 마주 보고 설 수 있는 자리를 찾는다
	bool FindArena(const CMapCollision& map, const std::vector<Row>& rows, Vec3& outA, Vec3& outB, Vec3& outSide)
	{
		const double kDistance = 800.0;
		for (size_t i = 200; i < rows.size(); i += 97)
		{
			const Vec3 base = rows[i].after;
			if (rows[i].modeBefore != kModeWalking) continue;
			for (int d = 0; d < 8; ++d)
			{
				const double angle = d * kPi / 4.0;
				const Vec3 dir(std::cos(angle), std::sin(angle), 0);
				const Vec3 other = base + dir * kDistance;
				RayHit ray;
				if (map.Raycast(base + Vec3(0, 0, 40), other + Vec3(0, 0, 40), MAP_BLOCK_WEAPON, ray)) continue;
				// 그쪽에 바닥이 있고 높이가 거의 같다
				RayHit floor;
				if (!map.Raycast(other + Vec3(0, 0, 10), other + Vec3(0, 0, -150), MAP_BLOCK_MOVE, floor)) continue;
				if (floor.distance < 95 || floor.distance > 105) continue;
				if (map.OverlapCapsule(other, 35, 90, MAP_BLOCK_MOVE)) continue;
				// 옆으로 비껴 설 자리
				const Vec3 side = base + Vec3(-dir.y, dir.x, 0) * 300.0;
				if (map.OverlapCapsule(side, 35, 90, MAP_BLOCK_MOVE)) continue;
				outA = other;
				outB = base;
				outSide = side;
				return true;
			}
		}
		return false;
	}

	void AimAt(const Vec3& from, const Vec3& to, float& outYaw, float& outPitch)
	{
		const Vec3 d = to - from;
		outYaw = (float)(std::atan2(d.y, d.x) * 180.0 / kPi);
		outPitch = (float)(std::atan2(d.z, std::sqrt(d.x * d.x + d.y * d.y)) * 180.0 / kPi);
	}

	bool RunShots(int port, const CMapCollision& map, const std::vector<Row>& rows, bool breakDamage)
	{
		Vec3 shooterAt, enemyAt, sideAt;
		if (!FindArena(map, rows, shooterAt, enemyAt, sideAt))
		{
			std::printf("SHOTS 설 자리를 못 찾음\n");
			return false;
		}
		ShotClient clients[3];
		for (ShotClient& c : clients)
		{
			if (!ConnectAndHello(c.net, port, c.welcome))
			{
				std::printf("SHOTS 접속 실패\n");
				return false;
			}
		}
		ShotClient& shooter = clients[0];
		ShotClient& enemy = clients[1];
		ShotClient& ally = clients[2];
		if (shooter.welcome.team == enemy.welcome.team || shooter.welcome.team != ally.welcome.team)
		{
			std::printf("SHOTS 팀이 예상과 다름 %u %u %u\n", shooter.welcome.team, enemy.welcome.team, ally.welcome.team);
			return false;
		}

		bool ok = true;
		timeBeginPeriod(1);
		const int kExpectedGap[(int)EWeapon::Count] = { 9, 9, 19 };

		for (int w = 0; w < (int)EWeapon::Count; ++w)
		{
			const WeaponDef& def = GetWeaponDef((EWeapon)w);
			shooter.Place(shooterAt, 100.f);
			enemy.Place(enemyAt, 100.f);
			ally.Place(sideAt, 100.f);
			PumpFor(clients, 3, 0.2);
			for (ShotClient& c : clients)
			{
				c.shots.clear();
				c.damages.clear();
			}

			const Vec3 eye = shooterAt + Vec3(0, 0, 60);
			float yaw = 0, pitch = 0;
			AimAt(eye, enemyAt, yaw, pitch);
			const int kFrames = 120;
			CWeaponCycle mirror;
			mirror.Init((EWeapon)w);
			if (w != 0) mirror.Equip();
			int mirrorShots = 0;
			const auto start = Clock::now();
			int sent = 0;
			while (sent < kFrames)
			{
				const int target = (int)(std::chrono::duration<double>(Clock::now() - start).count() * 60.0) + 6;
				while (sent < target && sent < kFrames)
				{
					wire::Input input = wire::Input();
					input.seq = ++shooter.seq;
					input.yaw = yaw;
					input.pitch = pitch;
					input.aimX = (float)eye.x; input.aimY = (float)eye.y; input.aimZ = (float)eye.z;
					input.buttons = wire::BUTTON_FIRE;
					input.weapon = (uint8_t)w;
					shooter.net.SendPacket(input, wire::Msg::C2S_INPUT);
					WeaponInput weaponInput;
					weaponInput.fireHeld = true;
					if (mirror.Tick(kTick, weaponInput, WeaponMotion()).fired) ++mirrorShots;
					++sent;
				}
				for (ShotClient& c : clients) c.Pump();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			PumpFor(clients, 3, 0.3);

			// 연사 간격 — 발 수·탄창이 본 판정. 틱 간격은 입력이 도착한 시각을 타서(PC 가 바쁘면 8·9·10 으로 흔들린다) 첫 칸(앞서 보낸 6개가 한 틱에 몰린다)을 빼고 평균만 본다
			std::map<int, int> gaps;
			for (size_t i = 2; i < shooter.shots.size(); ++i) ++gaps[(int)(shooter.shots[i].tick - shooter.shots[i - 1].tick)];
			int gapSum = 0, gapCount = 0;
			for (const auto& g : gaps)
			{
				gapSum += g.first * g.second;
				gapCount += g.second;
			}
			const bool gapOk = gapCount > 0 && std::fabs((double)gapSum / gapCount - kExpectedGap[w]) <= 1.0;
			const bool countOk = (int)shooter.shots.size() == mirrorShots && shooter.self.magazine == mirror.GetMagazine() && shooter.self.spare == mirror.GetSpare();

			// 피해 — 탄마다 거리로 다시 계산, 체력 합
			int bullets = 0, bodyHits = 0, weak = 0, damageBad = 0;
			float damageSum = 0;
			double maxDiff = 0;
			for (const wire::Shot& shot : shooter.shots)
			{
				for (int b = 0; b < shot.count; ++b)
				{
					const wire::ShotBullet& bullet = shot.bullets[b];
					++bullets;
					if (bullet.hit != (uint8_t)wire::EShotHit::Body) continue;
					++bodyHits;
					if (bullet.weakSpot) ++weak;
					const double dx = bullet.x - shot.startX, dy = bullet.y - shot.startY, dz = bullet.z - shot.startZ;
					const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
					float mine = ComputeDamage(def, distance, bullet.weakSpot != 0, bullet.target == ally.welcome.playerId);
					if (breakDamage) mine += 1.f;
					const double diff = std::fabs(mine - bullet.damage);
					maxDiff = std::max(maxDiff, diff);
					if (diff > 0.01) ++damageBad;
					damageSum += bullet.damage;
				}
			}
			float damageEventSum = 0;
			float lastHp = 100.f;
			for (const wire::Damage& d : shooter.damages)
			{
				damageEventSum += d.amount;
				lastHp = d.hpAfter;
			}
			const float expectedHp = std::max(100.f - damageEventSum, 0.f);
			const bool hpOk = std::fabs(enemy.self.hp - lastHp) < 0.001f && std::fabs(lastHp - expectedHp) < 0.001f;
			std::string gapText;
			for (const auto& g : gaps) gapText += " " + std::to_string(g.first) + ":" + std::to_string(g.second);
			std::printf("SHOTS %-8s shots=%zu(같은 코드 %d) 간격 틱[%s ] 기대 %d | 탄=%d 몸=%d 약점=%d 피해차 max=%.4f bad=%d | 피해 알림 %zu 합 %.2f 맞힌 탄 합 %.2f 상대 체력 %.2f(기대 %.2f) | 탄창 %d 여분 %d\n",
				def.name, shooter.shots.size(), mirrorShots, gapText.c_str(), kExpectedGap[w], bullets, bodyHits, weak, maxDiff, damageBad, shooter.damages.size(), damageEventSum, damageSum, enemy.self.hp, expectedHp, shooter.self.magazine, shooter.self.spare);
			if (!gapOk || !countOk || bodyHits == 0 || damageBad != 0 || !hpOk || shooter.damages.empty()) ok = false;
		}

		// 같은 팀 — 사이에 세워 쏜다. 피해 0, 체력 그대로
		{
			const Vec3 eye = shooterAt + Vec3(0, 0, 60);
			shooter.Place(shooterAt, 100.f);
			enemy.Place(enemyAt, 100.f);
			ally.Place(enemyAt + (shooterAt - enemyAt) * 0.5, 100.f);
			PumpFor(clients, 3, 0.2);
			for (ShotClient& c : clients)
			{
				c.shots.clear();
				c.damages.clear();
			}
			float yaw = 0, pitch = 0;
			AimAt(eye, enemyAt + (shooterAt - enemyAt) * 0.5, yaw, pitch);
			const auto start = Clock::now();
			int sent = 0;
			const int kFrames = 40;
			while (sent < kFrames)
			{
				const int target = (int)(std::chrono::duration<double>(Clock::now() - start).count() * 60.0) + 6;
				while (sent < target && sent < kFrames)
				{
					wire::Input input = wire::Input();
					input.seq = ++shooter.seq;
					input.yaw = yaw;
					input.pitch = pitch;
					input.aimX = (float)eye.x; input.aimY = (float)eye.y; input.aimZ = (float)eye.z;
					input.buttons = wire::BUTTON_FIRE;
					input.weapon = (uint8_t)EWeapon::Rifle;	// 앞 차례에 권총 탄창을 다 썼다
					shooter.net.SendPacket(input, wire::Msg::C2S_INPUT);
					++sent;
				}
				for (ShotClient& c : clients) c.Pump();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			PumpFor(clients, 3, 0.3);
			int allyHits = 0, nonZero = 0, allyDamages = 0;
			for (const wire::Damage& d : shooter.damages)
			{
				if (d.target == ally.welcome.playerId) ++allyDamages;
			}
			for (const wire::Shot& shot : shooter.shots)
			{
				for (int b = 0; b < shot.count; ++b)
				{
					const wire::ShotBullet& bullet = shot.bullets[b];
					if (bullet.hit == (uint8_t)wire::EShotHit::Body && bullet.target == ally.welcome.playerId)
					{
						++allyHits;
						if (bullet.damage != 0.f) ++nonZero;
					}
				}
			}
			std::printf("SHOTS 같은 팀 shots=%zu 아군 맞음=%d 피해 있음=%d 아군 피해 알림=%d 아군 체력 %.2f 상대 체력 %.2f\n", shooter.shots.size(), allyHits, nonZero, allyDamages, ally.self.hp, enemy.self.hp);
			if (allyHits == 0 || nonZero != 0 || allyDamages != 0 || ally.self.hp != 100.f) ok = false;
		}

		timeEndPeriod(1);
		return ok;
	}

	// ─── 4. 경기 규칙 ──────────────────────────────────────
	void Expect(bool ok, int& fails, const char* text)
	{
		std::printf("RULES %s %s\n", ok ? "통과" : "실패", text);
		if (!ok) ++fails;
	}

	void SendAim(ShotClient& client, const Vec3& eye, const Vec3& target, float moveX, float moveY, uint8_t buttons, uint8_t weapon)
	{
		wire::Input input = wire::Input();
		input.seq = ++client.seq;
		AimAt(eye, target, input.yaw, input.pitch);
		input.aimX = (float)eye.x; input.aimY = (float)eye.y; input.aimZ = (float)eye.z;
		input.moveX = moveX;
		input.moveY = moveY;
		input.buttons = buttons;
		input.weapon = weapon;
		client.net.SendPacket(input, wire::Msg::C2S_INPUT);
	}

	// 고정 빠르기로 each(몇 번째) 를 부르고 받기를 돌린다. until 이 참이면 일찍 끝낸다
	template <class Each, class Until>
	int RunTicks(ShotClient* clients, int count, int ticks, Each&& each, Until&& until)
	{
		const auto start = Clock::now();
		int sent = 0;
		while (sent < ticks && !until())
		{
			const int target = (int)(std::chrono::duration<double>(Clock::now() - start).count() * 60.0) + 1;
			while (sent < target && sent < ticks) each(sent++);
			for (int i = 0; i < count; ++i) clients[i].Pump();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return sent;
	}

	// 어떤 자리를 쏠 수 있는 곁자리 — 땅이 있고 막히지 않은 곳
	bool FindSpotNear(const CMapCollision& map, const Vec3& target, Vec3& out)
	{
		for (double distance : { 400.0, 300.0, 600.0, 200.0 })
		{
			for (int d = 0; d < 16; ++d)
			{
				const double angle = d * kPi / 8.0;
				const Vec3 at = target + Vec3(std::cos(angle), std::sin(angle), 0) * distance;
				RayHit ray;
				if (map.Raycast(at + Vec3(0, 0, 60), target, MAP_BLOCK_WEAPON, ray)) continue;
				if (map.OverlapCapsule(at + Vec3(0, 0, 5), 35, 90, MAP_BLOCK_MOVE)) continue;
				RayHit floor;
				if (!map.Raycast(at, at + Vec3(0, 0, -150), MAP_BLOCK_MOVE, floor)) continue;
				out = at + Vec3(0, 0, 95.0 - floor.distance);
				return true;
			}
		}
		return false;
	}

	const wire::Spawn* LastSpawnOf(const ShotClient& observer, uint32_t player)
	{
		for (size_t i = observer.spawns.size(); i > 0; --i)
		{
			if (observer.spawns[i - 1].player == player) return &observer.spawns[i - 1];
		}
		return nullptr;
	}

	bool RunRules(int port, const CMapCollision& map, const std::vector<Row>& rows)
	{
		const MatchRules rules;
		const uint32_t kImmuneTicks = SecondsToTicks(rules.spawnImmunitySeconds);
		const int kLockInputs = SpawnMoveLockInputs(rules);
		const uint32_t kRespawnTicks = SecondsToTicks(rules.respawnSeconds);
		const uint32_t kPostGameTicks = SecondsToTicks(rules.postGameSeconds);
		const uint8_t kRifle = (uint8_t)EWeapon::Rifle;
		int fails = 0;

		ShotClient clients[3];
		for (ShotClient& c : clients)
		{
			if (!ConnectAndHello(c.net, port, c.welcome))
			{
				std::printf("RULES 접속 실패\n");
				return false;
			}
		}
		ShotClient& a = clients[0];
		ShotClient& b = clients[1];
		ShotClient& c = clients[2];
		const uint32_t idA = a.welcome.playerId, idB = b.welcome.playerId, idC = c.welcome.playerId;
		Expect(a.welcome.team == wire::kTeamA && b.welcome.team == wire::kTeamB && c.welcome.team == wire::kTeamA, fails, "팀 = 적은 쪽, 같으면 작은 번호(1·2·1)");
		timeBeginPeriod(1);

		// ① 경기 전 대기 — 맞아도 피해 없음
		Vec3 arenaA, arenaB, arenaSide;
		if (!FindArena(map, rows, arenaA, arenaB, arenaSide))
		{
			std::printf("RULES 설 자리를 못 찾음\n");
			timeEndPeriod(1);
			return false;
		}
		a.Place(arenaA, 100.f);
		b.Place(arenaB, 100.f);
		PumpFor(clients, 3, 0.15);
		const Vec3 eyeWarm = arenaA + Vec3(0, 0, 60);
		RunTicks(clients, 3, 60, [&](int) { SendAim(a, eyeWarm, arenaB, 0, 0, wire::BUTTON_FIRE, kRifle); }, [] { return false; });
		PumpFor(clients, 3, 0.1);
		int warmHits = 0, warmNonZero = 0;
		for (const wire::Shot& shot : a.shots)
		{
			for (int i = 0; i < shot.count; ++i)
			{
				if (shot.bullets[i].hit != (uint8_t)wire::EShotHit::Body) continue;
				++warmHits;
				if (shot.bullets[i].damage != 0.f) ++warmNonZero;
			}
		}
		std::printf("RULES 대기 중 몸에 맞음=%d 피해 있음=%d 피해 알림=%zu 체력=%.1f 단계=%u\n", warmHits, warmNonZero, a.damages.size(), b.self.hp, a.matches.empty() ? 99u : a.matches.back().phase);
		Expect(warmHits > 0 && warmNonZero == 0 && a.damages.empty() && b.self.hp == 100.f && !a.matches.empty() && a.matches.back().phase == (uint8_t)wire::EMatchPhase::Warmup, fails, "대기 중 맞아도 피해 0");

		// ② 대기 끝 — 모두 다시 세움
		RunTicks(clients, 3, 60 * 10, [](int) {}, [&] { return !a.matches.empty() && a.matches.back().phase == (uint8_t)wire::EMatchPhase::Playing; });
		PumpFor(clients, 3, 0.02);
		const wire::Match playing = a.matches.back();
		const wire::Spawn* spawnB = LastSpawnOf(a, idB);
		const wire::Spawn* spawnC = LastSpawnOf(a, idC);
		if (playing.phase != (uint8_t)wire::EMatchPhase::Playing || !spawnB || !spawnC)
		{
			std::printf("RULES 경기가 시작되지 않음\n");
			timeEndPeriod(1);
			return false;
		}
		const uint32_t playTick = playing.tick;
		std::printf("RULES 시작 틱=%u 목표=%u 남은 초=%.2f 점수=%u:%u B 시작 틱=%u C 시작 틱=%u 자리 B=%u C=%u\n", playTick, playing.targetScore, playing.secondsLeft, playing.teamScoreA, playing.teamScoreB, spawnB->tick, spawnC->tick, spawnB->startIndex, spawnC->startIndex);
		Expect(spawnB->tick == playTick && spawnC->tick == playTick && playing.teamScoreA == 0 && playing.teamScoreB == 0, fails, "대기 끝에 모두 다시 세우고 점수 0");
		Expect(std::fabs(playing.secondsLeft - rules.smallMatchSeconds) < 1.0, fails, "6명 미만이면 경기 시간 720초");

		// ③ 부활 무적 · 처치 — B 는 가만히 서고 A 가 곁에서 쏜다. C 는 같은 동안 이동 막음을 잰다
		const Vec3 bStart(spawnB->x, spawnB->y, spawnB->z);
		const uint32_t tickB = spawnB->tick;
		const Vec3 cStart(spawnC->x, spawnC->y, spawnC->z);
		const uint32_t seqC0 = c.seq;
		const double yawC = spawnC->yaw * kPi / 180.0;
		const float forwardX = (float)std::cos(yawC), forwardY = (float)std::sin(yawC);
		Vec3 spotA;
		if (!FindSpotNear(map, bStart, spotA))
		{
			std::printf("RULES B 곁에 설 자리를 못 찾음\n");
			timeEndPeriod(1);
			return false;
		}
		a.Place(spotA, 100.f);
		const size_t shotsBefore = a.shots.size();
		const Vec3 eyeA = spotA + Vec3(0, 0, 60);
		const Vec3 cEye = cStart + Vec3(0, 0, 60);
		RunTicks(clients, 3, 60 * 15, [&](int i)
		{
			SendAim(a, eyeA, bStart, 0, 0, wire::BUTTON_FIRE, kRifle);
			if (i < 200)
			{
				const uint8_t jump = (i >= 20 && i < 25) ? wire::BUTTON_JUMP : 0;
				SendAim(c, cEye, cEye + Vec3(forwardX, forwardY, 0) * 100.0, forwardX, forwardY, jump, 0);
			}
		}, [&] { return !a.kills.empty(); });
		PumpFor(clients, 3, 0.05);
		int immuneHits = 0;
		uint32_t firstDamageTick = 0;
		for (size_t s = shotsBefore; s < a.shots.size(); ++s)
		{
			const wire::Shot& shot = a.shots[s];
			if (shot.shooter != idA) continue;
			for (int i = 0; i < shot.count; ++i)
			{
				const wire::ShotBullet& bullet = shot.bullets[i];
				if (bullet.hit != (uint8_t)wire::EShotHit::Body || bullet.target != idB) continue;
				if (bullet.damage == 0.f) { if (firstDamageTick == 0) ++immuneHits; }
				else if (firstDamageTick == 0) firstDamageTick = shot.tick;
			}
		}
		std::printf("RULES 무적 중 맞음=%d 첫 피해 틱=시작+%u (무적 %u틱) 첫 처치 수=%zu\n", immuneHits, firstDamageTick ? firstDamageTick - tickB : 0, kImmuneTicks, a.kills.size());
		Expect(immuneHits > 0 && firstDamageTick >= tickB + kImmuneTicks && firstDamageTick <= tickB + kImmuneTicks + 12, fails, "부활 무적 165틱 — 그 안은 피해 0, 지나면 바로 들어옴");
		if (a.kills.empty())
		{
			std::printf("RULES 처치가 나오지 않음\n");
			timeEndPeriod(1);
			return false;
		}
		const wire::Kill kill1 = a.kills.front();
		std::printf("RULES 처치 틱=%u 가한=%u 맞은=%u 점수=%u:%u 처치수=%u 사망수=%u\n", kill1.tick, kill1.attacker, kill1.target, kill1.teamScoreA, kill1.teamScoreB, kill1.attackerKills, kill1.targetDeaths);
		Expect(kill1.attacker == idA && kill1.target == idB && kill1.teamScoreA == 1 && kill1.teamScoreB == 0 && kill1.attackerKills == 1 && kill1.targetDeaths == 1, fails, "처치 — 가한 쪽 팀 점수·처치 +1, 맞은 쪽 사망 +1");

		// ④ 이동 막음 — 입력 124개 동안 수평으로 안 움직이고 점프는 된다
		for (int i = 0; i < 40; ++i)
		{
			if (c.seq - seqC0 >= 200) break;
			SendAim(c, cEye, cEye + Vec3(forwardX, forwardY, 0) * 100.0, forwardX, forwardY, 0, 0);
		}
		RunTicks(clients, 3, 30, [](int) {}, [] { return false; });
		int lockedSamples = 0, lockedMoved = 0, lockFlagBad = 0;
		bool sawJump = false, movedAfter = false;
		for (const wire::PlayerState& p : c.history)
		{
			if (p.lastSeq <= seqC0) continue;
			const int n = (int)(p.lastSeq - seqC0);
			if (n <= kLockInputs)
			{
				++lockedSamples;
				if ((float)cStart.x != p.x || (float)cStart.y != p.y) ++lockedMoved;
				if (((p.flags & wire::STATE_MOVE_LOCKED) != 0) != (n < kLockInputs)) ++lockFlagBad;
				if (p.flags & wire::STATE_FALLING && p.z > (float)cStart.z) sawJump = true;
			}
			else if ((float)cStart.x != p.x || (float)cStart.y != p.y)
			{
				movedAfter = true;
			}
		}
		std::printf("RULES 이동 막음 입력 %d개 표본=%d 움직임=%d 표시 어긋남=%d 점프 보임=%d 풀린 뒤 움직임=%d\n", kLockInputs, lockedSamples, lockedMoved, lockFlagBad, sawJump ? 1 : 0, movedAfter ? 1 : 0);
		Expect(lockedSamples > 0 && lockedMoved == 0 && lockFlagBad == 0, fails, "부활 뒤 입력 124개는 수평 이동 없음");
		Expect(sawJump, fails, "이동 막음 중에도 점프는 됨");
		Expect(movedAfter, fails, "막음이 풀리면 움직임");

		// ⑤ 사망 중 입력 거절 · 부활 시간
		const size_t shotsAtDeath = a.shots.size();
		const Vec3 eyeB = bStart + Vec3(0, 0, 60);
		RunTicks(clients, 3, 120, [&](int) { SendAim(b, eyeB, spotA, 1.f, 0, wire::BUTTON_FIRE, kRifle); }, [] { return false; });
		PumpFor(clients, 3, 0.1);
		int shotsByDead = 0;
		for (size_t s = shotsAtDeath; s < a.shots.size(); ++s)
		{
			if (a.shots[s].shooter == idB) ++shotsByDead;
		}
		const wire::PlayerState deadState = b.self;
		std::printf("RULES 사망 중 발사=%d 체력=%.1f 사망 표시=%d 번호 반영=%u/%u 위치 차=%.3f\n", shotsByDead, deadState.hp, (deadState.flags & wire::STATE_DEAD) ? 1 : 0, deadState.lastSeq, b.seq, std::fabs(deadState.x - bStart.x));
		Expect(shotsByDead == 0 && deadState.hp == 0.f && (deadState.flags & wire::STATE_DEAD) != 0, fails, "죽어 있으면 입력을 쓰지 않음(발사 0)");
		Expect(deadState.lastSeq == b.seq && std::fabs(deadState.x - bStart.x) < 1.0, fails, "죽어 있으면 번호만 반영하고 움직이지 않음");

		RunTicks(clients, 3, 60 * 7, [](int) {}, [&]
		{
			const wire::Spawn* last = LastSpawnOf(a, idB);
			return last && last->tick > kill1.tick;
		});
		PumpFor(clients, 3, 0.05);
		const wire::Spawn respawnB = *LastSpawnOf(a, idB);
		std::printf("RULES 부활까지 %d틱(기대 %u) 체력=%.1f 탄창=%d 여분=%d 무기=%u\n", (int)(respawnB.tick - kill1.tick), kRespawnTicks, b.self.hp, b.self.magazine, b.self.spare, b.self.weapon);
		Expect(respawnB.tick - kill1.tick == kRespawnTicks, fails, "부활까지 300틱");
		Expect(b.self.hp == 100.f && b.self.weapon == 0 && b.self.magazine == GetWeaponDef(EWeapon::Pistol).magazineSize && b.self.spare == GetWeaponDef(EWeapon::Pistol).startSpareAmmo, fails, "부활하면 체력·탄 처음대로, 권총");

		// ⑥ 떨어져 죽음 — 사망 · 점수 그대로(처치자 없는 자기 파괴 — B_ShooterGameScoring_Base 는 다른 팀 처치만 센다, L-3-a)
		wire::DebugPlace fall = wire::DebugPlace();
		fall.seq = ++c.seq;
		fall.x = 50000; fall.y = 50000; fall.z = rules.fallKillZ + 5.0;
		fall.vz = -2000;
		fall.hp = 100.f;
		fall.falling = 1;
		c.net.SendPacket(fall, wire::Msg::C2S_DEBUG_PLACE);
		const size_t killsBeforeFall = a.kills.size();
		RunTicks(clients, 3, 30, [&](int) { SendAim(c, cEye, cEye + Vec3(100, 0, 0), 0, 0, 0, 0); }, [&] { return a.kills.size() > killsBeforeFall; });
		PumpFor(clients, 3, 0.05);
		if (a.kills.size() > killsBeforeFall)
		{
			const wire::Kill& k = a.kills[killsBeforeFall];
			std::printf("RULES 낙사 가한=%u 맞은=%u 점수=%u:%u 사망수=%u\n", k.attacker, k.target, k.teamScoreA, k.teamScoreB, k.targetDeaths);
			Expect(k.attacker == 0 && k.target == idC && k.teamScoreA == 1 && k.teamScoreB == 0 && k.targetDeaths == 0, fails, "낙사 — 사망 · 점수 그대로");
		}
		else
		{
			Expect(false, fails, "낙사가 안 일어남");
		}

		// ⑥-1 · ⑥-2 의 C 자리 — A · B 가 폭발 반지름 밖인 곳(두 사람 체력 · 도움 기록을 건드리면 ⑦ 이 달라진다).
		// ⑥-1 은 던진 뒤 C 를 옮겨 둘 먼 자리(spotFar — C 자리에서도 폭발 반지름 밖)도 쓴다
		const Vec3 posA(a.self.x, a.self.y, a.self.z), posB(b.self.x, b.self.y, b.self.z);
		const double keepAway = kGrenadeBlastRadius + 150.0;
		Vec3 spotC, spotFar;
		bool found = false, foundFar = false;
		{
			size_t startCount = 0;
			const SpawnPoint* starts = GetExpanseStarts(startCount);
			for (size_t s = 0; s < startCount && !foundFar; ++s)
			{
				Vec3 at;
				if (!FindSpotNear(map, starts[s].location, at)) continue;
				if (Length(at - posA) < keepAway || Length(at - posB) < keepAway) continue;
				if (!found) { spotC = at; found = true; }
				else if (Length(at - spotC) >= keepAway) { spotFar = at; foundFar = true; }
			}
		}

		// ⑥-1 수류탄 카메라 거리(L-3-c) — 선 시작 = 입력 시작점 + 시선 × (450 − camBack), 선 시작에서 45 안에 맞으면 조종 회전 그대로(Grenade.cpp ComputeGrenadeThrow).
		// 바닥까지 471 이면 17.25 → 38.25(조종) · 30 → 51(맞은 곳 바라봄 — 대조군), 515 면 −40 → 25(조종) · 0 으로 잘리면 65. 아래 끝이 30 이면 첫째 · 셋째가, 0 이면 셋째만 갈린다.
		// 조종 회전이면 던짐 소식 회전 = 입력 회전(Room.cpp ThrowGrenade → SendGrenadeEvent). 던짐 소식을 받으면 C 를 먼 자리로 옮겨 폭발을 피하고,
		// 다음 던짐까지 쿨다운(301 입력)을 C 입력으로 채운다 — 경기 끝 단계(10초)엔 세 번을 못 기다려 경기 중에 던진다
		if (!found || !foundFar)
		{
			Expect(false, fails, "수류탄 카메라 거리 — A · B 에서 먼 자리 둘을 못 찾음");
		}
		else
		{
			struct CamCase { float camBack; double floorAt; bool control; };
			const CamCase cases[] = { { 17.25f, 471.0, true }, { 30.f, 471.0, false }, { -40.f, 515.0, true } };
			const auto angleDiff = [](double x, double y) { return std::fmod(x - y + 540.0, 360.0) - 180.0; };
			const Vec3 eyeC = spotC + Vec3(0, 0, 60);
			const Vec3 eyeFar = spotFar + Vec3(0, 0, 60);
			RayHit down;
			const bool floorFound = map.Raycast(eyeC, eyeC + Vec3(0, 0, -400), MAP_BLOCK_WEAPON, down);
			for (const CamCase& cc : cases)
			{
				// 피치는 눈높이로 정하고, 요는 16 방향 중 눈에서 그 시선으로 무기 채널 맞은 거리가 floorAt ± 4 인 첫 방향(45 까지 여유 2.75 이상)
				const float pitch = floorFound ? (float)(-std::asin(down.distance / cc.floorAt) * 180.0 / kPi) : 0.f;
				float yaw = 0.f;
				double seen = 0.0;
				bool aimed = false;
				for (int d = 0; d < 16 && floorFound && !aimed; ++d)
				{
					const float tryYaw = (float)(d * 22.5);
					RayHit ray;
					if (!map.Raycast(eyeC, eyeC + AimDirection(tryYaw, pitch) * kGrenadeTraceRange, MAP_BLOCK_WEAPON, ray)) continue;
					if (std::fabs(ray.distance - cc.floorAt) > 4.0) continue;
					yaw = tryYaw;
					seen = ray.distance;
					aimed = true;
				}
				if (!aimed)
				{
					Expect(false, fails, "수류탄 카메라 거리 — 바닥까지 거리가 맞는 방향이 없음");
					continue;
				}
				c.Place(spotC, 100.f);
				PumpFor(clients, 3, 0.1);
				const size_t grenadesBefore = a.grenades.size();
				const auto findThrow = [&]() -> const wire::Grenade*
				{
					for (size_t g = grenadesBefore; g < a.grenades.size(); ++g)
					{
						if (a.grenades[g].thrower == idC && a.grenades[g].kind == wire::GRENADE_THROW) return &a.grenades[g];
					}
					return nullptr;
				};
				uint32_t throwSeq = 0;
				RunTicks(clients, 3, 60, [&](int i)
				{
					wire::Input input = wire::Input();
					input.seq = ++c.seq;
					input.yaw = yaw;
					input.pitch = pitch;
					input.aimX = (float)eyeC.x; input.aimY = (float)eyeC.y; input.aimZ = (float)eyeC.z;
					input.camBack = cc.camBack;
					if (i == 0)
					{
						input.buttons = wire::BUTTON_GRENADE;
						throwSeq = input.seq;
					}
					c.net.SendPacket(input, wire::Msg::C2S_INPUT);
				}, [&] { return findThrow() != nullptr; });
				PumpFor(clients, 3, 0.05);
				c.Place(spotFar, 100.f);		// 폭발을 피한다
				const wire::Grenade* thrown = findThrow();
				const double pitchDiff = thrown ? angleDiff(thrown->pitch, pitch) : 0.0;
				const double yawDiff = thrown ? angleDiff(thrown->yaw, yaw) : 0.0;
				std::printf("RULES 수류탄 카메라 거리 camBack=%.2f 바닥까지=%.2f(목표 %.0f) 요=%.1f 피치=%.3f 던짐=%d 소식 seq=%u(누름 %u) 피치 차=%.4f 요 차=%.4f\n",
					cc.camBack, seen, cc.floorAt, yaw, pitch, thrown ? 1 : 0, thrown ? thrown->seq : 0, throwSeq, pitchDiff, yawDiff);
				const bool control = thrown && std::fabs(pitchDiff) < 0.01 && std::fabs(yawDiff) < 0.01;
				const bool lookAt = thrown && std::fabs(pitchDiff) > 0.5;
				Expect(thrown && thrown->seq == throwSeq && (cc.control ? control : lookAt), fails,
					cc.control ? "수류탄 카메라 거리 — 선 시작에서 45 안 → 조종 회전(자르지 않음)" : "수류탄 카메라 거리 — 45 밖 → 맞은 곳 바라봄(대조군)");
				RunTicks(clients, 3, 310, [&](int) { SendAim(c, eyeFar, eyeFar + Vec3(100, 0, 0), 0, 0, 0, 0); }, [] { return false; });	// 쿨다운 301 입력 + 여유
			}
		}

		// ⑥-2 자기 수류탄 처치 — C 를 체력 1 로 옮겨(옮기기가 죽음 · 무적을 푼다) 발밑에 던진다. 처치자 = 자기(같은 팀)라 사망 · 처치 · 점수 다 그대로(L-3-a).
		// 자리는 위 C 자리
		{
			if (!found)
			{
				Expect(false, fails, "자기 수류탄 처치 — A · B 에서 먼 자리를 못 찾음");
			}
			else
			{
				c.Place(spotC, 1.f);
				PumpFor(clients, 3, 0.1);
				const Vec3 eyeC = spotC + Vec3(0, 0, 60);
				const size_t killsBeforeSelf = a.kills.size();
				RunTicks(clients, 3, 60 * 4, [&](int i) { SendAim(c, eyeC, eyeC + Vec3(30, 0, -100), 0, 0, i == 0 ? wire::BUTTON_GRENADE : 0, 0); },
					[&] { return a.kills.size() > killsBeforeSelf; });
				PumpFor(clients, 3, 0.05);
				if (a.kills.size() > killsBeforeSelf)
				{
					const wire::Kill& k = a.kills[killsBeforeSelf];
					std::printf("RULES 자기 수류탄 처치 가한=%u 맞은=%u 무기=%u 점수=%u:%u 처치수=%u 사망수=%u A 거리=%.0f B 거리=%.0f\n",
						k.attacker, k.target, k.weapon, k.teamScoreA, k.teamScoreB, k.attackerKills, k.targetDeaths, Length(spotC - posA), Length(spotC - posB));
					Expect(k.attacker == idC && k.target == idC && k.weapon == wire::kWeaponGrenade && k.teamScoreA == 1 && k.teamScoreB == 0 && k.attackerKills == 0 && k.targetDeaths == 0,
						fails, "자기 수류탄 처치 — 사망 · 처치 · 점수 그대로");
				}
				else
				{
					Expect(false, fails, "자기 수류탄 처치가 안 일어남");
				}
			}
		}

		// ⑦ 승리 — 목표 2. 무적이 끝날 때쯤 쏘기 시작한다
		const Vec3 bAgain(respawnB.x, respawnB.y, respawnB.z);
		Vec3 spotA2;
		if (!FindSpotNear(map, bAgain, spotA2))
		{
			std::printf("RULES B 곁에 설 자리를 못 찾음(2)\n");
			timeEndPeriod(1);
			return false;
		}
		a.Place(spotA2, 100.f);
		PumpFor(clients, 3, 2.5);
		const Vec3 eyeA2 = spotA2 + Vec3(0, 0, 60);
		const auto isPostGame = [&] { return !a.matches.empty() && a.matches.back().phase == (uint8_t)wire::EMatchPhase::PostGame; };
		RunTicks(clients, 3, 60 * 20, [&](int) { SendAim(a, eyeA2, bAgain, 0, 0, wire::BUTTON_FIRE, kRifle); }, isPostGame);
		if (!isPostGame())
		{
			Expect(false, fails, "목표 점수에 닿아도 경기가 안 끝남");
			timeEndPeriod(1);
			return false;
		}
		const wire::Match post = a.matches.back();
		std::printf("RULES 승리 틱=%u 이긴 팀=%u 점수=%u:%u 남은 초=%.2f\n", post.tick, post.winningTeam, post.teamScoreA, post.teamScoreB, post.secondsLeft);
		Expect(post.winningTeam == wire::kTeamA && post.teamScoreA == 2, fails, "목표 점수에 닿은 팀이 이김");
		Expect(std::fabs(post.secondsLeft - rules.postGameSeconds) < 0.1, fails, "경기 뒤 10초");

		// ⑧ 경기 끝 — 걷기 · 쏘기 · 수류탄 다 된다. Lyra 는 능력 입력 막음(GE_BlockAbilityInput)을 맵을 다시 열기 한 틱 전에야 건다
		// (그래프 원본 · 기록 g0end1 · 2 — 끝 단계 Q 가 던져져 상대 몸에 맞고도 피해 0 · 점수 그대로, L-3-b). 피해는 끝 단계 무적(IsImmune)이 막는다
		const wire::PlayerState beforeWalk = a.self;
		const Vec3 walkDir = (bAgain - spotA2) * (1.0 / Length(bAgain - spotA2));
		RunTicks(clients, 3, 60, [&](int) { SendAim(a, eyeA2, bAgain, (float)walkDir.x, (float)walkDir.y, wire::BUTTON_FIRE, kRifle); }, [] { return false; });
		PumpFor(clients, 3, 0.1);
		int shotsAfter = 0;
		for (const wire::Shot& shot : a.shots)
		{
			if (shot.tick > post.tick) ++shotsAfter;
		}
		const double walked = std::sqrt((double)(a.self.x - beforeWalk.x) * (a.self.x - beforeWalk.x) + (double)(a.self.y - beforeWalk.y) * (a.self.y - beforeWalk.y));
		std::printf("RULES 경기 끝 뒤 발사=%d 걸은 거리=%.1f\n", shotsAfter, walked);
		Expect(shotsAfter > 0, fails, "경기 끝에도 발사는 됨");
		Expect(walked > 100.0, fails, "경기 끝에도 이동은 됨");

		// 몸에 맞혀도 피해 0 — 우승 처치로 죽은 B 를 체력 100 으로 다시 세운다(옮기기가 죽음을 푼다 · 부활 시계는 죽은 동안만 돈다 — Room.cpp TickRespawns).
		// A 는 처음 자리로 · 소총 탄을 채워 둔다(걸으며 쏜 탄이 비면 재장전이 쏘기를 막는다)
		const WeaponDef& rifle = GetWeaponDef(EWeapon::Rifle);
		a.Place(spotA2, 100.f, wire::DEBUG_PLACE_SET_AMMO, kRifle, (int16_t)rifle.magazineSize, (int16_t)rifle.startSpareAmmo);
		b.Place(bAgain, 100.f);
		PumpFor(clients, 3, 0.2);
		const size_t damagesBeforePost = a.damages.size(), killsBeforePost = a.kills.size(), shotsBeforePost = a.shots.size();
		RunTicks(clients, 3, 30, [&](int) { SendAim(a, eyeA2, bAgain, 0, 0, wire::BUTTON_FIRE, kRifle); }, [] { return false; });
		PumpFor(clients, 3, 0.1);
		int postShots = 0, postBodyHits = 0;
		for (size_t s = shotsBeforePost; s < a.shots.size(); ++s)
		{
			const wire::Shot& shot = a.shots[s];
			if (shot.shooter != idA) continue;
			++postShots;
			for (int i = 0; i < shot.count && i < wire::kMaxBulletsInShot; ++i)
			{
				if (shot.bullets[i].hit == (uint8_t)wire::EShotHit::Body && shot.bullets[i].target == idB) ++postBodyHits;
			}
		}
		const wire::Match afterFire = a.matches.back();
		std::printf("RULES 경기 끝 뒤 몸 겨눔 발사=%d 몸 맞음=%d 피해=%zu 처치=%zu B 체력=%.1f 점수=%u:%u\n",
			postShots, postBodyHits, a.damages.size() - damagesBeforePost, a.kills.size() - killsBeforePost, b.self.hp, afterFire.teamScoreA, afterFire.teamScoreB);
		Expect(postShots > 0 && postBodyHits > 0 && a.damages.size() == damagesBeforePost && a.kills.size() == killsBeforePost && b.self.hp == 100.f
			&& afterFire.teamScoreA == 2 && afterFire.teamScoreB == 0, fails, "경기 끝 — 몸에 맞아도 피해 0 · 점수 그대로");

		// 수류탄 — B 에게 던진다. 던짐 소식이 오고(능력이 켜졌다) 터져도 A · B 피해 0
		const size_t grenadesBeforePost = a.grenades.size();
		const auto postBoomed = [&]
		{
			for (size_t g = grenadesBeforePost; g < a.grenades.size(); ++g)
			{
				if (a.grenades[g].thrower == idA && a.grenades[g].kind != wire::GRENADE_THROW) return true;
			}
			return false;
		};
		RunTicks(clients, 3, 60 * 4, [&](int i) { SendAim(a, eyeA2, bAgain, 0, 0, i == 0 ? wire::BUTTON_GRENADE : 0, kRifle); }, postBoomed);
		PumpFor(clients, 3, 0.1);
		int postThrows = 0, postBoomsHit = 0, postBoomsTimer = 0;
		for (size_t g = grenadesBeforePost; g < a.grenades.size(); ++g)
		{
			const wire::Grenade& grenade = a.grenades[g];
			if (grenade.thrower != idA) continue;
			if (grenade.kind == wire::GRENADE_THROW) ++postThrows;
			else if (grenade.kind == wire::GRENADE_BOOM_HIT) ++postBoomsHit;
			else if (grenade.kind == wire::GRENADE_BOOM_TIMER) ++postBoomsTimer;
		}
		const wire::Match afterGrenade = a.matches.back();
		std::printf("RULES 경기 끝 수류탄 던짐=%d 터짐 맞음=%d 신관=%d 피해=%zu 처치=%zu A 체력=%.1f B 체력=%.1f 점수=%u:%u\n",
			postThrows, postBoomsHit, postBoomsTimer, a.damages.size() - damagesBeforePost, a.kills.size() - killsBeforePost, a.self.hp, b.self.hp, afterGrenade.teamScoreA, afterGrenade.teamScoreB);
		Expect(postThrows == 1 && postBoomsHit + postBoomsTimer == 1 && a.damages.size() == damagesBeforePost && a.kills.size() == killsBeforePost
			&& a.self.hp == 100.f && b.self.hp == 100.f && afterGrenade.teamScoreA == 2 && afterGrenade.teamScoreB == 0, fails, "경기 끝 — 수류탄 던져지고 터져도 피해 0 · 점수 그대로");

		// ⑨ 결과
		RunTicks(clients, 3, 60 * 12, [](int) {}, [&] { return a.hasResult; });
		PumpFor(clients, 3, 0.05);
		if (!a.hasResult)
		{
			Expect(false, fails, "결과가 안 옴");
			timeEndPeriod(1);
			return false;
		}
		int killsA = -1, deathsB = -1, deathsC = -1;
		for (int i = 0; i < a.result.count; ++i)
		{
			const wire::PlayerResult& p = a.result.players[i];
			if (p.id == idA) killsA = p.kills;
			if (p.id == idB) deathsB = p.deaths;
			if (p.id == idC) deathsC = p.deaths;
		}
		std::printf("RULES 결과 틱=경기 끝+%d(기대 %u) 이긴 팀=%u A 처치=%d B 사망=%d C 사망=%d 결과 인원=%u\n", (int)(a.result.tick - post.tick), kPostGameTicks, a.result.winningTeam, killsA, deathsB, deathsC, a.result.count);
		Expect(a.result.tick - post.tick == kPostGameTicks, fails, "경기 끝 600틱 뒤 결과");
		Expect(a.result.winningTeam == wire::kTeamA && killsA == 2 && deathsB == 2 && deathsC == 0 && a.result.count == 3, fails, "결과 = 처치·사망 집계");

		timeEndPeriod(1);
		std::printf("RULES 실패 %d건\n", fails);
		return fails == 0;
	}

	// ─── 5. 시작 자리 ──────────────────────────────────────
	bool CheckStarts(const CMapCollision& map, const MoveParams& params, const char* dumpPath)
	{
		size_t count = 0;
		const SpawnPoint* starts = GetExpanseStarts(count);

		FILE* file = std::fopen(dumpPath, "rb");
		if (!file)
		{
			std::printf("STARTS 덤프를 못 읽음 %s\n", dumpPath);
			return false;
		}
		char line[1024];
		size_t dumped = 0, tableDiff = 0;
		while (std::fgets(line, sizeof(line), file))
		{
			if (std::strncmp(line, "MOB_PLAYERSTART ", 16) != 0) continue;
			const char* loc = std::strstr(line, "loc=");
			double x = 0, y = 0, z = 0, yaw = 0;
			if (!loc || std::sscanf(loc, "loc=%lf,%lf,%lf yaw=%lf", &x, &y, &z, &yaw) != 4) continue;
			if (dumped < count)
			{
				const SpawnPoint& s = starts[dumped];
				if (std::fabs(s.location.x - x) > 0.0005 || std::fabs(s.location.y - y) > 0.0005 || std::fabs(s.location.z - z) > 0.0005 || std::fabs(s.yawDegrees - yaw) > 0.0005) ++tableDiff;
			}
			++dumped;
		}
		std::fclose(file);
		std::printf("STARTS 표=%zu 덤프=%zu 다름=%zu\n", count, dumped, tableDiff);

		CMoveSim sim(map, params);
		int bad = 0;
		for (size_t i = 0; i < count; ++i)
		{
			const Vec3& at = starts[i].location;
			const bool overlap = map.OverlapCapsule(at, params.capsuleRadius, params.capsuleHalfHeight, MAP_BLOCK_MOVE);
			MoveState state;
			state.location = at;
			state.maxSpeed = params.maxWalkSpeed;
			sim.RefreshFloor(state);
			int landedAt = -1;
			for (int t = 0; t < 120; ++t)
			{
				sim.Tick(state, Vec3(), kTick);
				if (landedAt < 0 && state.mode == EMoveMode::Walking) landedAt = t + 1;
			}
			const double drop = at.z - state.location.z;
			std::printf("STARTS %2zu loc=%.3f,%.3f,%.3f yaw=%.3f 겹침=%d 착지 틱=%d 내려앉은 높이=%.3f 끝 위치 수평 차=%.3f\n", i, at.x, at.y, at.z, starts[i].yawDegrees, overlap ? 1 : 0, landedAt, drop,
				std::sqrt((state.location.x - at.x) * (state.location.x - at.x) + (state.location.y - at.y) * (state.location.y - at.y)));
			if (landedAt < 0) ++bad;
		}
		std::printf("STARTS 착지 못 함=%d\n", bad);
		return count == dumped && tableDiff == 0 && bad == 0;
	}

	// ─── 권위 확인 ─────────────────────────────────────────
	// 일부러 나쁜 입력을 보내고, 결과와 서버의 사유별 개수(REJECT 줄)를 함께 본다.
	// 서버 쪽 개수는 서버가 꺼질 때 찍히므로 그 로그를 읽는다. 서버는 --debug 없이 --warmup=0
	uint64_t ReadCount(const std::string& text, const char* key)
	{
		const std::string pattern = std::string(key) + "=";
		const size_t rejectAt = text.find("REJECT ");
		if (rejectAt == std::string::npos) return (uint64_t)-1;
		const size_t at = text.find(pattern, rejectAt);
		if (at == std::string::npos) return (uint64_t)-1;
		return std::strtoull(text.c_str() + at + pattern.size(), nullptr, 10);
	}

	// 몇 발을 몇 틱에 걸쳐 쐈나 — 입력 한도 8 만큼 한 번 몰릴 수 있어 최소 간격이 아니라 평균으로 본다
	void ShotSpan(const std::vector<wire::Shot>& shots, uint8_t weapon, size_t from, int& outCount, int& outSpan)
	{
		outCount = 0;
		outSpan = 0;
		uint32_t first = 0;
		for (size_t i = from; i < shots.size(); ++i)
		{
			if (shots[i].weapon != weapon) continue;
			if (outCount == 0) first = shots[i].tick;
			outSpan = (int)(shots[i].tick - first);
			++outCount;
		}
	}

	// ─── 던진 사람 나감(L-3-d) ─────────────────────────────
	// Lyra 기록대로 — 던진 사람이 나가도 수류탄은 남아 제때 터지고(L-1-d ① l1df), 나간 뒤엔 상대 몸에 닿아도 안 터지고 튕기며(L-3-d l3dh), 반지름 안 몸도 피해 0 · 소식 0.
	// 칸 셋 — ① 대조(안 나감): V 의 등 뒤 400 에서 V 눈높이를 보고 던짐 → 몸에 닿아 터짐 · 피해 소식(Lyra l3dh 나이 8)
	// ② 같은 자리 · 같은 입력으로 던지고 던짐 소식을 받자마자 나감 → 닿아도 신관 터짐 · 피해 0
	// ③ V 곁 150 에서 발 앞 바닥에 던지고 나감 → 터진 자리에서 V 가 반지름 안(같은 코드로 잰 곡선 피해 > 0)인데 피해 0.
	// 던지는 사람은 칸마다 새로 들어온다(나가면 번호가 없어진다 — V 가 팀 A, 던지는 사람은 늘 팀 B)
	bool RunGrenadeLeave(int port, const CMapCollision& map, const std::vector<Row>& rows)
	{
		int fails = 0;
		const auto check = [&fails](bool ok, const char* text)
		{
			std::printf("GLEAVE %s %s\n", ok ? "통과" : "실패", text);
			if (!ok) ++fails;
		};
		Vec3 farA, spotV, sideV;
		if (!FindArena(map, rows, farA, spotV, sideV))
		{
			std::printf("GLEAVE 설 자리를 못 찾음\n");
			return false;
		}
		Vec3 dir = farA - spotV;
		dir.z = 0.0;
		dir = dir * (1.0 / Length(dir));
		const Vec3 spotT = spotV + dir * 400.0;							// ① · ② 던지는 자리 — Lyra 기록 l3dh 와 같은 거리
		const Vec3 spotN = spotV + Vec3(-dir.y, dir.x, 0.0) * 150.0;		// ③ 던지는 자리 — V 곁
		for (const Vec3& at : { spotT, spotN })
		{
			RayHit floor;
			if (!map.Raycast(at + Vec3(0, 0, 10), at + Vec3(0, 0, -150), MAP_BLOCK_MOVE, floor) || floor.distance < 95 || floor.distance > 105 || map.OverlapCapsule(at, 35, 90, MAP_BLOCK_MOVE))
			{
				std::printf("GLEAVE 던질 자리에 바닥이 없거나 막힘 (%.0f, %.0f, %.0f)\n", at.x, at.y, at.z);
				return false;
			}
		}

		ShotClient clients[4];		// V · 던지는 사람 셋(칸마다 하나 — 닫힌 쪽 Pump 는 recv 실패로 바로 돈다)
		ShotClient& v = clients[0];
		if (!ConnectAndHello(v.net, port, v.welcome))
		{
			std::printf("GLEAVE 접속 실패\n");
			return false;
		}
		timeBeginPeriod(1);
		const Vec3 eyeV = spotV + Vec3(0, 0, 60);
		// ① · ② 는 camBack 100 — 겨눔 선 시작(눈 + 시선 × 350)이 V 몸 겉(눈높이에서 약 365)에서 45 안이라 조종 회전 (요, 0) 그대로 던진다(Grenade.cpp ComputeGrenadeThrow).
		// Lyra 기록의 봇도 조종 회전 (0, 0) 그대로였다(FTPRESS ctlRot · 속도 2472, 0, 370) — 0 을 실으면 선 시작이 V 를 지나 먼 벽을 바라봐 피치가 달라진다(첫 판 1.46)
		struct Cell { const char* name; Vec3 at; Vec3 lookAt; float camBack; bool leave; };
		const Cell cells[] = {
			{ "대조(안 나감)", spotT, eyeV, 100.f, false },
			{ "나감 · 몸에 닿음", spotT, eyeV, 100.f, true },
			{ "나감 · 곁에서 터짐", spotN, spotN + dir * 40.0 + Vec3(0, 0, -90), 0.f, true },
		};
		for (int ci = 0; ci < 3; ++ci)
		{
			const Cell& cell = cells[ci];
			ShotClient& t = clients[1 + ci];
			if (!ConnectAndHello(t.net, port, t.welcome))
			{
				check(false, "던지는 사람 접속 실패");
				continue;
			}
			const uint32_t idT = t.welcome.playerId;
			v.Place(spotV, 1000.f);		// 체력 1000 — 대조 칸 피해로 안 죽는다(옮기기가 부활 무적도 푼다)
			t.Place(cell.at, 100.f);
			PumpFor(clients, 4, 0.15);
			const size_t gFrom = v.grenades.size(), dFrom = v.damages.size(), kFrom = v.kills.size();
			wire::Grenade thrown = wire::Grenade(), boom = wire::Grenade();
			bool hasThrow = false, hasBoom = false;
			const auto scan = [&]
			{
				for (size_t g = gFrom; g < v.grenades.size(); ++g)
				{
					const wire::Grenade& m = v.grenades[g];
					if (m.thrower != idT) continue;
					if (!hasThrow && m.kind == wire::GRENADE_THROW && m.id != 0) { thrown = m; hasThrow = true; }
					else if (hasThrow && !hasBoom && m.id == thrown.id && (m.kind == wire::GRENADE_BOOM_HIT || m.kind == wire::GRENADE_BOOM_TIMER)) { boom = m; hasBoom = true; }
				}
			};
			const Vec3 eyeT = cell.at + Vec3(0, 0, 60);
			const auto send = [&](uint8_t buttons)
			{
				wire::Input input = wire::Input();
				input.seq = ++t.seq;
				AimAt(eyeT, cell.lookAt, input.yaw, input.pitch);
				input.aimX = (float)eyeT.x; input.aimY = (float)eyeT.y; input.aimZ = (float)eyeT.z;
				input.camBack = cell.camBack;
				input.buttons = buttons;
				t.net.SendPacket(input, wire::Msg::C2S_INPUT);
			};
			// 손 자리(몸 앞 5 · 위 25)는 앞 입력까지의 몸 요로 잡힌다 — 옮긴 뒤 첫 입력에 누르면 몸이 아직 V 반대쪽을 봐 손이 뒤쪽 5 에 생긴다
			// (첫 판 생긴 자리가 V 에서 405 · Lyra l3dh 395 — leave_srv_new3 · new4 의 throw 줄).
			// Lyra 기록의 봇은 V 쪽을 본 채 섰다가 던졌다 → 몇 입력 먼저 돌려 둔다
			constexpr int kTurnInputs = 5;
			RunTicks(clients, 4, 60, [&](int i) { send(i == kTurnInputs ? wire::BUTTON_GRENADE : 0); }, [&] { scan(); return hasThrow; });
			struct { bool has; float z; } throwerZ = { t.hasSelf, t.self.z };		// 나가기 전 마지막 내 상태 — 두 자리 바닥 높이가 다르면 Lyra(같은 바닥)와 맞은 점이 달라진다
			if (cell.leave && hasThrow) t.net.Close();		// 던짐 소식을 받자마자 — 서버는 다음 받기에서 끊김을 안다(GameThread OnLeave → RemovePlayer)
			RunTicks(clients, 4, 60 * 4, [&](int) { if (!cell.leave) send(0); }, [&] { scan(); return hasBoom; });
			PumpFor(clients, 4, 0.1);
			int damages = 0;
			float selfDamage = 0.f;		// 던진 사람 자기 피해(대조 칸 — Lyra l3dh 54.8652)
			for (size_t d = dFrom; d < v.damages.size(); ++d)
			{
				if (v.damages[d].weapon != wire::kWeaponGrenade) continue;
				++damages;
				if (v.damages[d].target == idT) selfDamage += v.damages[d].amount;
			}
			const size_t kills = v.kills.size() - kFrom;
			GrenadeBlastTarget body;
			body.center = Vec3(v.self.x, v.self.y, v.self.z);
			const GrenadeBlastHit would = TraceGrenadeBlast(map, Vec3(boom.x, boom.y, boom.z), body);
			const bool byHit = hasBoom && boom.kind == wire::GRENADE_BOOM_HIT;
			// 던진 자리 기준 — 앞 = V 쪽(Lyra l3dh 는 던진 방향 +X 로 같은 값을 읽는다) · 위 = 던진 사람 몸 가운데에서
			const Vec3 fromT = Vec3(boom.x, boom.y, boom.z) - cell.at;
			std::printf("GLEAVE %s 팀=%u(V %u) 몸 높이 차(V − 던진 사람)=%.2f 던짐=%d 번호=%u 던짐 속도=%.2f,%.2f,%.2f 터짐=%s 나이=%d 던진 자리 기준 앞=%.1f 위=%.1f V 까지 몸 거리=%.1f 막힘=%d 같은 코드 피해=%.2f 피해 소식=%d(자기 %.4f) 처치=%zu V 체력=%.2f\n",
				cell.name, t.welcome.team, v.welcome.team, throwerZ.has ? v.self.z - throwerZ.z : 0.f, hasThrow ? 1 : 0, thrown.id, thrown.vx, thrown.vy, thrown.vz, !hasBoom ? "없음" : (byHit ? "맞음" : "신관"), hasBoom ? (int)(boom.tick - thrown.tick) : -1,
				-(fromT.x * dir.x + fromT.y * dir.y), fromT.z, would.bodyDistance, would.blocked ? 1 : 0, would.damage, damages, selfDamage, kills, v.self.hp);
			const bool teamsOk = t.welcome.team != v.welcome.team;
			if (ci == 0) check(teamsOk && hasBoom && byHit && damages > 0 && v.self.hp < 1000.f, "대조 — 안 나가면 상대 몸에 닿아 터지고 피해가 든다");
			else if (ci == 1) check(teamsOk && hasBoom && !byHit && damages == 0 && kills == 0 && v.self.hp == 1000.f, "나감 — 남아 제때 터지고 상대 몸에 닿아도 안 터짐 · 피해 0");
			else check(teamsOk && hasBoom && !byHit && would.damage > 0.0 && !would.blocked && damages == 0 && kills == 0 && v.self.hp == 1000.f, "나감 — 반지름 안 몸도 피해 0 · 소식 0");
			if (!cell.leave) t.net.Close();
			PumpFor(clients, 4, 0.2);
		}
		v.net.Close();
		timeEndPeriod(1);
		std::printf("GLEAVE 실패 %d건\n", fails);
		return fails == 0;
	}

	bool RunAuthority(int port, const CMapCollision& map, const std::string& serverLog)
	{
		int fails = 0;
		const auto check = [&fails](bool ok, const char* text)
		{
			std::printf("AUTH %s %s\n", ok ? "통과" : "실패", text);
			if (!ok) ++fails;
		};
		const auto length = [](const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); };

		ShotClient clients[1];
		ShotClient& a = clients[0];
		if (!ConnectAndHello(a.net, port, a.welcome))
		{
			std::printf("AUTH 접속 실패\n");
			return false;
		}
		timeBeginPeriod(1);
		const auto me = [&a] { return Vec3(a.self.x, a.self.y, a.self.z); };
		const auto eye = [&me] { return me() + Vec3(0, 0, 60); };
		const uint8_t kPistol = (uint8_t)EWeapon::Pistol, kRifle = (uint8_t)EWeapon::Rifle, kShotgun = (uint8_t)EWeapon::Shotgun;
		const auto noStop = [] { return false; };

		// 이동 막음이 풀릴 때까지
		RunTicks(clients, 1, 130, [&](int) { SendAim(a, eye(), eye() + Vec3(100, 0, 0), 0, 0, 0, kPistol); }, noStop);
		PumpFor(clients, 1, 0.1);

		// 1. 시험 입력이 꺼진 서버에 자리 옮기기 — 벽 통과를 가장 쉽게 해 보는 길
		const Vec3 beforePlace = me();
		for (int i = 0; i < 5; ++i) a.Place(Vec3(50000, 50000, 5000), 100.f);
		PumpFor(clients, 1, 0.2);
		std::printf("AUTH 자리 옮기기 뒤 움직인 거리=%.3f\n", length(me() - beforePlace));
		check(length(me() - beforePlace) < 1.0, "시험 입력이 꺼져 있으면 자리를 못 옮김");

		// 2. NaN·무한대
		const uint32_t seqBeforeBad = a.seq;
		for (int i = 0; i < 21; ++i)
		{
			wire::Input bad = wire::Input();
			bad.seq = ++a.seq;
			const float nan = std::numeric_limits<float>::quiet_NaN();
			const float inf = std::numeric_limits<float>::infinity();
			switch (i % 7)
			{
			case 0: bad.moveX = nan; break;
			case 1: bad.moveY = inf; break;
			case 2: bad.yaw = nan; break;
			case 3: bad.pitch = -inf; break;
			case 4: bad.aimX = nan; break;
			case 5: bad.aimY = inf; break;
			default: bad.aimZ = nan; break;
			}
			a.net.SendPacket(bad, wire::Msg::C2S_INPUT);
		}
		PumpFor(clients, 1, 0.2);
		std::printf("AUTH 값 이상 뒤 lastSeq=%u(이상 값 전 %u) 위치=%.1f,%.1f,%.1f\n", a.self.lastSeq, seqBeforeBad, a.self.x, a.self.y, a.self.z);
		check(a.self.lastSeq <= seqBeforeBad && std::isfinite(a.self.x) && std::isfinite(a.self.y) && std::isfinite(a.self.z), "NaN·무한대 입력은 안 쓰이고 상태가 멀쩡함");

		// 3. 가까운 벽을 찾아 이동값을 키워 달린다 — 과속 · 벽 통과
		Vec3 wallDir(1, 0, 0);
		double wallDistance = 1e30;
		for (int d = 0; d < 48; ++d)
		{
			const double angle = d * 2.0 * kPi / 48.0;
			const Vec3 dir(std::cos(angle), std::sin(angle), 0);
			RayHit ray;
			if (!map.Raycast(me(), me() + dir * 1500.0, MAP_BLOCK_MOVE, ray)) continue;
			if (ray.distance < 80.0 || ray.distance >= wallDistance || std::fabs(ray.normal.z) > 0.2) continue;
			wallDistance = ray.distance;
			wallDir = dir;
		}
		const Vec3 walkFrom = me();
		float maxSpeed = 0;
		int bigMoves = 0;
		const size_t historyFrom = a.history.size();
		RunTicks(clients, 1, 360, [&](int)
		{
			++bigMoves;
			SendAim(a, eye(), eye() + wallDir * 100.0, (float)(wallDir.x * 1000.0), (float)(wallDir.y * 1000.0), 0, kPistol);
		}, noStop);
		PumpFor(clients, 1, 0.2);
		for (size_t i = historyFrom; i < a.history.size(); ++i)
		{
			const wire::PlayerState& s = a.history[i];
			maxSpeed = std::max(maxSpeed, (float)std::sqrt(s.vx * s.vx + s.vy * s.vy));
		}
		const Vec3 walkTo = me();
		RayHit crossed;
		const bool passedWall = map.Raycast(walkFrom, walkTo, MAP_BLOCK_MOVE, crossed);
		const bool stuckInWall = map.OverlapCapsule(walkTo, 35, 90, MAP_BLOCK_MOVE);
		RayHit gap;
		const bool wallAhead = map.Raycast(walkTo, walkTo + wallDir * 300.0, MAP_BLOCK_MOVE, gap);
		std::printf("AUTH 벽까지 %.1f cm 방향 %.2f,%.2f · 걸은 거리 %.1f · 최대 수평 속도 %.3f · 벽 앞 간격 %.1f · 지나온 길 막힘 %d · 벽 안 %d\n", wallDistance, wallDir.x, wallDir.y, length(walkTo - walkFrom), maxSpeed, wallAhead ? gap.distance : -1.0, passedWall ? 1 : 0, stuckInWall ? 1 : 0);
		check(maxSpeed <= 600.5f, "이동값을 1000 배로 보내도 최대 속도 600");
		check(wallAhead && gap.distance < 60.0, "벽까지 걸어가 벽 앞에 멈춤");
		check(!passedWall && !stuckInWall, "벽을 통과하지 못함");

		// 4. 벽에 붙어 시작점을 벽 너머로 — 범위 안이어도 벽 앞에서 쏜다
		const size_t shotsBeforeWall = a.shots.size();
		const Vec3 throughWall = me() + wallDir * 85.0 + Vec3(0, 0, 10);	// 수평 90 안 — 범위 검사는 통과하고 벽에 걸린다
		SendAim(a, throughWall, throughWall + wallDir * 1000.0, 0, 0, wire::BUTTON_FIRE, kShotgun);
		RunTicks(clients, 1, 40, [&](int) { SendAim(a, eye(), eye() + wallDir * 100.0, 0, 0, 0, kShotgun); }, noStop);
		PumpFor(clients, 1, 0.1);
		bool pulledOk = a.shots.size() == shotsBeforeWall + 1;
		double farthest = 0;
		if (pulledOk)
		{
			const wire::Shot& shot = a.shots.back();
			const Vec3 start(shot.startX, shot.startY, shot.startZ);
			RayHit startRay;
			if (map.Raycast(me(), start, MAP_BLOCK_WEAPON, startRay)) pulledOk = false;
			for (int i = 0; i < shot.count; ++i)
			{
				const Vec3 point(shot.bullets[i].x, shot.bullets[i].y, shot.bullets[i].z);
				farthest = std::max(farthest, length(point - me()));
			}
			std::printf("AUTH 벽 너머 시작점 %.1f,%.1f,%.1f → 실제 시작 %.1f,%.1f,%.1f · 맞은 점 중 가장 먼 거리 %.1f\n", throughWall.x, throughWall.y, throughWall.z, start.x, start.y, start.z, farthest);
		}
		check(pulledOk && farthest < 150.0, "벽 너머 시작점은 벽 앞으로 당겨 벽을 맞음");

		// 5. 입력을 몰아 보내도 연사가 안 빨라짐
		const size_t shotsBeforeFlood = a.shots.size();
		RunTicks(clients, 1, 120, [&](int)
		{
			for (int i = 0; i < 10; ++i) SendAim(a, eye(), eye() - wallDir * 1000.0, 0, 0, wire::BUTTON_FIRE, kRifle);
		}, noStop);
		PumpFor(clients, 1, 0.6);
		int floodShots = 0, floodSpan = 0;
		ShotSpan(a.shots, kRifle, shotsBeforeFlood, floodShots, floodSpan);
		std::printf("AUTH 몰아 보낸 소총 %d발 · %d틱 동안\n", floodShots, floodSpan);
		check(floodShots >= 2 && floodShots - 1 <= floodSpan / 9 + 2, "입력을 틱당 10개 보내도 소총이 정상 간격(9틱)보다 빨라지지 않음");

		// 6. 무기를 번갈아 바꿔 발사 간격 건너뛰기
		const size_t shotsBeforeSwap = a.shots.size();
		RunTicks(clients, 1, 180, [&](int i) { SendAim(a, eye(), eye() - wallDir * 1000.0, 0, 0, wire::BUTTON_FIRE, i % 2 ? kRifle : kPistol); }, noStop);
		PumpFor(clients, 1, 0.1);
		int swapPistol = 0, swapRifle = 0, spanPistol = 0, spanRifle = 0;
		ShotSpan(a.shots, kPistol, shotsBeforeSwap, swapPistol, spanPistol);
		ShotSpan(a.shots, kRifle, shotsBeforeSwap, swapRifle, spanRifle);
		std::printf("AUTH 번갈아 바꾸기 권총 %d발 %d틱 · 소총 %d발 %d틱\n", swapPistol, spanPistol, swapRifle, spanRifle);
		check(swapPistol >= 2 && swapRifle >= 2 && swapPistol - 1 <= spanPistol / 7 + 1 && swapRifle - 1 <= spanRifle / 7 + 1, "무기를 매 틱 바꿔도 무기별로 7틱에 한 발을 못 넘음");

		// 7. 탄창 0 에서 계속 발사
		RunTicks(clients, 1, 240, [&](int) { SendAim(a, eye(), eye() - wallDir * 1000.0, 0, 0, wire::BUTTON_FIRE, kPistol); }, [&] { return a.self.weapon == kPistol && a.self.magazine == 0; });
		PumpFor(clients, 1, 0.05);
		const size_t shotsAtEmpty = a.shots.size();
		const bool reachedEmpty = a.self.weapon == kPistol && a.self.magazine == 0;
		int stillEmpty = 0, shotsWhileEmpty = 0;
		RunTicks(clients, 1, 60, [&](int) { SendAim(a, eye(), eye() - wallDir * 1000.0, 0, 0, wire::BUTTON_FIRE, kPistol); }, [&]
		{
			if (a.self.magazine != 0) return true;
			++stillEmpty;
			shotsWhileEmpty = (int)(a.shots.size() - shotsAtEmpty);
			return false;
		});
		std::printf("AUTH 탄창 0 도달 %d · 그 뒤 살펴본 횟수 %d · 그동안 발사 %d\n", reachedEmpty ? 1 : 0, stillEmpty, shotsWhileEmpty);
		check(reachedEmpty && stillEmpty > 0 && shotsWhileEmpty == 0, "탄창 0 이면 발사 0");

		// 8. 시작점 위조 — 쏘긴 쏘되 내 몸 근처에서 나간다
		RunTicks(clients, 1, 60, [&](int) { SendAim(a, eye(), eye() + Vec3(100, 0, 0), 0, 0, 0, kShotgun); }, noStop);
		PumpFor(clients, 1, 0.05);
		const size_t shotsBeforeAim = a.shots.size();
		int aimFarSent = 0;
		const Vec3 forged[] = { Vec3(0, 0, 500), Vec3(300, 0, 0), Vec3(150, 0, 0), Vec3(0, 0, 170) };	// 멀리 · 수평으로 140 넘음(한도 바로 위) · 위로 140 넘음 — 한도 140(D66, 전엔 120 에 130 · 150)
		RunTicks(clients, 1, 40, [&](int i)
		{
			++aimFarSent;
			const Vec3 start = me() + forged[i % 4];
			SendAim(a, start, start + Vec3(1000, 0, 0), 0, 0, wire::BUTTON_FIRE, kShotgun);
		}, noStop);
		PumpFor(clients, 1, 0.1);
		const uint64_t aimFarFired = a.shots.size() - shotsBeforeAim;
		double aimFarthest = 0;
		for (size_t i = shotsBeforeAim; i < a.shots.size(); ++i)
		{
			aimFarthest = std::max(aimFarthest, length(Vec3(a.shots[i].startX, a.shots[i].startY, a.shots[i].startZ) - me()));
		}
		std::printf("AUTH 시작점 위조 입력 %d번 · 발사 %llu · 실제 시작점이 내 위치에서 가장 먼 거리 %.1f\n", aimFarSent, (unsigned long long)aimFarFired, aimFarthest);
		check(aimFarFired > 0 && aimFarthest <= 61.0, "범위 밖 시작점은 내 눈높이로 바뀜");

		// 9. 몰아 보내기 — 이동 거리는 틱에 묶인다
		const Vec3 floodFrom = me();
		const uint32_t floodStartSeq = a.seq;
		const auto floodStart = Clock::now();
		RunTicks(clients, 1, 60, [&](int)		// 짧게 — 길면 앞쪽 벽에 막혀 빨라져도 거리가 그대로다
		{
			for (int i = 0; i < 10; ++i) SendAim(a, eye(), eye() - wallDir * 1000.0, (float)-wallDir.x, (float)-wallDir.y, 0, kRifle);
		}, noStop);
		const double floodSeconds = std::chrono::duration<double>(Clock::now() - floodStart).count();
		const Vec3 floodTo = me();
		const double floodDistance = std::sqrt((floodTo.x - floodFrom.x) * (floodTo.x - floodFrom.x) + (floodTo.y - floodFrom.y) * (floodTo.y - floodFrom.y));
		const double floodLimit = 600.0 * floodSeconds + 80.0;		// 한도 8 입력 = 몰아서 한 틱에 80cm
		std::printf("AUTH 입력 %u개 몰아 보냄 %.2f초 · 수평 이동 %.1f cm · 한도 %.1f cm\n", a.seq - floodStartSeq, floodSeconds, floodDistance, floodLimit);
		check(floodDistance <= floodLimit, "입력을 틱당 10개 보내도 빨라지지 않음");
		PumpFor(clients, 1, 0.8);

		// 10. 낡은 번호 · 없는 무기 — 몰아 보낸 끝자락은 버려졌으니 먼저 하나를 받게 한다
		SendAim(a, eye(), eye() + Vec3(100, 0, 0), 0, 0, 0, kRifle);
		for (int i = 0; i < 5; ++i)
		{
			wire::Input old = wire::Input();
			old.seq = a.seq;
			a.net.SendPacket(old, wire::Msg::C2S_INPUT);
		}
		PumpFor(clients, 1, 0.1);
		const uint8_t weaponBefore = a.self.weapon;
		RunTicks(clients, 1, 10, [&](int) { SendAim(a, eye(), eye() + Vec3(100, 0, 0), 0, 0, 0, 99); }, noStop);
		PumpFor(clients, 1, 0.1);
		check(a.self.weapon == weaponBefore, "없는 무기 번호면 든 무기 그대로");

		// 10-1. 입력 번호를 건너뛴 입력의 사격은 버린다(D15). 다음 입력은 번호가 이어져 쏜다 — 대조군
		const Vec3 gapAim = eye() + Vec3(100, 0, 0);
		SendAim(a, eye(), gapAim, 0, 0, wire::BUTTON_RELOAD, kRifle);
		RunTicks(clients, 1, 240, [&](int) { SendAim(a, eye(), gapAim, 0, 0, 0, kRifle); }, noStop);
		PumpFor(clients, 1, 0.1);
		check(a.self.magazine > 0, "번호 시험 전에 탄이 있음");
		const size_t shotsBeforeGap = a.shots.size();
		a.seq += 5;
		SendAim(a, eye(), gapAim, 0, 0, wire::BUTTON_FIRE, kRifle);
		RunTicks(clients, 1, 30, [&](int) { SendAim(a, eye(), gapAim, 0, 0, 0, kRifle); }, noStop);
		PumpFor(clients, 1, 0.1);
		check(a.shots.size() == shotsBeforeGap, "입력 번호를 건너뛴 사격은 안 나감");
		SendAim(a, eye(), gapAim, 0, 0, wire::BUTTON_FIRE, kRifle);
		RunTicks(clients, 1, 30, [&](int) { SendAim(a, eye(), gapAim, 0, 0, 0, kRifle); }, noStop);
		PumpFor(clients, 1, 0.1);
		check(a.shots.size() == shotsBeforeGap + 1, "번호가 이어지면 그 사격은 나감");

		// 11. 깨진 패킷 — 크기 틀림 3 · 모르는 종류 2 · 두 번째 인사 1
		struct ShortInput { wire::Header h; uint32_t seq; };
		for (int i = 0; i < 3; ++i)
		{
			ShortInput shortInput = ShortInput();
			a.net.SendPacket(shortInput, wire::Msg::C2S_INPUT);
		}
		for (int i = 0; i < 2; ++i)
		{
			ShortInput unknown = ShortInput();
			a.net.SendPacket(unknown, (wire::Msg)9999);
		}
		wire::Hello again = wire::Hello();
		again.protocolVersion = wire::kProtocolVersion;
		a.net.SendPacket(again, wire::Msg::C2S_HELLO);
		PumpFor(clients, 1, 0.1);
		const uint32_t seqBeforeAfter = a.self.lastSeq;
		RunTicks(clients, 1, 10, [&](int) { SendAim(a, eye(), eye() + Vec3(100, 0, 0), 0, 0, 0, kRifle); }, noStop);
		PumpFor(clients, 1, 0.1);
		check(a.net.IsOpen() && a.self.lastSeq > seqBeforeAfter, "깨진 패킷 뒤에도 접속이 살아 있음");
		timeEndPeriod(1);

		a.net.Close();
		std::string log;
		for (int i = 0; i < 100 && log.empty(); ++i)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			std::ifstream file(serverLog, std::ios::binary);
			log.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			if (log.find("SERVER kills=") == std::string::npos) log.clear();
		}
		const auto exact = [&](const char* key, uint64_t expected)
		{
			const uint64_t value = ReadCount(log, key);
			std::printf("AUTH %s 개수 %s=%llu 보낸 것 %llu\n", value == expected ? "통과" : "실패", key, (unsigned long long)value, (unsigned long long)expected);
			if (value != expected) ++fails;
		};
		const auto some = [&](const char* key)
		{
			const uint64_t value = ReadCount(log, key);
			const bool ok = value != (uint64_t)-1 && value > 0;
			std::printf("AUTH %s 개수 %s=%llu (0 보다 큼)\n", ok ? "통과" : "실패", key, (unsigned long long)value);
			if (!ok) ++fails;
		};
		exact("debugPlace", 5);
		exact("badValue", 21);
		exact("moveOver", (uint64_t)bigMoves);
		exact("aimWall", 1);
		exact("aimFar", aimFarFired);
		exact("oldSeq", 5);
		exact("seqGapFire", 1);
		exact("weaponId", 10);
		exact("badPackets", 6);
		some("swap");
		some("droppedFull");
		some("fireTooSoon");
		some("fireEmpty");
		std::printf("AUTH 실패 %d건\n", fails);
		return fails == 0;
	}
}

namespace
{
	// ─── 되감기 ────────────────────────────────────────────
	struct RewindSpots
	{
		Vec3 shooter;
		Vec3 view;		// 쏘는 쪽에서 트인 옛 자리
		Vec3 away;		// 옛 자리에서 200 넘게 떨어진 트인 자리 — 보간 안 함
		Vec3 nearby;		// 옛 자리에서 200 안쪽 — 보간함
		Vec3 hidden;	// 쏘는 쪽에서 벽 뒤
	};

	// 캡슐 중심을 바닥 위로 맞춘다. 그 자리에 설 수 없으면 false
	bool SnapToFloor(const CMapCollision& map, const Vec3& at, Vec3& out)
	{
		RayHit floor;
		const Vec3 from = at + Vec3(0, 0, 40);
		if (!map.Raycast(from, from + Vec3(0, 0, -300), MAP_BLOCK_MOVE, floor)) return false;
		out = Vec3(at.x, at.y, from.z - floor.distance + 95.0);
		if (std::fabs(out.z - at.z) > 15.0) return false;	// 낭떠러지·턱 — 같은 높이만 쓴다
		return !map.OverlapCapsule(out + Vec3(0, 0, 5), 35, 90, MAP_BLOCK_MOVE);
	}

	bool FindRewindSpots(const CMapCollision& map, const std::vector<Row>& rows, RewindSpots& out)
	{
		const Vec3 kBody(0, 0, 15);
		for (size_t i = 200; i < rows.size(); i += 97)
		{
			if (rows[i].modeBefore != kModeWalking) continue;
			const Vec3 base = rows[i].after;
			const Vec3 eye = base + Vec3(0, 0, 60);
			const auto open = [&](const Vec3& at)
			{
				RayHit ray;
				return !map.Raycast(eye, at + kBody, MAP_BLOCK_WEAPON, ray);
			};
			Vec3 hidden;
			bool hasHidden = false;
			for (double distance : { 400.0, 600.0, 800.0, 1000.0, 1200.0 })
			{
				for (int d = 0; d < 16 && !hasHidden; ++d)
				{
					const double angle = d * kPi / 8.0;
					Vec3 at;
					if (!SnapToFloor(map, base + Vec3(std::cos(angle), std::sin(angle), 0) * distance, at)) continue;
					if (!open(at))
					{
						hidden = at;
						hasHidden = true;
					}
				}
			}
			if (!hasHidden) continue;
			for (int d = 0; d < 16; ++d)
			{
				const double angle = d * kPi / 8.0;
				const Vec3 dir(std::cos(angle), std::sin(angle), 0);
				const Vec3 perp(-dir.y, dir.x, 0);
				Vec3 view, away, nearby;
				if (!SnapToFloor(map, base + dir * 300.0, view) || !open(view)) continue;
				if (!SnapToFloor(map, view + perp * 260.0, away) || !open(away)) continue;
				if (!SnapToFloor(map, view + perp * 190.0, nearby) || !open(nearby)) continue;
				if (!open(view + (nearby - view) * 0.5)) continue;
				out.shooter = base;
				out.view = view;
				out.away = away;
				out.nearby = nearby;
				out.hidden = hidden;
				return true;
			}
		}
		return false;
	}

	// 서버 판정과 따로 적어 둔 갈래 규칙 — 서버 코드를 부르면 같이 틀려도 시험이 못 잡는다
	enum class EExpectView { NoView, Future, Clamped, Ok };
	EExpectView ExpectView(uint32_t shotTick, uint32_t viewTick, float alpha)
	{
		if (viewTick == 0) return EExpectView::NoView;
		if (viewTick > shotTick || (viewTick == shotTick && alpha > 0.f)) return EExpectView::Future;
		return shotTick - viewTick > 18 ? EExpectView::Clamped : EExpectView::Ok;
	}

	bool RunRewind(int port, const CMapCollision& map, const std::vector<Row>& rows, const std::string& serverLog)
	{
		int fails = 0;
		const auto check = [&fails](bool ok, const char* text)
		{
			std::printf("REWIND %s %s\n", ok ? "통과" : "실패", text);
			if (!ok) ++fails;
		};
		RewindSpots spots;
		if (!FindRewindSpots(map, rows, spots))
		{
			std::printf("REWIND 설 자리를 못 찾음\n");
			return false;
		}
		std::printf("REWIND 자리 쏘는쪽 %.0f,%.0f,%.0f 옛 %.0f,%.0f,%.0f 먼 %.0f,%.0f,%.0f 가까운 %.0f,%.0f,%.0f 벽뒤 %.0f,%.0f,%.0f\n",
			spots.shooter.x, spots.shooter.y, spots.shooter.z, spots.view.x, spots.view.y, spots.view.z, spots.away.x, spots.away.y, spots.away.z, spots.nearby.x, spots.nearby.y, spots.nearby.z, spots.hidden.x, spots.hidden.y, spots.hidden.z);

		ShotClient clients[2];
		ShotClient& a = clients[0];
		ShotClient& b = clients[1];
		if (!ConnectAndHello(a.net, port, a.welcome) || !ConnectAndHello(b.net, port, b.welcome))
		{
			std::printf("REWIND 접속 실패\n");
			return false;
		}
		timeBeginPeriod(1);
		const uint32_t idB = b.welcome.playerId;
		const uint8_t kRifle = (uint8_t)EWeapon::Rifle;
		const Vec3 kBody(0, 0, 15);
		const Vec3 eye = spots.shooter + Vec3(0, 0, 60);

		const auto sendShooter = [&](const Vec3& aim, uint8_t buttons, uint32_t viewTick, float viewAlpha)
		{
			wire::Input input = wire::Input();
			input.seq = ++a.seq;
			AimAt(eye, aim, input.yaw, input.pitch);
			input.aimX = (float)eye.x; input.aimY = (float)eye.y; input.aimZ = (float)eye.z;
			input.buttons = buttons;
			input.weapon = kRifle;
			input.viewTick = viewTick;
			input.viewAlpha = viewAlpha;
			a.net.SendPacket(input, wire::Msg::C2S_INPUT);
		};
		const auto settle = [&](int ticks)
		{
			RunTicks(clients, 2, ticks, [&](int) { sendShooter(spots.view, 0, 0, 0.f); }, [] { return false; });
		};
		// 대상을 옮기고 그 자리가 상태로 돌아올 때까지 기다린다. 반환 = 그 자리가 처음 보인 틱
		const auto moveTarget = [&](const Vec3& at, float hp) -> uint32_t
		{
			const size_t mark = b.history.size();
			b.Place(at, hp);
			for (int i = 0; i < 600; ++i)
			{
				sendShooter(spots.view, 0, 0, 0.f);
				for (ShotClient& c : clients) c.Pump();
				for (size_t h = mark; h < b.history.size(); ++h)
				{
					const wire::PlayerState& s = b.history[h];
					const double dx = s.x - at.x, dy = s.y - at.y, dz = s.z - at.z;
					if (dx * dx + dy * dy < 1.0 && std::fabs(dz) < 10.0 && (s.flags & wire::STATE_DEAD) == 0) return b.historyTicks[h];
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(16));
			}
			return 0;
		};
		const auto waitDead = [&]() -> uint32_t
		{
			const size_t mark = b.history.size() > 4 ? b.history.size() - 4 : 0;
			for (int i = 0; i < 300; ++i)
			{
				sendShooter(spots.view, 0, 0, 0.f);
				for (ShotClient& c : clients) c.Pump();
				for (size_t h = mark; h < b.history.size(); ++h)
				{
					if ((b.history[h].flags & wire::STATE_DEAD) != 0) return b.historyTicks[h];
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(16));
			}
			return 0;
		};

		uint64_t tallyNoView = 0, tallyFuture = 0, tallyClamped = 0, tallyOk = 0;
		int firedExtra = 0;
		// 한 발 쏜다. 받은 탄의 틱으로 갈래를 따로 세어 서버 계수와 맞춰 본다
		const auto fire = [&](const Vec3& aim, uint32_t viewTick, float viewAlpha, EExpectView& outView, bool& outHitTarget) -> bool
		{
			const size_t before = a.shots.size();
			for (int i = 0; i < 40 && a.shots.size() == before; ++i)
			{
				sendShooter(aim, wire::BUTTON_FIRE, viewTick, viewAlpha);
				std::this_thread::sleep_for(std::chrono::milliseconds(16));
				for (ShotClient& c : clients) c.Pump();
			}
			if (a.shots.size() == before) return false;
			sendShooter(aim, 0, 0, 0.f);
			PumpFor(clients, 2, 0.05);
			firedExtra += (int)(a.shots.size() - before) - 1;
			for (size_t s = before; s < a.shots.size(); ++s)
			{
				switch (ExpectView(a.shots[s].tick, viewTick, viewAlpha))
				{
				case EExpectView::NoView: ++tallyNoView; break;
				case EExpectView::Future: ++tallyFuture; break;
				case EExpectView::Clamped: ++tallyClamped; break;
				case EExpectView::Ok: ++tallyOk; break;
				}
			}
			const wire::Shot& shot = a.shots[before];
			outView = ExpectView(shot.tick, viewTick, viewAlpha);
			outHitTarget = shot.bullets[0].hit == (uint8_t)wire::EShotHit::Body && shot.bullets[0].target == idB;
			return true;
		};
		// 되감기 정상 갈래가 필요한 경우 — 바쁜 PC 에서 한도를 넘겼으면 처음부터 다시 만든다(판정 실패가 아니다)
		const auto caseOk = [&](const char* text, bool expectHit, auto&& prepare)
		{
			for (int attempt = 0; attempt < 4; ++attempt)
			{
				settle(30);
				Vec3 aim;
				uint32_t viewTick = 0;
				float alpha = 0.f;
				if (!prepare(aim, viewTick, alpha))
				{
					check(false, text);
					return;
				}
				EExpectView view = EExpectView::NoView;
				bool hit = false;
				if (!fire(aim + kBody, viewTick, alpha, view, hit))
				{
					check(false, text);
					return;
				}
				if (view != EExpectView::Ok) continue;
				check(hit == expectHit, text);
				return;
			}
			check(false, text);
		};
		const auto caseOther = [&](const char* text, EExpectView expectView, bool expectHit, const Vec3& aim, uint32_t viewTick)
		{
			settle(30);
			EExpectView view = EExpectView::NoView;
			bool hit = false;
			const bool fired = fire(aim + kBody, viewTick, 0.f, view, hit);
			check(fired && view == expectView && hit == expectHit, text);
		};

		a.Place(spots.shooter, 100.f);
		b.Place(spots.view, 100.f);
		PumpFor(clients, 2, 0.2);
		settle(90);		// 소총을 든다

		uint32_t oldTick = 0;
		// ① 옮기기 전 틱을 보고 쏘면 맞는다
		caseOk("옮기기 전 틱으로 쏘면 맞음", true, [&](Vec3& aim, uint32_t& view, float&)
		{
			if (!moveTarget(spots.view, 100.f)) return false;
			settle(6);
			const uint32_t first = moveTarget(spots.away, 100.f);
			if (!first) return false;
			oldTick = first - 1;
			aim = spots.view;
			view = oldTick;
			return true;
		});
		// ② 같은 조준 + 시점 없음 = 현재 자리라 빗나간다
		caseOther("같은 조준에 시점 없으면 빗나감", EExpectView::NoView, false, spots.view, 0);
		// ③ 한도보다 과거 — 한도 시점에는 이미 옮겨 간 뒤다
		settle(30);
		caseOther("한도보다 과거면 옛 자리를 쏴도 빗나감", EExpectView::Clamped, false, spots.view, oldTick);
		caseOther("한도보다 과거면 한도 시점 자리는 맞음", EExpectView::Clamped, true, spots.away, oldTick);
		// ④ 미래 틱 = 현재 자리
		const uint32_t farFuture = (b.historyTicks.empty() ? 0 : b.historyTicks.back()) + 100000;
		caseOther("미래 틱이면 현재 자리는 맞음", EExpectView::Future, true, spots.away, farFuture);
		caseOther("미래 틱이면 옛 자리는 빗나감", EExpectView::Future, false, spots.view, farFuture);
		// ⑤ 그 시점에 죽어 있었음
		caseOk("그 시점에 죽어 있었으면 안 맞음", false, [&](Vec3& aim, uint32_t& view, float&)
		{
			if (!moveTarget(spots.view, 1.f)) return false;
			settle(30);
			EExpectView killView = EExpectView::NoView;
			bool killed = false;
			if (!fire(spots.view + kBody, 0, 0.f, killView, killed) || !killed) return false;
			const uint32_t deadTick = waitDead();
			if (!deadTick) return false;
			settle(3);		// 부활시키기 전에 죽은 칸이 기록에 남게
			if (!moveTarget(spots.view, 100.f)) return false;
			settle(4);
			aim = spots.view;
			view = deadTick + 1;	// 쓰러진 틱의 기록은 판정 앞에 적혀 아직 살아 있다 — 그다음 틱부터 죽은 칸
			return true;
		});
		// ⑥ 지금 죽어 있음 — 그때 살아 있었어도 안 맞는다
		caseOk("지금 죽어 있으면 안 맞음", false, [&](Vec3& aim, uint32_t& view, float&)
		{
			if (!moveTarget(spots.view, 1.f)) return false;
			settle(30);
			const uint32_t aliveTick = b.historyTicks.back();
			EExpectView killView = EExpectView::NoView;
			bool killed = false;
			if (!fire(spots.view + kBody, 0, 0.f, killView, killed) || !killed) return false;
			if (!waitDead()) return false;
			aim = spots.view;
			view = aliveTick;
			return true;
		});
		moveTarget(spots.view, 100.f);
		// ⑦ 옛 자리는 트였고 지금 자리는 벽 뒤
		caseOk("옛 자리 트임·지금 벽 뒤면 맞음", true, [&](Vec3& aim, uint32_t& view, float&)
		{
			if (!moveTarget(spots.view, 100.f)) return false;
			settle(6);
			const uint32_t first = moveTarget(spots.hidden, 100.f);
			if (!first) return false;
			aim = spots.view;
			view = first - 1;
			return true;
		});
		// ⑧ 두 틱 거리 200 초과 — 비율이 커도 앞 틱 자리
		caseOk("200 넘게 뛴 사이는 보간 안 함", true, [&](Vec3& aim, uint32_t& view, float& alpha)
		{
			if (!moveTarget(spots.view, 100.f)) return false;
			settle(6);
			const uint32_t first = moveTarget(spots.away, 100.f);
			if (!first) return false;
			aim = spots.view;
			view = first - 1;
			alpha = 0.9f;
			return true;
		});
		// ⑨ 200 안쪽 — 가운데 자리로 보간. 앞 틱·뒤 틱 자리면 95cm 어긋나 빗나간다
		caseOk("200 안쪽 사이는 비율대로 보간", true, [&](Vec3& aim, uint32_t& view, float& alpha)
		{
			if (!moveTarget(spots.view, 100.f)) return false;
			settle(6);
			const uint32_t first = moveTarget(spots.nearby, 100.f);
			if (!first) return false;
			aim = spots.view + (spots.nearby - spots.view) * 0.5;
			view = first - 1;
			alpha = 0.5f;
			return true;
		});
		// ⑩ 비율이 NaN·범위 밖이면 입력째 버린다
		const size_t shotsBeforeBad = a.shots.size();
		const float badAlpha[] = { std::numeric_limits<float>::quiet_NaN(), -0.1f, 1.5f };
		for (float alpha : badAlpha)
		{
			settle(15);
			sendShooter(spots.away, wire::BUTTON_FIRE, 1, alpha);
		}
		sendShooter(spots.away, 0, 0, 0.f);
		PumpFor(clients, 2, 0.2);
		check(a.shots.size() == shotsBeforeBad, "비율이 나쁜 입력은 안 쏨");
		check(firedExtra == 0, "한 번에 한 발만 나감");
		timeEndPeriod(1);

		a.net.Close();
		b.net.Close();
		std::string log;
		for (int i = 0; i < 100 && log.empty(); ++i)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			std::ifstream file(serverLog, std::ios::binary);
			log.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			if (log.find("SERVER kills=") == std::string::npos) log.clear();
		}
		const auto exact = [&](const char* key, uint64_t expected)
		{
			const uint64_t value = ReadCount(log, key);
			std::printf("REWIND %s 개수 %s=%llu 기대 %llu\n", value == expected ? "통과" : "실패", key, (unsigned long long)value, (unsigned long long)expected);
			if (value != expected) ++fails;
		};
		exact("rewindNoView", tallyNoView);
		exact("rewindFuture", tallyFuture);
		exact("rewindClamped", tallyClamped);
		exact("rewindOk", tallyOk);
		exact("badValue", 3);
		exact("rewindOnlyBehindWall", 1);
		const uint64_t bullets = ReadCount(log, "bullets");
		const uint64_t sum = ReadCount(log, "hitsBoth") + ReadCount(log, "hitsRewindOnly") + ReadCount(log, "hitsNowOnly") + ReadCount(log, "hitsNeither");
		std::printf("REWIND %s 네 경우 합 %llu = 탄 %llu\n", sum == bullets && bullets > 0 ? "통과" : "실패", (unsigned long long)sum, (unsigned long long)bullets);
		if (sum != bullets || bullets == 0) ++fails;
		// 옛 자리로만 맞은 경우 ①⑦⑧⑨ 넷. 다시 만든 판이 있으면 더 늘 수 있어 하한만 본다
		const uint64_t rewindOnly = ReadCount(log, "hitsRewindOnly");
		std::printf("REWIND %s hitsRewindOnly=%llu (4 이상)\n", rewindOnly >= 4 && rewindOnly != (uint64_t)-1 ? "통과" : "실패", (unsigned long long)rewindOnly);
		if (rewindOnly < 4 || rewindOnly == (uint64_t)-1) ++fails;
		std::printf("REWIND 실패 %d건\n", fails);
		return fails == 0;
	}

	// ─── 6. 체력 받침(--pads, 컨텐츠 B-2) ─────────────────
	// Lyra 받침 판(record_content_probe.ps1 -Mode pads, MOBContentProbeSubsystem.cpp PADS_SCRIPT)을 같은 차례 · 같은 길이 · 같은 자리로 돌린다.
	// 순간이동 · 체력 정하기는 시험 입력(C2S_DEBUG_PLACE) 하나로 한다 — Lyra 의 SetHealth 단계는 그 자리(집)에 다시 놓으며 체력만 바꾼다.
	// 여기는 구동과 기록만: 단계마다 자리 옮긴 틱 · 체력이 바뀐 틱(상태 패킷)을 PADPROBE 줄로, 서버 사건은 서버 출력의 PADEV 줄을 그대로 옮겨 찍는다
	bool RunPads(int port, const std::string& serverLog)
	{
		ShotClient client;
		if (!ConnectAndHello(client.net, port, client.welcome))
		{
			std::printf("PADPROBE 접속 실패\n");
			return false;
		}
		timeBeginPeriod(1);
		// 자리 = Lyra 기록 cprobe_pads2.log — 집 MOB_CPROBE_READY home, 받침 위 MOB_CPROBE_STAND spot(바닥 + 반높이 + 3)
		const Vec3 home(-4300.0, 0.0, -368.010);
		const Vec3 bigSpot(0.0, 0.0, -1206.475);
		const Vec3 smallSpot(0.0, -1086.0, 378.0);
		enum class EAct { Away, PadBig, PadSmall, SetHealth40 };
		struct Step { const char* name; double seconds; EAct act; };
		// PADS_SCRIPT 와 같은 줄 — settle 은 Lyra 에서 받침 정보만 찍는 단계라 집에 선다
		const Step steps[] = {
			{ "settle", 1.0, EAct::Away },
			{ "big_full", 2.0, EAct::PadBig },
			{ "away1", 1.0, EAct::Away },
			{ "hp40", 0.5, EAct::SetHealth40 },
			{ "big_hurt", 52.0, EAct::PadBig },
			{ "away2", 1.0, EAct::Away },
			{ "hp40b", 0.5, EAct::SetHealth40 },
			{ "small_hurt", 22.0, EAct::PadSmall },
			{ "end", 0.5, EAct::Away },
		};
		PumpFor(&client, 1, 0.3);
		size_t scanned = 0;
		float lastHp = client.hasSelf ? client.self.hp : -1.f;
		const auto scan = [&](const char* step, uint32_t placeSeq, bool& placeSeen)
		{
			for (; scanned < client.history.size(); ++scanned)
			{
				const wire::PlayerState& s = client.history[scanned];
				const uint32_t tick = client.historyTicks[scanned];
				if (!placeSeen && s.lastSeq >= placeSeq)
				{
					placeSeen = true;
					std::printf("PADPROBE step=%s placeTick=%u hp=%.1f at=(%.3f,%.3f,%.3f)\n", step, tick, s.hp, s.x, s.y, s.z);
				}
				if (s.hp != lastHp)
				{
					std::printf("PADPROBE step=%s hpTick=%u hp=%.1f>%.1f\n", step, tick, lastHp, s.hp);
					lastHp = s.hp;
				}
			}
		};
		int placesSeen = 0;
		for (const Step& step : steps)
		{
			const float hpNow = client.hasSelf ? client.self.hp : 100.f;
			switch (step.act)
			{
			case EAct::Away: client.Place(home, hpNow); break;
			case EAct::PadBig: client.Place(bigSpot, hpNow); break;
			case EAct::PadSmall: client.Place(smallSpot, hpNow); break;
			case EAct::SetHealth40: client.Place(home, 40.f); break;
			}
			const uint32_t placeSeq = client.seq;
			bool placeSeen = false;
			RunTicks(&client, 1, (int)std::lround(step.seconds * 60.0),
				[&](int) { SendAim(client, home, home + Vec3(100, 0, 0), 0, 0, 0, 0); scan(step.name, placeSeq, placeSeen); }, [] { return false; });
			PumpFor(&client, 1, 0.05);
			scan(step.name, placeSeq, placeSeen);
			if (placeSeen) ++placesSeen;
		}
		timeEndPeriod(1);
		client.net.Close();

		std::string log;
		for (int i = 0; i < 100 && log.empty(); ++i)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			std::ifstream file(serverLog, std::ios::binary);
			log.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			if (log.find("SERVER kills=") == std::string::npos) log.clear();
		}
		int events = 0;
		size_t at = 0;
		while (at < log.size())
		{
			size_t end = log.find('\n', at);
			if (end == std::string::npos) end = log.size();
			const std::string line = log.substr(at, end - at);
			if (line.rfind("PADEV ", 0) == 0 || line.rfind("SERVER pickups=", 0) == 0 || line.rfind("PADS ", 0) == 0)
			{
				std::printf("%s\n", line.c_str());
				if (line.rfind("PADEV ", 0) == 0) ++events;
			}
			at = end + 1;
		}
		const int stepCount = (int)(sizeof(steps) / sizeof(steps[0]));
		std::printf("PADPROBE 단계 %d 중 자리 옮김 확인 %d · 서버 출력 %s · 받침 사건 %d줄\n", stepCount, placesSeen, log.empty() ? "못 읽음" : "읽음", events);
		return placesSeen == stepCount && !log.empty() && events > 0;
	}

	// ─── 6-1. 무기 받침(--weapons, 컨텐츠 W-2) ─────────────
	// Lyra 무기 판(record_content_probe.ps1 -Mode weapons, MOBContentProbeSubsystem.cpp WEAPONS_SCRIPT)을 같은 차례 · 같은 길이 · 같은 자리로 돌린다.
	// 순간이동 · 탄 정하기 · 스스로 죽음은 시험 입력(C2S_DEBUG_PLACE, flags)으로, 무기 고르기 · 재장전은 보통 입력으로 한다.
	// 여기는 구동과 기록만: 단계마다 첫 메시지를 서버가 쓴 틱(WPROBE startTick) · 내 상태의 든 무기 · 탄이 바뀐 틱(WPROBE seen)을 찍고,
	// 서버 사건은 서버 출력의 PADEV · INVEV · PAD 줄을 그대로 옮겨 찍는다. 대조는 Tools/LyraPilot/check_weapon_pads.py
	bool RunWeapons(int port, const std::string& serverLog)
	{
		ShotClient client;
		if (!ConnectAndHello(client.net, port, client.welcome))
		{
			std::printf("WPROBE 접속 실패\n");
			return false;
		}
		timeBeginPeriod(1);
		// 자리 = Lyra 기록 cprobe_w0a.log — 집 MOB_CPROBE_READY home, 받침 위 MOB_CPROBE_STAND spot
		const Vec3 home(4700.0, -250.0, -368.100);
		const Vec3 spots[4] = {
			Vec3(3377.4, -1830.0, -368.031),	// 0 = R1 소총(B_WeaponSpawner1)
			Vec3(-3377.4, 1830.0, -368.031),	// 1 = R3 소총(B_WeaponSpawner3)
			Vec3(-4586.5, 1008.4, -368.031),	// 2 = P7 권총(B_WeaponSpawner7)
			Vec3(1472.0, -6.7, -1205.475),		// 3 = S5 샷건(B_WeaponSpawner5)
		};
		struct AmmoSet { uint8_t weapon; int16_t magazine, spare; };
		const AmmoSet ammoSets[4] = { { 1, 10, 20 }, { 0, 4, 48 }, { 1, 10, 60 }, { 2, 2, 16 } };	// Lyra AMMO_SETS 와 같은 차례
		enum class EAct { Home, Pad, Select, SetAmmo, Reload, Kill, None };
		struct Step { const char* name; double seconds; EAct act; int arg; };
		// WEAPONS_SCRIPT 와 같은 줄. settle 은 Lyra 에서 받침 정보만 찍는 단계라 집에 선다.
		// Select 의 arg 는 무기 번호 — Lyra 는 칸 번호를 골랐고 그때 가방이 [권총, 소총, 샷건] 이라 칸 = 무기 번호다
		const Step steps[] = {
			{ "settle", 1.0, EAct::Home, 0 },
			{ "rifle_r1", 14.0, EAct::Pad, 0 },
			{ "away1", 1.0, EAct::Home, 0 },
			{ "slot0", 1.5, EAct::Select, 0 },
			{ "rifle_low", 0.5, EAct::SetAmmo, 0 },
			{ "rifle_r3", 2.0, EAct::Pad, 1 },
			{ "away2", 1.0, EAct::Home, 0 },
			{ "pistol_p7", 2.0, EAct::Pad, 2 },
			{ "away3", 1.0, EAct::Home, 0 },
			{ "pistol_low", 0.5, EAct::SetAmmo, 1 },
			{ "reload_press", 0.1, EAct::Reload, 0 },
			{ "shotgun_s5", 3.0, EAct::Pad, 3 },
			{ "away4", 1.0, EAct::Home, 0 },
			{ "pistol_low2", 0.5, EAct::SetAmmo, 1 },
			{ "reload_pistol", 3.0, EAct::Reload, 0 },
			{ "slot1", 1.5, EAct::Select, 1 },
			{ "rifle_low2", 0.5, EAct::SetAmmo, 2 },
			{ "reload_rifle", 3.0, EAct::Reload, 0 },
			{ "slot2", 1.5, EAct::Select, 2 },
			{ "shotgun_low", 0.5, EAct::SetAmmo, 3 },
			{ "reload_shotgun", 4.0, EAct::Reload, 0 },
			{ "die", 15.0, EAct::Kill, 0 },
			{ "end", 1.0, EAct::None, 0 },
		};
		PumpFor(&client, 1, 0.3);
		// 보내는 무기 번호 = 다리 · 봇이 할 일과 같다 — 키로 고른 무기, 그리고 서버가 줍기 · 부활로 무기를 바꿨으면(내 집음 · 부활 소식 뒤 그 틱 상태의 든 무기) 그 무기.
		// 「앞 입력과 달라졌을 때만 바꾼다」(Shared/Loadout.h) 규칙이라 줍기로 바뀐 뒤 옛 번호를 계속 보내도 되돌아가지 않고, 따라 맞춘 뒤에 키를 눌러야 바뀐다
		uint8_t wanted = client.hasSelf ? client.self.weapon : 0;
		size_t pickupsSeen = 0, spawnsSeen = 0, scanned = 0;
		uint32_t adoptFromTick = 0;
		bool adopt = false;
		int32_t lastWeapon = -1, lastMag = -1, lastSpare = -1, lastSlots = -1;
		constexpr int kStepCount = (int)(sizeof(steps) / sizeof(steps[0]));
		uint32_t firstSeqs[kStepCount] = {};
		int stepsSent = 0, stepsStarted = 0;
		const auto scan = [&]()
		{
			for (; pickupsSeen < client.pickups.size(); ++pickupsSeen)
			{
				const wire::Pickup& m = client.pickups[pickupsSeen];
				if ((m.reason == wire::PICKUP_TAKE || m.reason == wire::PICKUP_TAKE_RECHECK) && m.picker == client.welcome.playerId)
				{
					adoptFromTick = adopt ? std::max(adoptFromTick, m.tick) : m.tick;	// 둘 다 기다리면 늦은 쪽 — 그 틱 상태가 두 일을 다 담는다
					adopt = true;
				}
			}
			// 부활하면 가방이 [권총] 으로 돌아간다 — 그 틱 상태의 든 무기로 맞춘다(안 맞추면 죽기 전 고른 무기를 안 가진 무기로 계속 보낸다)
			for (; spawnsSeen < client.spawns.size(); ++spawnsSeen)
			{
				if (client.spawns[spawnsSeen].player != client.welcome.playerId) continue;
				adoptFromTick = adopt ? std::max(adoptFromTick, client.spawns[spawnsSeen].tick) : client.spawns[spawnsSeen].tick;
				adopt = true;
			}
			for (; scanned < client.history.size(); ++scanned)
			{
				const wire::PlayerState& s = client.history[scanned];
				const uint32_t tick = client.historyTicks[scanned];
				// 단계 시작 = 그 단계 첫 입력을 서버가 쓴 틱(시험 입력은 그 뒤에 보내 같은 틱에 쓰인다). 상태의 lastSeq 로 안다
				while (stepsStarted < stepsSent && s.lastSeq >= firstSeqs[stepsStarted])
				{
					std::printf("WPROBE step=%s startTick=%u\n", steps[stepsStarted].name, tick);
					++stepsStarted;
				}
				const char* stepName = stepsStarted > 0 ? steps[stepsStarted - 1].name : "prime";
				if ((int32_t)s.weapon != lastWeapon || s.magazine != lastMag || s.spare != lastSpare || (int32_t)s.quickSlots != lastSlots)
				{
					lastWeapon = s.weapon;
					lastMag = s.magazine;
					lastSpare = s.spare;
					lastSlots = s.quickSlots;
					std::printf("WPROBE seen tick=%u step=%s weapon=%u mag=%d spare=%d dead=%d slots=%u\n", tick, stepName, (unsigned)s.weapon, (int)s.magazine, (int)s.spare, (s.flags & wire::STATE_DEAD) ? 1 : 0, (unsigned)s.quickSlots);
				}
				if (adopt && tick >= adoptFromTick)
				{
					adopt = false;
					if (s.weapon != wanted) std::printf("WPROBE adopt tick=%u weapon=%u>%u\n", tick, (unsigned)wanted, (unsigned)s.weapon);
					wanted = s.weapon;
				}
			}
		};
		// 각본 앞 채우기 — 서버 입력 줄에 늘 2개가 서 있게 한다(한 틱에 꺼내는 한도 kMaxInputTokens = 8 을 먼저 다 쓰게 8 + 2 를 몰아 보냄).
		// 입력이 틱마다 하나씩 꼭 쓰여야 서버 틱 = 입력 수 = Lyra 프레임이 된다 — 늦게 온 입력으로 빈 틱(starvedTicks)이 나면 단계가 밀린다(첫 판 311틱)
		constexpr int kPrimeBurst = 8 + 2;
		constexpr int kPrimeTicks = 60;
		client.Place(home, client.hasSelf ? client.self.hp : 100.f);
		for (int i = 0; i < kPrimeBurst; ++i) SendAim(client, home, home + Vec3(100, 0, 0), 0, 0, 0, wanted);
		int scriptTicks = 0;
		int stepEnds[kStepCount] = {};
		for (int i = 0; i < kStepCount; ++i)
		{
			scriptTicks += (int)std::lround(steps[i].seconds * 60.0);
			stepEnds[i] = scriptTicks;
		}
		// 단계 사이에 쉬지 않고 한 줄로 보낸다 — 단계마다 따로 돌리면 사이 0.05초(3틱)마다 입력이 빈다
		int stepAt = -1;
		RunTicks(&client, 1, kPrimeTicks + scriptTicks, [&](int i)
		{
			const int scriptTick = i - kPrimeTicks;
			if (scriptTick < 0 || (stepAt >= 0 && scriptTick < stepEnds[stepAt]))
			{
				SendAim(client, home, home + Vec3(100, 0, 0), 0, 0, 0, wanted);
				scan();
				return;
			}
			// 단계 첫 틱 — 시험 입력(자리 · 탄 · 죽음)은 그 틱 입력 「뒤」에 보낸다. 시험 입력은 한도를 안 쓰고 줄에 닿는 대로 쓰여서(TickPlayerInputs),
			// 입력 앞에 두면 앞 틱 마지막 입력과 같은 틱에 쓰인다. 뒤에 두면 이 단계 첫 입력과 같은 틱 = Lyra 가 그 단계 첫 프레임에 한 일
			++stepAt;
			const Step& step = steps[stepAt];
			const float hpNow = client.hasSelf ? client.self.hp : 100.f;
			firstSeqs[stepAt] = client.seq + 1;
			stepsSent = stepAt + 1;
			if (step.act == EAct::Select) wanted = (uint8_t)step.arg;
			SendAim(client, home, home + Vec3(100, 0, 0), 0, 0, step.act == EAct::Reload ? (uint8_t)wire::BUTTON_RELOAD : (uint8_t)0, wanted);	// 재장전은 누른 입력 하나에만(EButton)
			switch (step.act)
			{
			case EAct::Home: client.Place(home, hpNow); break;
			case EAct::Pad: client.Place(spots[step.arg], hpNow); break;
			case EAct::SetAmmo:
			{
				const AmmoSet& a = ammoSets[step.arg];
				client.Place(home, hpNow, wire::DEBUG_PLACE_SET_AMMO, a.weapon, a.magazine, a.spare);
				break;
			}
			case EAct::Kill: client.Place(home, hpNow, wire::DEBUG_PLACE_KILL); break;
			case EAct::Select:
			case EAct::Reload:
			case EAct::None:
				break;
			}
			scan();
		}, [] { return false; });
		PumpFor(&client, 1, 0.3);
		scan();
		timeEndPeriod(1);
		client.net.Close();

		// 받침 소식 전부(W-3) — 대조 도구가 종류 · 줍기 칸을 서버 PADEV 줄과 견준다. 소식은 그 틱 상태보다 먼저 오므로 입력 번호는 다 받은 뒤 상태로 본다:
		// 그 틱 상태의 lastSeq(그 틱에 쓴 마지막 입력) · 앞 틱 상태의 lastSeq. 상태가 없으면 -1
		for (const wire::Pickup& m : client.pickups)
		{
			int64_t stateSeq = -1, prevSeq = -1;
			for (size_t i = 0; i < client.history.size(); ++i)
			{
				if (client.historyTicks[i] == m.tick) stateSeq = client.history[i].lastSeq;
				if (client.historyTicks[i] + 1 == m.tick) prevSeq = client.history[i].lastSeq;
			}
			std::printf("WPROBE padmsg tick=%u pad=%u reason=%u picker=%u kind=%u give=%u weapon=%u slot=%u mag=%d spare=%d seq=%u stateSeq=%lld prevSeq=%lld\n", m.tick, (unsigned)m.pad,
				(unsigned)m.reason, m.picker, (unsigned)m.kind, (unsigned)m.give, (unsigned)m.weapon, (unsigned)m.slot, (int)m.magazine, (int)m.spare, m.seq, (long long)stateSeq, (long long)prevSeq);
		}

		std::string log;
		for (int i = 0; i < 100 && log.empty(); ++i)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			std::ifstream file(serverLog, std::ios::binary);
			log.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			if (log.find("SERVER kills=") == std::string::npos) log.clear();
		}
		int padEvents = 0, invEvents = 0;
		size_t at = 0;
		while (at < log.size())
		{
			size_t end = log.find('\n', at);
			if (end == std::string::npos) end = log.size();
			std::string line = log.substr(at, end - at);
			if (!line.empty() && line.back() == '\r') line.pop_back();
			const bool pad = line.rfind("PADEV ", 0) == 0;
			const bool inv = line.rfind("INVEV ", 0) == 0;
			if (pad || inv || line.rfind("SERVER pickups=", 0) == 0 || line.rfind("SERVER weaponPickups ", 0) == 0 || line.rfind("PADS ", 0) == 0 || line.rfind("PAD ", 0) == 0)
			{
				std::printf("%s\n", line.c_str());
				if (pad) ++padEvents;
				if (inv) ++invEvents;
			}
			at = end + 1;
		}
		const int stepCount = (int)(sizeof(steps) / sizeof(steps[0]));
		std::printf("WPROBE player=%u 단계 %d 중 시작 확인 %d · 서버 출력 %s · 받침 사건 %d줄 · 가방 사건 %d줄\n", client.welcome.playerId, stepCount, stepsStarted, log.empty() ? "못 읽음" : "읽음", padEvents, invEvents);
		return stepsStarted == stepCount && !log.empty() && padEvents > 0 && invEvents > 0;
	}

	// ─── 7. 근접(--melee, 컨텐츠 E-3) ─────────────────────
	// Lyra 연습장 근접 판(record_content_probe.ps1 -Mode gym → cprobe_<tag>.log · .csv)의 칸마다 공격자 · 상대를 Lyra 자리에 놓고,
	// 같은 요로 선 뒤 근접을 한 번 누른다. 견주는 것
	//   결과 = 서버 근접 소식(S2C_MELEE 결과 · 대상 · 피해) ↔ Lyra(MOB_CPROBE_MELEE hit · dmg, 같은 팀 칸은 쓸기 줄 hit)
	//   밀린 거리 = 서버 상태의 수평 이동 ↔ Lyra lunge2d. Lyra 는 미끄러지다 상대 캡슐에 닿으면 멈춘다 — 플레이어끼리 부딪힘(컨텐츠 F)이 없는
	//     서버는 더 간다. 닿은 칸(기록 표의 공격자 ↔ 상대 수평 거리 최솟값이 두 반지름 + 1 안)은 거리를 견주지 않고 따로 센다
	//   손 · 맞은 거리 = 서버 시험 줄 MELEEEV(--debug) ↔ Lyra 쓸기 줄 MOB_CPROBE_MTRACE which=prev(앞 프레임 손 — 능력이 쓰는 손)
	// 손이 다르면(서버는 자기 애니 자세의 손, 클립 없이 뜨면 기본 자세 손) 맞은 거리 · 밀린 거리가 그만큼 다르다 — 그 크기를 재는 것이 목적이고,
	// 통과 조건은 결과 · 대상 · 피해 · 안 맞은 칸의 안 밀림만이다
	constexpr int kMeleeSettleTicks = 40;		// 놓은 뒤 서 있는 틱 — 앞 입력의 요(몸 앞) · 애니 서기 · 되감기 기록이 놓은 자리로 찬다
	constexpr int kMeleeChangeTicks = 120;		// 무기를 바꿨거나 앉는 칸은 더 선다 — 무기 꺼내기 · 앉기 전환 자세가 끝나게(Lyra 칸은 오래 든 무기 · 앉은 자세에서 눌렀다)
	constexpr int kMeleeSlideTicks = 120;		// 누른 뒤 — 밀기 3프레임 뒤 보통 제동으로 멈출 때까지(Lyra 칸 하나 0.75초 안에 멈춘다)

	struct MeleeCell
	{
		std::string step;
		Vec3 center, foe, fwd, hand;	// 공격자 캡슐 중심 · 상대 캡슐 중심 · 몸 앞 · 앞 프레임 손(Lyra)
		Vec3 start;					// 근접 직전 공격자 자리 = 기록 표에서 능력이 켜진 줄 바로 앞 줄. 쓸기 줄 center 는 밀기가 한 프레임 들어간 뒤다(맞은 칸에서 힘 ÷ 60 만큼 앞, 실측)
		bool hasStart = false;
		float foeYaw = 0;
		bool sameTeam = false;		// 앞선 MOB_CPROBE_FOE_SAME 줄 — 상대가 같은 팀(MeleeCheck 와 같은 읽기)
		bool crouched = false;		// 공격자가 앉아 있음
		bool traceHit = false;		// 기록 도구 쓸기가 몸에 닿음
		bool traceWall = false;		// 그 손 → 닿은 점 선이 벽에 막힘(wall=1)
		double traceDist = 0;		// 그 맞은 거리
		EWeapon weapon = EWeapon::Pistol;	// 공격자 몽타주(myMontage AM_MM_<무기>_Melee)
		bool hasResult = false;
		bool lyraHit = false;		// 능력이 맞힘(피해 줄)
		float lyraDamage = 0;
		double lyraLunge = 0;
		double contactGap = 1e9;	// 칸 안에서 공격자 ↔ 상대 수평 거리 최솟값
		// 17-d 걸으며 · 뛰며 칸 — 칸 첫 줄 자리에서 줄마다 Lyra 와 같은 입력을 보내 근접 직전 자리까지 따라간다
		int runPhase = 0;				// 0 = 첫 줄 전 · 1 = 근접 앞 · 2 = 근접 뒤
		bool hasRun = false;			// 첫 줄이 멈춰 있다(거기서 출발할 수 있다)
		bool moving = false;			// 근접 앞이나 근접 입력에 +X 누름 · 점프가 있다
		bool walking = false;			// 칸 어디든 +X 누름 — 밀린 거리에 걸음이 섞인다
		Vec3 runStart;					// 칸 첫 줄 자리
		std::vector<uint8_t> runWalk, runJump;	// 근접 입력 앞 이동마다 +X 누름 · 점프 누름
		uint8_t meleeWalk = 0;			// 근접 입력(능력이 켜진 줄의 이동)의 +X 누름
		std::vector<uint8_t> slideWalk;	// 근접 입력 뒤 이동마다 +X 누름
	};

	// 줄에서 " 이름=" 뒤 값(빈칸까지)
	std::string FieldValue(const std::string& line, const char* key)
	{
		const std::string pattern = std::string(" ") + key + "=";
		const size_t at = line.find(pattern);
		if (at == std::string::npos) return std::string();
		const size_t from = at + pattern.size();
		const size_t end = line.find(' ', from);
		return line.substr(from, end == std::string::npos ? std::string::npos : end - from);
	}

	bool FieldVec(const std::string& line, const char* key, Vec3& out)
	{
		const std::string v = FieldValue(line, key);
		double x = 0, y = 0, z = 0;
		if (v.empty() || std::sscanf(v.c_str(), "%lf,%lf,%lf", &x, &y, &z) != 3) return false;
		out = Vec3(x, y, z);
		return true;
	}

	bool ReadMeleeCells(const std::string& logPath, const std::string& csvPath, std::vector<MeleeCell>& cells)
	{
		std::ifstream log(logPath, std::ios::binary);
		if (!log) return false;
		std::map<std::string, size_t> byStep;
		std::string line;
		bool sameTeam = false;
		while (std::getline(log, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			const std::string step = FieldValue(line, "step");
			if (line.find("MOB_CPROBE_FOE_SAME ") != std::string::npos) sameTeam = true;
			else if (line.find("MOB_CPROBE_FOE ") != std::string::npos) sameTeam = false;
			else if (line.find("MOB_CPROBE_MTRACE ") != std::string::npos && FieldValue(line, "which") == "prev")
			{
				if (byStep.count(step)) continue;		// 칸마다 첫 근접만. 연타 칸(spam_*)은 MELEE 줄이 없어 아래에서 빠진다
				MeleeCell cell;
				cell.step = step;
				if (!FieldVec(line, "center", cell.center) || !FieldVec(line, "foe", cell.foe) || !FieldVec(line, "fwd", cell.fwd) || !FieldVec(line, "hand", cell.hand)) continue;
				cell.foeYaw = (float)std::atof(FieldValue(line, "foeYaw").c_str());
				cell.traceHit = FieldValue(line, "hit") == "1";
				cell.traceWall = FieldValue(line, "wall") == "1";
				if (cell.traceHit) cell.traceDist = std::atof(FieldValue(line, "dist").c_str());
				cell.crouched = FieldValue(line, "crouched") == "1";
				cell.sameTeam = sameTeam;
				byStep[step] = cells.size();
				cells.push_back(cell);
			}
			else if (line.find("MOB_CPROBE_MELEE ") != std::string::npos)
			{
				const auto it = byStep.find(step);
				if (it == byStep.end()) continue;
				MeleeCell& cell = cells[it->second];
				cell.hasResult = true;
				cell.lyraHit = FieldValue(line, "hit") == "1";
				cell.lyraDamage = (float)std::atof(FieldValue(line, "dmg").c_str());
				cell.lyraLunge = std::atof(FieldValue(line, "lunge2d").c_str());
				const std::string montage = FieldValue(line, "myMontage");
				cell.weapon = montage.find("Rifle") != std::string::npos ? EWeapon::Rifle : montage.find("Shotgun") != std::string::npos ? EWeapon::Shotgun : EWeapon::Pistol;
			}
		}
		cells.erase(std::remove_if(cells.begin(), cells.end(), [](const MeleeCell& c) { return !c.hasResult; }), cells.end());		// 근접 직전 자리가 없는 칸은 아래 표 읽기 뒤에 뺀다
		byStep.clear();
		for (size_t i = 0; i < cells.size(); ++i) byStep[cells[i].step] = i;

		// 닿음 · 근접 직전 자리 — 기록 표(frame,t,step,ev,px,py,pz,... 22 foeX, 23 foeY). Lyra lunge2d 도 앞 줄 자리부터 잰 값이다
		std::ifstream csv(csvPath, std::ios::binary);
		if (!csv) return false;
		std::getline(csv, line);
		int colWalk = -1;		// 17-d 칸 walk(그 줄 프레임에 +X 누름). 옛 기록엔 없다
		{
			size_t from = 0;
			for (int i = 0;; ++i)
			{
				const size_t comma = line.find(',', from);
				std::string name = line.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
				while (!name.empty() && (name.back() == '\r' || name.back() == '\n')) name.pop_back();
				if (name == "walk") colWalk = i;
				if (comma == std::string::npos) break;
				from = comma + 1;
			}
		}
		const auto walkOf = [&](const std::vector<std::string>& row) -> uint8_t { return colWalk >= 0 && (int)row.size() > colWalk && std::atoi(row[(size_t)colWalk].c_str()) != 0 ? 1 : 0; };
		const auto jumpOf = [](const std::vector<std::string>& row) -> uint8_t { return row.size() > 3 && row[3].find("jump_press;") != std::string::npos ? 1 : 0; };
		std::vector<std::string> previous;
		while (std::getline(csv, line))
		{
			std::vector<std::string> cols;
			size_t from = 0;
			while (true)
			{
				const size_t comma = line.find(',', from);
				cols.push_back(line.substr(from, comma == std::string::npos ? std::string::npos : comma - from));
				if (comma == std::string::npos) break;
				from = comma + 1;
			}
			if (cols.size() < 24) continue;
			const std::vector<std::string> before = previous;
			previous = cols;
			const auto it = byStep.find(cols[2]);
			if (it == byStep.end()) continue;
			MeleeCell& cell = cells[it->second];
			const bool sameStepBefore = before.size() >= 24 && before[2] == cols[2];
			if (cell.runPhase == 0)
			{
				// 칸 첫 줄 — 근접 칸은 시작에 남은 입력을 비우고 순간이동하므로 멈춰 있어야 한다(vz · speed2d 0)
				cell.runStart = Vec3(std::atof(cols[4].c_str()), std::atof(cols[5].c_str()), std::atof(cols[6].c_str()));
				cell.hasRun = std::atof(cols[9].c_str()) == 0.0 && std::atof(cols[10].c_str()) == 0.0;
				cell.runPhase = 1;
			}
			else if (sameStepBefore)
			{
				const uint8_t walk = walkOf(before), jump = jumpOf(before);
				if (cell.runPhase == 1 && cols[3].find("melee_start;") != std::string::npos)
				{
					cell.meleeWalk = walk;
					cell.runPhase = 2;
				}
				else if (cell.runPhase == 1)
				{
					cell.runWalk.push_back(walk);
					cell.runJump.push_back(jump);
				}
				else cell.slideWalk.push_back(walk);
				if (walk) cell.walking = true;
				if (cell.runPhase == 1 && (walk || jump)) cell.moving = true;
				if (cell.runPhase == 2 && cell.meleeWalk) cell.moving = true;
			}
			if (!cell.hasStart && cols[3].find("melee_start;") != std::string::npos && before.size() >= 24 && before[2] == cols[2])
			{
				cell.start = Vec3(std::atof(before[4].c_str()), std::atof(before[5].c_str()), std::atof(before[6].c_str()));
				cell.hasStart = true;
			}
			const double dx = std::atof(cols[4].c_str()) - std::atof(cols[22].c_str());
			const double dy = std::atof(cols[5].c_str()) - std::atof(cols[23].c_str());
			cell.contactGap = std::min(cell.contactGap, std::sqrt(dx * dx + dy * dy));
		}
		const size_t read = cells.size();
		for (const MeleeCell& c : cells)
		{
			if (c.moving && !c.hasRun) std::printf("MELEEPROBE step=%s 움직이는 칸인데 첫 줄이 멈춰 있지 않음 — 뺌\n", c.step.c_str());
		}
		cells.erase(std::remove_if(cells.begin(), cells.end(), [](const MeleeCell& c) { return !c.hasStart || (c.moving && !c.hasRun); }), cells.end());
		if (cells.size() != read) std::printf("MELEEPROBE 기록 표에 근접 시작 줄이 없는 칸 %zu 뺌\n", read - cells.size());
		return !cells.empty() && cells.size() == read;
	}

	// 칸에 안 쓰는 사람을 세울 먼 자리 — 바닥이 있고 맵과 안 겹친다
	bool ParkSpot(const CMapCollision& map, const Vec3& from, Vec3& out)
	{
		for (double distance : { 1500.0, 1200.0, 2000.0, 1000.0 })
		{
			for (int d = 0; d < 16; ++d)
			{
				const double angle = d * kPi / 8.0;
				// 바닥을 위에서 찾고 그 위에 선 캡슐(반높이 90 + 2)이 맵과 안 겹치는지 — 앉은 칸의 낮은 중심에서 재도 같게
				const Vec3 top = from + Vec3(std::cos(angle) * distance, std::sin(angle) * distance, 200.0);
				RayHit floor;
				if (!map.Raycast(top, top + Vec3(0, 0, -600), MAP_BLOCK_MOVE, floor) || floor.startInside) continue;
				const Vec3 at = top + Vec3(0, 0, 92.0 - floor.distance);
				if (map.OverlapCapsule(at, 35, 90, MAP_BLOCK_MOVE)) continue;
				out = at;
				return true;
			}
		}
		return false;
	}

	void SendYaw(ShotClient& client, float yaw, uint8_t buttons, EWeapon weapon, float moveX = 0.f)
	{
		wire::Input input = wire::Input();
		input.seq = ++client.seq;
		input.moveX = moveX;		// 걸으며 칸 — 기록 도구가 넣은 +X 누름(월드 축)
		input.yaw = yaw;
		input.buttons = buttons;
		input.weapon = (uint8_t)weapon;
		client.net.SendPacket(input, wire::Msg::C2S_INPUT);
	}

	bool RunMelee(int port, const CMapCollision& map, const std::string& cprobeLog, const std::string& serverLog)
	{
		std::string csvPath = cprobeLog;
		if (csvPath.size() > 4 && csvPath.compare(csvPath.size() - 4, 4, ".log") == 0) csvPath.replace(csvPath.size() - 4, 4, ".csv");
		std::vector<MeleeCell> cells;
		if (!ReadMeleeCells(cprobeLog, csvPath, cells))
		{
			std::printf("MELEEPROBE Lyra 기록을 읽지 못함 %s\n", cprobeLog.c_str());
			return false;
		}
		std::unique_ptr<ShotClient[]> clients(new ShotClient[3]);
		for (int i = 0; i < 3; ++i)
		{
			if (!ConnectAndHello(clients[i].net, port, clients[i].welcome))
			{
				std::printf("MELEEPROBE 접속 실패\n");
				return false;
			}
		}
		int foeIndex = -1, allyIndex = -1;
		for (int i = 1; i < 3; ++i)
		{
			if (clients[i].welcome.team != clients[0].welcome.team) { if (foeIndex < 0) foeIndex = i; }
			else if (allyIndex < 0) allyIndex = i;
		}
		if (foeIndex < 0 || allyIndex < 0)
		{
			std::printf("MELEEPROBE 세 사람 팀이 다른 팀 하나 · 같은 팀 하나로 안 갈림\n");
			return false;
		}
		timeBeginPeriod(1);
		PumpFor(clients.get(), 3, 0.3);
		ShotClient& attacker = clients[0];
		const MoveParams moveParams = MakeLyraMoveParams();		// CMoveSim 은 설정을 참조로 든다 — 임시값을 넘기면 사라진 메모리를 읽는다
		const CMoveSim sim(map, moveParams);
		EWeapon heldWeapon = EWeapon::Pistol;		// 접속 때 든 무기 — 처음 칸이 다른 무기면 더 선다
		int crouchMismatch = 0;
		std::vector<uint32_t> seqs(cells.size(), 0);
		std::vector<Vec3> befores(cells.size()), afters(cells.size());
		std::vector<bool> beforeFound(cells.size(), false), beforeCrouched(cells.size(), false);
		std::vector<uint32_t> beforeSeqs(cells.size(), 0);		// 근접 앞 상태가 처리를 마친 입력 번호
		for (size_t c = 0; c < cells.size(); ++c)
		{
			const MeleeCell& cell = cells[c];
			const int targetIndex = cell.sameTeam ? allyIndex : foeIndex;
			const int parkIndex = cell.sameTeam ? foeIndex : allyIndex;
			const float attackerYaw = (float)(std::atan2(cell.fwd.y, cell.fwd.x) * 180.0 / kPi);
			Vec3 park;
			if (!ParkSpot(map, cell.start, park))
			{
				std::printf("MELEEPROBE step=%s 비켜 설 자리를 못 찾음\n", cell.step.c_str());
				return false;
			}
			const size_t historyFrom = attacker.history.size();
			// 놓으면 선 자세(서버 ApplyPlace 가 MoveState 를 새로 만든다) — 앉는 칸은 선 높이에 놓고 앉기를 한 번 누른다(누를 때마다 바뀌는 단추)
			// 걸으며 · 뛰며 칸은 칸 첫 줄 자리에 놓고 아래에서 따라간다(17-d)
			attacker.Place(cell.moving ? cell.runStart : cell.crouched ? cell.start + Vec3(0, 0, sim.CrouchMeshLiftZ()) : cell.start, 100.f);
			clients[targetIndex].Place(cell.foe, 1000.f);		// 근접 피해 100 으로 안 죽게 — 칸마다 다시 놓아 체력도 돌린다
			clients[parkIndex].Place(park, 100.f);
			const auto sendAll = [&](uint8_t attackerButtons, float attackerMoveX)
			{
				SendYaw(attacker, attackerYaw, attackerButtons, cell.weapon, attackerMoveX);
				SendYaw(clients[targetIndex], cell.foeYaw, 0, EWeapon::Pistol);
				SendYaw(clients[parkIndex], 0.f, 0, EWeapon::Pistol);
			};
			const int settle = kMeleeSettleTicks + (cell.weapon != heldWeapon || cell.crouched ? kMeleeChangeTicks : 0);
			heldWeapon = cell.weapon;
			RunTicks(clients.get(), 3, settle, [&](int tick) { sendAll(tick == 0 && cell.crouched ? (uint8_t)wire::BUTTON_CROUCH : (uint8_t)0, 0.f); }, [] { return false; });
			if (cell.moving)
			{
				// 따라가기 입력을 틱마다 하나씩, 그다음 틱에 근접
				RunTicks(clients.get(), 3, (int)cell.runWalk.size() + 1, [&](int i)
				{
					if ((size_t)i < cell.runWalk.size()) sendAll(cell.runJump[(size_t)i] ? (uint8_t)wire::BUTTON_JUMP : (uint8_t)0, cell.runWalk[(size_t)i] ? 1.f : 0.f);
					else sendAll(wire::BUTTON_MELEE, cell.meleeWalk ? 1.f : 0.f);
				}, [] { return false; });
			}
			else sendAll(wire::BUTTON_MELEE, 0.f);
			seqs[c] = attacker.seq;
			RunTicks(clients.get(), 3, kMeleeSlideTicks, [&](int i) { sendAll(0, (size_t)i < cell.slideWalk.size() && cell.slideWalk[(size_t)i] ? 1.f : 0.f); }, [] { return false; });
			PumpFor(clients.get(), 3, 0.1);
			for (size_t h = historyFrom; h < attacker.history.size(); ++h)
			{
				const wire::PlayerState& s = attacker.history[h];
				if (s.lastSeq < seqs[c])
				{
					befores[c] = Vec3(s.x, s.y, s.z);
					beforeFound[c] = true;
					beforeSeqs[c] = s.lastSeq;
					beforeCrouched[c] = (s.flags & wire::STATE_CROUCHED) != 0;
				}
				afters[c] = Vec3(s.x, s.y, s.z);
			}
		}
		timeEndPeriod(1);
		for (int i = 0; i < 3; ++i) clients[i].net.Close();

		// 서버 시험 줄 — 공격자 입력 번호 → 손 · 맞은 거리
		std::string serverText;
		for (int i = 0; i < 100 && serverText.empty(); ++i)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			std::ifstream file(serverLog, std::ios::binary);
			serverText.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			if (serverText.find("SERVER kills=") == std::string::npos) serverText.clear();
		}
		std::map<uint32_t, std::string> events;
		size_t at = 0;
		while (at < serverText.size())
		{
			size_t end = serverText.find('\n', at);
			if (end == std::string::npos) end = serverText.size();
			std::string line = serverText.substr(at, end - at);
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.rfind("MELEEEV ", 0) == 0 && (uint32_t)std::strtoul(FieldValue(line, "attacker").c_str(), nullptr, 10) == attacker.welcome.playerId)
			{
				events[(uint32_t)std::strtoul(FieldValue(line, "seq").c_str(), nullptr, 10)] = line;
			}
			at = end + 1;
		}

		int kindDiff = 0, targetDiff = 0, damageDiff = 0, missing = 0, slideWithoutHit = 0, contactCells = 0, slidesCompared = 0, eventsMissing = 0;
		double slideDiffMax = 0, distDiffMax = 0, handDiffMax = 0;
		constexpr double kContactGap = 2 * 35.0 + 1.0;		// MeleeCheck kContactSlack 과 같은 어림(닿은 넷 70.1, 안 닿은 것 중 가장 가까운 것 90.9 — e1g3)
		int movingCells = 0, runMeasured = 0, runFails = 0;
		constexpr double kRunTolerance = 1.0;		// 따라간 근접 직전 자리 허용 — 같은 이동 코드라 0 이어야 한다(d3c 잰 칸 0.000). 한 이동(걷기 6cm)이 모자라면 걸린다
		double runDiffMax = 0;		// 따라간 칸 — 서버 근접 직전 자리 ↔ Lyra 근접 직전 자리
		for (size_t c = 0; c < cells.size(); ++c)
		{
			const MeleeCell& cell = cells[c];
			const int targetIndex = cell.sameTeam ? allyIndex : foeIndex;
			const wire::Melee* got = nullptr;
			for (const wire::Melee& m : attacker.melees)
			{
				if (m.attacker == attacker.welcome.playerId && m.seq == seqs[c]) got = &m;
			}
			// Lyra 결과 = 피해 줄이 맞음. 아니면 기록 도구 쓸기로 — 안 닿음 · 같은 팀 · 벽(wall=1). 남이 닿고 벽도 없는데 피해가 없으면 빗나감으로 두어 서버 맞음과 갈리게
			const uint8_t expect = cell.lyraHit ? wire::MELEE_HIT : !cell.traceHit ? wire::MELEE_NONE : cell.sameTeam ? wire::MELEE_SAME_TEAM : cell.traceWall ? wire::MELEE_BLOCKED : wire::MELEE_NONE;
			if (beforeFound[c] && beforeCrouched[c] != cell.crouched) ++crouchMismatch;
			const bool contact = cell.contactGap < kContactGap;
			const double slide = beforeFound[c] ? std::sqrt((afters[c].x - befores[c].x) * (afters[c].x - befores[c].x) + (afters[c].y - befores[c].y) * (afters[c].y - befores[c].y)) : -1.0;
			if (!got || !beforeFound[c])
			{
				++missing;
				std::printf("MELEEPROBE step=%s 서버 근접 소식 %s · 근접 앞 상태 %s\n", cell.step.c_str(), got ? "받음" : "없음", beforeFound[c] ? "있음" : "없음");
				continue;
			}
			const bool kindOk = got->result == expect;
			const uint32_t expectTarget = expect == wire::MELEE_NONE ? 0u : clients[targetIndex].welcome.playerId;
			const bool targetOk = got->target == expectTarget || expect == wire::MELEE_BLOCKED;
			const bool damageOk = got->damage == (expect == wire::MELEE_HIT ? cell.lyraDamage : 0.f);
			if (!kindOk) ++kindDiff;
			if (!targetOk) ++targetDiff;
			if (!damageOk) ++damageDiff;
			double slideDiff = -1, distDiff = -1, handDiff = -1;
			if (got->result != wire::MELEE_HIT)
			{
				if (slide > 0.01 && !cell.walking) ++slideWithoutHit;		// 걸으며 칸은 걸음이 섞여 안 본다
			}
			else if (contact) ++contactCells;
			else
			{
				slideDiff = std::abs(slide - cell.lyraLunge);
				slideDiffMax = std::max(slideDiffMax, slideDiff);
				++slidesCompared;
			}
			if (got->result == wire::MELEE_HIT && cell.traceHit)
			{
				distDiff = std::abs((double)got->distance - cell.traceDist);
				distDiffMax = std::max(distDiffMax, distDiff);
			}
			const auto ev = events.find(seqs[c]);
			Vec3 serverHand;
			int fromPose = -1;
			if (ev != events.end() && FieldVec(ev->second, "hand", serverHand))
			{
				handDiff = Length(serverHand - cell.hand);
				handDiffMax = std::max(handDiffMax, handDiff);
				fromPose = std::atoi(FieldValue(ev->second, "fromPose").c_str());
			}
			else ++eventsMissing;
			if (cell.moving)
			{
				++movingCells;
				// 서버가 한 틱에 입력 둘을 처리하면 근접 바로 앞 입력 뒤 상태가 따로 오지 않는다 — 그때는 못 잰다(-1)
				const bool exact = beforeSeqs[c] + 1 == seqs[c];
				const double runDiff = exact ? Length(befores[c] - cell.start) : -1.0;
				if (exact)
				{
					++runMeasured;
					runDiffMax = std::max(runDiffMax, runDiff);
					if (runDiff > kRunTolerance) ++runFails;
				}
				std::printf("MELEEPROBE run step=%s inputs=%zu walk=%d jump=%d meleeWalk=%u runDiff=%.3f\n", cell.step.c_str(), cell.runWalk.size(),
					(int)std::count(cell.runWalk.begin(), cell.runWalk.end(), (uint8_t)1), (int)std::count(cell.runJump.begin(), cell.runJump.end(), (uint8_t)1), (unsigned)cell.meleeWalk, runDiff);
			}
			std::printf("MELEEPROBE step=%s %s weapon=%u crouch=%d/%d lyra hit=%d trace=%d dist=%.3f dmg=%.0f lunge=%.2f contactGap=%.1f | server result=%u target=%s dist=%.3f dmg=%.0f slide=%.2f fromPose=%d | slideDiff=%.2f distDiff=%.3f handDiff=%.2f\n",
				cell.step.c_str(), kindOk && targetOk && damageOk ? "ok" : "DIFF", (unsigned)cell.weapon, cell.crouched ? 1 : 0, beforeCrouched[c] ? 1 : 0, cell.lyraHit ? 1 : 0, cell.traceHit ? 1 : 0, cell.traceDist, cell.lyraDamage, cell.lyraLunge, cell.contactGap,
				(unsigned)got->result, targetOk ? "ok" : "DIFF", got->distance, got->damage, slide, fromPose, slideDiff, distDiff, handDiff);
		}
		std::printf("MELEEPROBE cells=%zu missing=%d kindDiff=%d targetDiff=%d damageDiff=%d slideWithoutHit=%d | slidesCompared=%d contact=%d slideDiffMax=%.2f distDiffMax=%.3f handDiffMax=%.2f eventsMissing=%d crouchMismatch=%d moving=%d runMeasured=%d runFails=%d runDiffMax=%.3f\n",
			cells.size(), missing, kindDiff, targetDiff, damageDiff, slideWithoutHit, slidesCompared, contactCells, slideDiffMax, distDiffMax, handDiffMax, eventsMissing, crouchMismatch, movingCells, runMeasured, runFails, runDiffMax);
		return missing == 0 && kindDiff == 0 && targetDiff == 0 && damageDiff == 0 && slideWithoutHit == 0 && eventsMissing == 0 && crouchMismatch == 0 && slidesCompared > 0 && runFails == 0;
	}
}

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc < 3)
	{
		std::printf("쓰는 법: ServerProbe <map.mocl> <move.csv> [--port=7791] [--rows=N] [--breakMirror] [--breakSpread] [--breakDamage]\n");
		return 2;
	}
	int port = 7791;
	size_t rowLimit = (size_t)-1;
	bool breakMirror = false, breakSpread = false, breakDamage = false, rulesOnly = false;
	bool authorityOnly = false, rewindOnly = false, padsOnly = false, weaponsOnly = false, grenadeLeaveOnly = false;
	std::string meleeLog;
	std::string serverLog;
	std::string startsDump;
	for (int i = 3; i < argc; ++i)
	{
		const std::string s = argv[i];
		if (s.rfind("--port=", 0) == 0) port = std::atoi(s.c_str() + 7);
		else if (s.rfind("--rows=", 0) == 0) rowLimit = (size_t)std::strtoull(s.c_str() + 7, nullptr, 10);
		else if (s == "--breakMirror") breakMirror = true;
		else if (s == "--breakSpread") breakSpread = true;
		else if (s == "--breakDamage") breakDamage = true;
		else if (s == "--rules") rulesOnly = true;
		else if (s == "--authority") authorityOnly = true;
		else if (s == "--rewind") rewindOnly = true;
		else if (s == "--pads") padsOnly = true;
		else if (s == "--weapons") weaponsOnly = true;
		else if (s == "--grenadeLeave") grenadeLeaveOnly = true;
		else if (s.rfind("--melee=", 0) == 0) meleeLog = s.substr(8);
		else if (s.rfind("--serverLog=", 0) == 0) serverLog = s.substr(12);
		else if (s.rfind("--checkStarts=", 0) == 0) startsDump = s.substr(14);
		else
		{
			std::printf("모르는 인자 %s\n", s.c_str());
			return 2;
		}
	}

	CMapCollision map;
	if (!LoadMap(argv[1], map))
	{
		std::printf("맵을 읽지 못함\n");
		return 2;
	}
	std::vector<Row> rows;
	std::map<std::string, double> values;
	if (!ReadMoveCsv(argv[2], rows, values, rowLimit))
	{
		std::printf("기록을 읽지 못함\n");
		return 2;
	}

	const MoveParams params = MakeLyraMoveParams();
	if (!startsDump.empty())
	{
		const bool startsOk = CheckStarts(map, params, startsDump.c_str());
		std::printf("RESULT %s (시작 자리)\n", startsOk ? "PASS" : "FAIL");
		return startsOk ? 0 : 1;
	}
	if (authorityOnly)
	{
		const bool authorityOk = RunAuthority(port, map, serverLog);
		std::printf("RESULT %s (권위 확인)\n", authorityOk ? "PASS" : "FAIL");
		return authorityOk ? 0 : 1;
	}
	if (rewindOnly)
	{
		const bool rewindOk = RunRewind(port, map, rows, serverLog);
		std::printf("RESULT %s (되감기)\n", rewindOk ? "PASS" : "FAIL");
		return rewindOk ? 0 : 1;
	}
	if (!meleeLog.empty())
	{
		const bool meleeOk = RunMelee(port, map, meleeLog, serverLog);
		std::printf("RESULT %s (근접 — 결과 · 대상 · 피해 · 안 맞은 칸 안 밀림 · 따라간 자리. 밀린 거리 · 손 차이는 잰 값)\n", meleeOk ? "PASS" : "FAIL");
		return meleeOk ? 0 : 1;
	}
	if (padsOnly)
	{
		const bool padsOk = RunPads(port, serverLog);
		std::printf("RESULT %s (받침 구동 — 대조는 check_pads.py)\n", padsOk ? "PASS" : "FAIL");
		return padsOk ? 0 : 1;
	}
	if (weaponsOnly)
	{
		const bool weaponsOk = RunWeapons(port, serverLog);
		std::printf("RESULT %s (무기 받침 구동 — 대조는 check_weapon_pads.py)\n", weaponsOk ? "PASS" : "FAIL");
		return weaponsOk ? 0 : 1;
	}
	if (grenadeLeaveOnly)
	{
		const bool leaveOk = RunGrenadeLeave(port, map, rows);
		std::printf("RESULT %s (던진 사람 나감)\n", leaveOk ? "PASS" : "FAIL");
		return leaveOk ? 0 : 1;
	}
	if (rulesOnly)
	{
		const bool rulesOk = RunRules(port, map, rows);
		std::printf("RESULT %s (경기 규칙)\n", rulesOk ? "PASS" : "FAIL");
		return rulesOk ? 0 : 1;
	}
	bool ok = CheckParams(params, values);
	const bool replayOk = RunReplay(port, map, params, rows, breakMirror, breakSpread);
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	const bool shotsOk = RunShots(port, map, rows, breakDamage);
	ok = ok && replayOk && shotsOk;
	std::printf("RESULT %s (표 · 재생 %s · 사격 %s)\n", ok ? "PASS" : "FAIL", replayOk ? "통과" : "실패", shotsOk ? "통과" : "실패");
	return ok ? 0 : 1;
}
