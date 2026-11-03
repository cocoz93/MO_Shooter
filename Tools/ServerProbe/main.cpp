// 서버 시험 클라 — 서버를 TCP 로 붙어서 본다. 서버 코드를 부르지 않는다.
//
// 서버는 --debug --warmup=0 으로 띄운다(경기 전 대기 중에는 피해가 없다).
//   ServerProbe <map.mocl> <move.csv> [--port=7791] [--rows=N] [--seed=20261102] [--breakMirror] [--breakSpread] [--breakDamage]
//   ServerProbe <map.mocl> <move.csv> --rules [--port=7791]		서버는 --debug --warmup=3 --targetScore=2
//   ServerProbe <map.mocl> <move.csv> --checkStarts=<weapons_match.txt>		서버 없이
//   ServerProbe <map.mocl> <move.csv> --authority --serverLog=<서버 출력 파일>	서버는 --warmup=0 --exitWhenEmpty, --debug 없이
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
		int checked = 0, missing = 0, diff = 0;
		for (const Item& item : items)
		{
			const auto it = values.find(item.name);
			if (it == values.end())
			{
				++missing;
				std::printf("PARAM 기록에 없음 %s\n", item.name);
				continue;
			}
			++checked;
			if (std::fabs((float)it->second - (float)item.mine) > 0.f)
			{
				++diff;
				std::printf("PARAM 다름 %s lyra=%.8f mine=%.8f\n", item.name, it->second, item.mine);
			}
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

	bool RunReplay(int port, const CMapCollision& map, const MoveParams& baseParams, const std::vector<Row>& rows, uint32_t seed, bool breakMirror, bool breakSpread)
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
		CSpreadRng bulletRng(seed);
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
		std::vector<wire::PlayerState> history;
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
				case wire::Msg::S2C_RESULT:
					std::memcpy(&result, data, std::min(size, (int)sizeof(result)));
					hasResult = true;
					break;
				default:
					break;
				}
			});
		}

		void Place(const Vec3& at, float hp)
		{
			wire::DebugPlace place = wire::DebugPlace();
			place.seq = ++seq;
			place.x = at.x; place.y = at.y; place.z = at.z;
			place.hp = hp;
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

		// ⑥ 떨어져 죽음 — 사망 +1, 점수 그대로
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
			Expect(k.attacker == 0 && k.target == idC && k.teamScoreA == 1 && k.teamScoreB == 0 && k.targetDeaths == 1, fails, "낙사 — 사망만 +1, 점수 그대로");
		}
		else
		{
			Expect(false, fails, "낙사가 안 일어남");
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

		// ⑧ 경기 끝 — 쏘지 못하고 움직일 수는 있다
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
		Expect(shotsAfter == 0, fails, "경기 끝에는 발사 막힘");
		Expect(walked > 100.0, fails, "경기 끝에도 이동은 됨");

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
		Expect(a.result.winningTeam == wire::kTeamA && killsA == 2 && deathsB == 2 && deathsC == 1 && a.result.count == 3, fails, "결과 = 처치·사망 집계");

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
		const Vec3 forged[] = { Vec3(0, 0, 500), Vec3(300, 0, 0), Vec3(130, 0, 0), Vec3(0, 0, 150) };	// 멀리 · 수평으로 120 넘음(한도 바로 위) · 위로 120 넘음
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

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc < 3)
	{
		std::printf("쓰는 법: ServerProbe <map.mocl> <move.csv> [--port=7791] [--rows=N] [--seed=N] [--breakMirror] [--breakSpread] [--breakDamage]\n");
		return 2;
	}
	int port = 7791;
	size_t rowLimit = (size_t)-1;
	uint32_t seed = 20261102;
	bool breakMirror = false, breakSpread = false, breakDamage = false, rulesOnly = false;
	bool authorityOnly = false;
	std::string serverLog;
	std::string startsDump;
	for (int i = 3; i < argc; ++i)
	{
		const std::string s = argv[i];
		if (s.rfind("--port=", 0) == 0) port = std::atoi(s.c_str() + 7);
		else if (s.rfind("--rows=", 0) == 0) rowLimit = (size_t)std::strtoull(s.c_str() + 7, nullptr, 10);
		else if (s.rfind("--seed=", 0) == 0) seed = (uint32_t)std::strtoul(s.c_str() + 7, nullptr, 10);
		else if (s == "--breakMirror") breakMirror = true;
		else if (s == "--breakSpread") breakSpread = true;
		else if (s == "--breakDamage") breakDamage = true;
		else if (s == "--rules") rulesOnly = true;
		else if (s == "--authority") authorityOnly = true;
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
	if (rulesOnly)
	{
		const bool rulesOk = RunRules(port, map, rows);
		std::printf("RESULT %s (경기 규칙)\n", rulesOk ? "PASS" : "FAIL");
		return rulesOk ? 0 : 1;
	}
	bool ok = CheckParams(params, values);
	const bool replayOk = RunReplay(port, map, params, rows, seed, breakMirror, breakSpread);
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	const bool shotsOk = RunShots(port, map, rows, breakDamage);
	ok = ok && replayOk && shotsOk;
	std::printf("RESULT %s (표 · 재생 %s · 사격 %s)\n", ok ? "PASS" : "FAIL", replayOk ? "통과" : "실패", shotsOk ? "통과" : "실패");
	return ok ? 0 : 1;
}
