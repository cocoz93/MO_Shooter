// 봇 클라 — 한 프로세스에서 봇 N개가 서버에 붙어 한 판을 끝까지 한다. 플랜 2-b.
//
//   BotClient --map=<파일.mocl> [--port=7791] [--bots=10] [--seed=20261102] [--record=파일] [--lead=3] [--breakRecord] [--breakMirror]
//   BotClient --map=<파일.mocl> --seconds=초 [--measureFrom=초 --measureSeconds=초] ... = 부하 시험. 경기 끝을 기다리지 않고 시간이 되면 끊고 LOADBOT 줄을 낸다.
//   봇이 많으면 서버가 방을 나눠 앉힌다 — 기록은 같은 방 봇끼리만 맞춰 본다. 방 2부터는 기록 이름 뒤에 .room<번호>
//   --measureView = 되감기 측정. Lyra 처럼 상태를 쌓아 0.1초 뒤 보간한 자리를 오차 없이 조준하고, 그 시점을 입력에 실어 보낸다. 기본 꺼짐
//
// 판정은 하지 않는다 — 입력을 보내고 받은 것을 적어 맞춰 볼 뿐이다.
// 1. 봇마다 같은 공용 코드(Shared)로 자기 입력을 여기서도 돌려, 돌아온 상태와 위치·속도·탄창까지 비교한다.
//    서버가 시작 자리로 옮기면(S2C_SPAWN) 그 뒤 입력을 새 자리에서 다시 돌린다.
// 2. 받은 처치·결과를 적어 --record 로 남긴다 — 서버 --record 와 바이트로 비교한다.
// 3. 받은 알림으로 규칙을 검사한다: 무적 중 피해 · 대기·경기 뒤 피해 · 죽은 사람의 발사 · 처치 집계 ↔ 결과.
//
// 일부러 틀리기: --breakRecord 는 기록을, --breakMirror 는 여기 쪽 가속을 바꿔 비교가 실패해야 한다.
#include "GameProtocol.h"
#include "KillRecord.h"
#include "MapCollision.h"
#include "MatchRules.h"
#include "Movement.h"
#include "NetClient.h"
#include "Prediction.h"
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
	constexpr float kTick = 1.f / (float)wire::kTickRate;
	constexpr double kPi = 3.14159265358979323846;
	constexpr double kEyeHeight = 60.0;
	constexpr double kFireRange = 4000.0;
	constexpr double kStrafeRange = 1200.0;

	double Length(const Vec3& v)
	{
		return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
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

	struct Args
	{
		std::string mapPath;
		std::string recordPath;
		int port = 7791;
		int bots = 10;
		int lead = 3;
		uint32_t seed = 20261102;
		bool breakRecord = false;
		bool breakMirror = false;
		double seconds = 0;				// 0 이 아니면 부하 시험
		double measureFromSeconds = 0;
		double measureSeconds = 0;
		bool measureView = false;		// 되감기 측정 모드 — 꺼져 있으면 기존 시험과 같은 입력을 보낸다
	};

	// 대조군 — 서버 숫자가 멀쩡한데 이 값이 나쁘면 송신 쪽 문제다. 측정 구간에만 가리킨다
	std::vector<float>* g_stateGapsMs = nullptr;

	double ProcessCpuSeconds()
	{
		FILETIME creation, exit, kernel, user;
		if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return 0;
		const auto seconds = [](const FILETIME& t) { return (double)(((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime) * 1e-7; };
		return seconds(kernel) + seconds(user);
	}

	// 받은 알림에서 나온 규칙 위반 수
	struct RuleChecks
	{
		uint64_t kills = 0;
		uint64_t damages = 0;
		uint64_t shots = 0;
		uint64_t shotsWhileDead = 0;
		uint64_t damageOutsidePlaying = 0;
		uint64_t damageWhileImmune = 0;
		uint64_t spawnsSeen = 0;
		uint64_t immuneBulletHits = 0;		// 몸에 맞았는데 피해 0 — 같은 팀 제외. 무적이 실제로 지나갔다는 분모
	};

	class CBot
	{
	public:
		CBot(const CMapCollision& map, CMoveSim& sim, const MatchRules& rules, uint32_t seed)
			: _map(map), _rules(rules), _rng(seed), _self(sim, rules, MakeLyraMoveParams().maxWalkSpeed)
		{
		}

		bool Connect(int port)
		{
			bool connected = false;
			for (int i = 0; i < 300 && !connected; ++i)
			{
				connected = _net.Connect("127.0.0.1", port);
				if (!connected) std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			if (!connected) return false;
			wire::Hello hello = wire::Hello();
			hello.protocolVersion = wire::kProtocolVersion;
			return _net.SendPacket(hello, wire::Msg::C2S_HELLO);
		}

		// 반환 false = 끊김
		bool Pump()
		{
			if (!_net.IsOpen()) return false;
			const bool open = _net.Poll([this](const wire::Header& header, const char* data, int size) { OnPacket(header, data, size); });
			if (!open) _net.Close();
			return open;
		}

		void SendNextInput(bool breakMirror)
		{
			if (!_hasSpawn || !_net.IsOpen() || _hasResult) return;
			wire::Input input = wire::Input();
			Think(input);
			_self.SetDebugMoveScale(breakMirror ? 0.999 : 1.0);
			_self.AddInput(input);
			if (!_net.SendPacket(input, wire::Msg::C2S_INPUT)) _net.Close();
		}

		void Close() { _net.Close(); }

		void SetMeasureView(bool on) { _measureView = on; }
		uint64_t GetAckLagSum() const { return _ackLagSum; }
		uint64_t GetAckLagSamples() const { return _ackLagSamples; }
		uint32_t GetAckLagMax() const { return _ackLagMax; }

		uint32_t GetId() const { return _welcome.playerId; }
		uint32_t GetRoomId() const { return _welcome.roomId; }
		uint8_t GetTeam() const { return _welcome.team; }
		bool HasResult() const { return _hasResult; }
		bool IsOpen() const { return _net.IsOpen(); }
		const std::string& GetRecord() const { return _record; }
		const wire::Result& GetResult() const { return _result; }
		const RuleChecks& GetChecks() const { return _checks; }
		const std::map<uint32_t, int>& GetKillTally() const { return _killTally; }
		const std::map<uint32_t, int>& GetDeathTally() const { return _deathTally; }
		const std::vector<wire::Match>& GetPhaseChanges() const { return _phaseChanges; }
		uint64_t GetCompared() const { return _compared; }
		uint64_t GetPosDiff() const { return _posDiff; }
		uint64_t GetVelDiff() const { return _velDiff; }
		uint64_t GetAmmoDiff() const { return _ammoDiff; }
		uint64_t GetFlagDiff() const { return _flagDiff; }
		double GetPosMax() const { return _posMax; }
		uint32_t GetFirstBadSeq() const { return _firstBadSeq; }
		uint32_t GetSeq() const { return _self.GetSeq(); }
		uint64_t GetStates() const { return _states; }
		uint64_t GetDetours() const { return _detours; }

	private:
		template <class T>
		static T Copy(const char* data, int size)
		{
			T out = T();
			std::memcpy(&out, data, std::min(size, (int)sizeof(T)));
			return out;
		}

		void OnPacket(const wire::Header& header, const char* data, int size)
		{
			switch ((wire::Msg)header.type)
			{
			case wire::Msg::S2C_WELCOME:
				_welcome = Copy<wire::Welcome>(data, size);
				break;
			case wire::Msg::S2C_STATE:
				OnState(data, size);
				break;
			case wire::Msg::S2C_SHOT:
				OnShot(Copy<wire::Shot>(data, size));
				break;
			case wire::Msg::S2C_DAMAGE:
				OnDamage(Copy<wire::Damage>(data, size));
				break;
			case wire::Msg::S2C_MATCH:
				OnMatch(Copy<wire::Match>(data, size));
				break;
			case wire::Msg::S2C_KILL:
				OnKill(Copy<wire::Kill>(data, size));
				break;
			case wire::Msg::S2C_SPAWN:
				OnSpawn(Copy<wire::Spawn>(data, size));
				break;
			case wire::Msg::S2C_RESULT:
				_result = Copy<wire::Result>(data, size);
				_hasResult = true;
				_record += ResultRecordLines(_result);
				break;
			default:
				break;
			}
		}

		void OnMatch(const wire::Match& match)
		{
			if (match.phase != _phase || _phaseChanges.empty())
			{
				_phaseChanges.push_back(match);
				if (match.phase == (uint8_t)wire::EMatchPhase::Playing)
				{
					// 서버가 처치·사망을 0 으로 돌린다 — 대기 중 떨어져 죽은 것은 결과에 안 들어간다
					_playTick = match.tick;
					_killTally.clear();
					_deathTally.clear();
				}
				else if (match.phase == (uint8_t)wire::EMatchPhase::PostGame)
				{
					_postTick = match.tick;
				}
			}
			_phase = match.phase;
		}

		void OnSpawn(const wire::Spawn& spawn)
		{
			++_checks.spawnsSeen;
			_spawnTick[spawn.player] = spawn.tick;
			_deadSince.erase(spawn.player);
			if (spawn.player != _welcome.playerId) return;

			_hasSpawn = true;
			_self.OnSpawn(spawn.x, spawn.y, spawn.z, spawn.lastSeq);
			_chosenWeapon = (uint8_t)(_rng.NextFloat() * (float)EWeapon::Count);
			if (_chosenWeapon >= (uint8_t)EWeapon::Count) _chosenWeapon = 0;
			_stuckCheckAt = Vec3(spawn.x, spawn.y, spawn.z);
			_stuckTicks = 0;
			_detourLeft = 0;
		}

		void OnKill(const wire::Kill& kill)
		{
			++_checks.kills;
			_record += KillRecordLine(kill);
			if (kill.attacker != 0) ++_killTally[kill.attacker];
			++_deathTally[kill.target];
			_deadSince[kill.target] = kill.tick;
			if (kill.target == _welcome.playerId)
			{
				_self.BeginWaitSpawn();
			}
		}

		void OnDamage(const wire::Damage& damage)
		{
			++_checks.damages;
			// 경기가 끝나는 틱의 앞선 탄은 정상이다 — 그 틱 안에서 승리가 난다
			if (_playTick == 0 || damage.tick < _playTick || (_postTick != 0 && damage.tick > _postTick)) ++_checks.damageOutsidePlaying;
			const auto spawned = _spawnTick.find(damage.target);
			if (spawned != _spawnTick.end() && damage.tick < spawned->second + SecondsToTicks(_rules.spawnImmunitySeconds)) ++_checks.damageWhileImmune;
		}

		void OnShot(const wire::Shot& shot)
		{
			++_checks.shots;
			const auto dead = _deadSince.find(shot.shooter);
			if (dead != _deadSince.end() && shot.tick > dead->second) ++_checks.shotsWhileDead;
			for (int i = 0; i < shot.count; ++i)
			{
				const wire::ShotBullet& bullet = shot.bullets[i];
				if (bullet.hit != (uint8_t)wire::EShotHit::Body || bullet.damage != 0.f) continue;
				const auto shooter = _players.find(shot.shooter);
				const auto target = _players.find(bullet.target);
				if (shooter != _players.end() && target != _players.end() && shooter->second.team != target->second.team) ++_checks.immuneBulletHits;
			}
		}

		void OnState(const char* data, int size)
		{
			if (size < (int)offsetof(wire::State, players)) return;
			++_states;
			const Clock::time_point now = Clock::now();
			if (g_stateGapsMs && _hasStateTime) g_stateGapsMs->push_back(std::chrono::duration<float, std::milli>(now - _stateTime).count());
			_stateTime = now;
			_hasStateTime = true;
			uint32_t stateTick = 0;
			std::memcpy(&stateTick, data + offsetof(wire::State, tick), sizeof(stateTick));
			if (_measureView) UpdateViewClock(stateTick);
			uint8_t count = 0;
			std::memcpy(&count, data + offsetof(wire::State, count), 1);
			for (int i = 0; i < count; ++i)
			{
				wire::PlayerState p;
				std::memcpy(&p, data + offsetof(wire::State, players) + sizeof(wire::PlayerState) * i, sizeof(p));
				_players[p.id] = p;
				if (_measureView) _viewHistory[p.id].Add(p, stateTick);
				if (p.id == _welcome.playerId)
				{
					CompareSelf(p);
					const uint32_t ackLag = _self.GetSeq() - p.lastSeq;
					_ackLagSum += ackLag;
					++_ackLagSamples;
					_ackLagMax = std::max(_ackLagMax, ackLag);
				}
			}
		}

		void CompareSelf(const wire::PlayerState& p)
		{
			const CSelfPrediction::Slot* found = _self.FindComparable(p);
			if (!found) return;
			const CSelfPrediction::Slot& slot = *found;
			// 와이어는 float — 같은 폭으로 맞춘다
			const float slotX = (float)slot.move.location.x, slotY = (float)slot.move.location.y, slotZ = (float)slot.move.location.z;
			const float slotVx = (float)slot.move.velocity.x, slotVy = (float)slot.move.velocity.y, slotVz = (float)slot.move.velocity.z;
			++_compared;
			const double dx = (double)p.x - slotX, dy = (double)p.y - slotY, dz = (double)p.z - slotZ;
			const double dp = std::sqrt(dx * dx + dy * dy + dz * dz);
			const double vx = (double)p.vx - slotVx, vy = (double)p.vy - slotVy, vz = (double)p.vz - slotVz;
			const double dv = std::sqrt(vx * vx + vy * vy + vz * vz);
			_posMax = std::max(_posMax, dp);
			bool bad = false;
			if (dp > 0) { ++_posDiff; bad = true; }
			if (dv > 0) { ++_velDiff; bad = true; }
			// 경기 끝 단계는 서버가 사격 입력을 지운다 — 그 경계를 입력 번호로는 모르므로 탄은 그 전까지만 본다
			if (_phase < (uint8_t)wire::EMatchPhase::PostGame)
			{
				if (p.magazine != slot.magazine || p.spare != slot.spare || p.weapon != slot.weapon) { ++_ammoDiff; bad = true; }
				if (((p.flags & wire::STATE_RELOADING) != 0) != slot.reloading) { ++_flagDiff; bad = true; }
			}
			if (((p.flags & wire::STATE_FALLING) != 0) != (slot.move.mode == EMoveMode::Falling) || ((p.flags & wire::STATE_MOVE_LOCKED) != 0) != (slot.lockInputs > 0)) { ++_flagDiff; bad = true; }
			if (bad && _firstBadSeq == 0) _firstBadSeq = p.lastSeq;
		}

		// Lyra 다리의 틱 시계와 같은 식(MOBridgeSubsystem.cpp HandlePacket) — 받은 상태로 맞추고 30틱 넘게 벌어지면 다시 잡는다
		void UpdateViewClock(uint32_t stateTick)
		{
			const double sample = (double)stateTick - std::chrono::duration<double>(Clock::now().time_since_epoch()).count() * wire::kTickRate;
			if (!_hasViewClock || std::abs(sample - _viewClockOffset) > 30.0)
			{
				_viewClockOffset = sample;
				_hasViewClock = true;
			}
			else
			{
				_viewClockOffset += 0.02 * (sample - _viewClockOffset);
			}
		}

		// Lyra 다리의 상대 보간과 같은 식(MOBridgeSubsystem.cpp TickRemotes) — 측정 봇의 화면에 해당한다
		void BuildShownPlayers(double renderTick)
		{
			_shownPlayers = _players;
			for (const auto& entry : _viewHistory)
			{
				const ViewHistory& history = entry.second;
				const ViewHistory::Snap* from = nullptr;
				const ViewHistory::Snap* to = nullptr;
				const uint32_t available = std::min<uint32_t>(history.count, ViewHistory::kSnaps);
				for (uint32_t back = 0; back < available; ++back)
				{
					const ViewHistory::Snap& s = history.snaps[(history.count - 1 - back) % ViewHistory::kSnaps];
					if ((double)s.tick <= renderTick)
					{
						from = &s;
						break;
					}
					to = &s;
				}
				const ViewHistory::Snap* base = from ? from : to;
				if (!base) continue;
				wire::PlayerState& shown = _shownPlayers[entry.first];
				shown.x = (float)base->at.x;
				shown.y = (float)base->at.y;
				shown.z = (float)base->at.z;
				shown.flags = base->flags;
				if (from && to && Length(to->at - from->at) <= wire::kRemoteTeleportDistance)
				{
					const double span = std::max((double)to->tick - (double)from->tick, 1.0);
					const double alpha = std::min(std::max((renderTick - (double)from->tick) / span, 0.0), 1.0);
					const Vec3 at = from->at + (to->at - from->at) * alpha;
					shown.x = (float)at.x;
					shown.y = (float)at.y;
					shown.z = (float)at.z;
				}
			}
		}

		// 두뇌 — 가장 가까운 산 적을 보고 쏘며 다가간다. 길찾기가 없어 막히면 무작위로 돌아간다
		void Think(wire::Input& input)
		{
			const Vec3 at = _self.GetMove().location;
			const Vec3 eye = at + Vec3(0, 0, kEyeHeight);

			if (_measureView && _hasViewClock)
			{
				const double renderTick = std::chrono::duration<double>(Clock::now().time_since_epoch()).count() * wire::kTickRate + _viewClockOffset - kViewDelayTicks;
				BuildShownPlayers(renderTick);
				const double whole = std::floor(renderTick);
				input.viewTick = renderTick >= 1.0 ? (uint32_t)whole : 0;
				input.viewAlpha = renderTick >= 1.0 ? (float)(renderTick - whole) : 0.f;
			}
			const std::map<uint32_t, wire::PlayerState>& seen = _measureView ? _shownPlayers : _players;

			const wire::PlayerState* enemy = nullptr;
			double nearest = 0;
			for (const auto& entry : seen)
			{
				const wire::PlayerState& p = entry.second;
				if (p.team == _welcome.team || (p.flags & wire::STATE_DEAD)) continue;
				const double d = Length(Vec3(p.x, p.y, p.z) - at);
				if (!enemy || d < nearest)
				{
					enemy = &p;
					nearest = d;
				}
			}

			float yaw = _lastYaw, pitch = 0;
			Vec3 toward(1, 0, 0);
			bool canShoot = false;
			if (enemy)
			{
				const Vec3 target(enemy->x, enemy->y, enemy->z);
				const Vec3 d = target - eye;
				const double error = _measureView ? 0.0 : 2.0;	// 도 — 사람 흉내가 아니라 전부 머리에 맞지 않게. 측정은 본 자리를 그대로 조준해 빗나감을 무기 퍼짐으로만 남긴다
				yaw = (float)(std::atan2(d.y, d.x) * 180.0 / kPi + (_rng.NextFloat() - 0.5f) * error);
				pitch = (float)(std::atan2(d.z, std::sqrt(d.x * d.x + d.y * d.y)) * 180.0 / kPi + (_rng.NextFloat() - 0.5f) * error);
				const double horizontal = std::sqrt(d.x * d.x + d.y * d.y);
				if (horizontal > 1.0) toward = Vec3(d.x / horizontal, d.y / horizontal, 0);

				if (--_sightWait <= 0)
				{
					_sightWait = 4;		// 광선은 네 틱에 한 번이면 충분하다
					RayHit ray;
					_hasSight = nearest < kFireRange && !_map.Raycast(eye, target, MAP_BLOCK_WEAPON, ray);
				}
				canShoot = _hasSight;
			}
			_lastYaw = yaw;

			// 막힘 — 30틱에 30cm 도 못 가면 돌아간다
			if (++_stuckTicks >= 30)
			{
				const Vec3 moved = at - _stuckCheckAt;
				if (!(_self.IsLocked()) && moved.x * moved.x + moved.y * moved.y < 30.0 * 30.0 && _detourLeft <= 0 && !(canShoot && nearest < kStrafeRange))
				{
					const double angle = _rng.NextFloat() * 2.0 * kPi;
					_detourDir = Vec3(std::cos(angle), std::sin(angle), 0);
					_detourLeft = 60;
					++_detours;
				}
				_stuckCheckAt = at;
				_stuckTicks = 0;
			}

			Vec3 move = toward;
			bool jump = false;
			if (_detourLeft > 0)
			{
				--_detourLeft;
				move = _detourDir;
				jump = _detourLeft > 50;
			}
			else if (canShoot && nearest < kStrafeRange)
			{
				if (--_strafeLeft <= 0)
				{
					_strafeLeft = 45 + (int)(_rng.NextFloat() * 60.f);
					_strafeSign = -_strafeSign;
				}
				move = Vec3(-toward.y * _strafeSign, toward.x * _strafeSign, 0);
			}

			input.moveX = (float)move.x;
			input.moveY = (float)move.y;
			input.yaw = yaw;
			input.pitch = pitch;
			input.aimX = (float)eye.x;
			input.aimY = (float)eye.y;
			input.aimZ = (float)eye.z;
			input.weapon = _chosenWeapon;
			if (jump) input.buttons |= wire::BUTTON_JUMP;

			// 탄을 다 쓰면 다른 무기로
			const CWeaponCycle& cycle = _self.GetWeapon();
			if (cycle.GetMagazine() == 0 && cycle.GetSpare() == 0)
			{
				_chosenWeapon = (uint8_t)((_chosenWeapon + 1) % (int)EWeapon::Count);
				input.weapon = _chosenWeapon;
			}
			else if (cycle.GetMagazine() == 0 && !cycle.IsReloading() && !_self.IsWaitingSpawn())
			{
				input.buttons |= wire::BUTTON_RELOAD;
			}
			// 죽어 있어도 방아쇠를 놓지 않는다 — 서버가 거절하는지 보려고. 대기 중에도 쏜다 — 피해 0 인지 보려고
			if (canShoot || _self.IsWaitingSpawn()) input.buttons |= wire::BUTTON_FIRE;
		}

		const CMapCollision& _map;
		const MatchRules& _rules;
		CSpreadRng _rng;
		CNetClient _net;
		wire::Welcome _welcome = wire::Welcome();
		wire::Result _result = wire::Result();
		bool _hasResult = false;
		std::string _record;
		std::map<uint32_t, wire::PlayerState> _players;
		std::map<uint32_t, uint32_t> _spawnTick;
		std::map<uint32_t, uint32_t> _deadSince;
		std::map<uint32_t, int> _killTally;
		std::map<uint32_t, int> _deathTally;
		std::vector<wire::Match> _phaseChanges;
		uint8_t _phase = (uint8_t)wire::EMatchPhase::Waiting;
		uint32_t _playTick = 0;
		uint32_t _postTick = 0;
		RuleChecks _checks;

		CSelfPrediction _self;
		uint8_t _chosenWeapon = 0;
		bool _hasSpawn = false;

		float _lastYaw = 0;
		int _sightWait = 0;
		bool _hasSight = false;
		Vec3 _stuckCheckAt;
		int _stuckTicks = 0;
		int _detourLeft = 0;
		Vec3 _detourDir;
		int _strafeLeft = 0;
		int _strafeSign = 1;
		uint64_t _detours = 0;

		// 측정 모드 — 화면에 해당하는 상태 기록. 6틱 지연 + 왕복 300ms(18틱) + 여유로 32칸
		struct ViewHistory
		{
			static constexpr uint32_t kSnaps = 32;
			struct Snap
			{
				uint32_t tick = 0;
				Vec3 at;
				uint8_t flags = 0;
			};
			std::array<Snap, kSnaps> snaps;
			uint32_t count = 0;
			void Add(const wire::PlayerState& p, uint32_t tick)
			{
				Snap& s = snaps[count % kSnaps];
				s.tick = tick;
				s.at = Vec3(p.x, p.y, p.z);
				s.flags = p.flags;
				++count;
			}
		};
		static constexpr double kViewDelayTicks = 6.0;	// Lyra 다리 kRemoteDelayTicks 와 같은 값(0.1초)
		bool _measureView = false;
		bool _hasViewClock = false;
		double _viewClockOffset = 0;
		std::map<uint32_t, ViewHistory> _viewHistory;
		std::map<uint32_t, wire::PlayerState> _shownPlayers;
		uint64_t _ackLagSum = 0, _ackLagSamples = 0;
		uint32_t _ackLagMax = 0;

		uint64_t _states = 0;
		Clock::time_point _stateTime;
		bool _hasStateTime = false;
		uint64_t _compared = 0, _posDiff = 0, _velDiff = 0, _ammoDiff = 0, _flagDiff = 0;
		double _posMax = 0;
		uint32_t _firstBadSeq = 0;
	};

	bool ParseArgs(int argc, char** argv, Args& out)
	{
		for (int i = 1; i < argc; ++i)
		{
			const std::string s = argv[i];
			const auto value = [&s](const char* key, std::string& result)
			{
				const size_t n = std::strlen(key);
				if (s.compare(0, n, key) != 0) return false;
				result = s.substr(n);
				return true;
			};
			std::string v;
			if (value("--map=", v)) out.mapPath = v;
			else if (value("--record=", v)) out.recordPath = v;
			else if (value("--port=", v)) out.port = std::atoi(v.c_str());
			else if (value("--bots=", v)) out.bots = std::atoi(v.c_str());
			else if (value("--lead=", v)) out.lead = std::atoi(v.c_str());
			else if (value("--seed=", v)) out.seed = (uint32_t)std::strtoul(v.c_str(), nullptr, 10);
			else if (value("--seconds=", v)) out.seconds = std::atof(v.c_str());
			else if (value("--measureFrom=", v)) out.measureFromSeconds = std::atof(v.c_str());
			else if (value("--measureSeconds=", v)) out.measureSeconds = std::atof(v.c_str());
			else if (s == "--measureView") out.measureView = true;
			else if (s == "--breakRecord") out.breakRecord = true;
			else if (s == "--breakMirror") out.breakMirror = true;
			else
			{
				std::printf("모르는 인자 %s\n", s.c_str());
				return false;
			}
		}
		return !out.mapPath.empty();
	}
}

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	Args args;
	if (!ParseArgs(argc, argv, args))
	{
		std::printf("쓰는 법: BotClient --map=<파일.mocl> [--port=7791] [--bots=10] [--seed=N] [--record=파일] [--lead=3] [--breakRecord] [--breakMirror]\n");
		return 2;
	}
	CMapCollision map;
	if (!LoadMap(args.mapPath.c_str(), map))
	{
		std::printf("맵을 읽지 못함\n");
		return 2;
	}
	const MoveParams params = MakeLyraMoveParams();
	CMoveSim sim(map, params);	// 틱 사이에 들고 가는 것이 MoveState 뿐이라 봇끼리 나눠 쓴다
	const MatchRules rules;

	std::vector<std::unique_ptr<CBot>> bots;
	for (int i = 0; i < args.bots; ++i)
	{
		bots.emplace_back(new CBot(map, sim, rules, args.seed + 100 + (uint32_t)i));
		bots.back()->SetMeasureView(args.measureView);
		if (!bots.back()->Connect(args.port))
		{
			std::printf("BOT %d 접속 실패\n", i);
			return 1;
		}
	}

	timeBeginPeriod(1);
	const auto start = Clock::now();
	uint64_t sentTicks = 0;
	uint64_t lastReportSecond = 0;
	bool allDone = false;
	std::vector<float> stateGapsMs;
	std::vector<float> loopGapsMs;
	if (args.measureSeconds > 0)
	{
		const size_t expected = (size_t)(args.measureSeconds * (double)wire::kTickRate * 1.2) * (size_t)args.bots;
		stateGapsMs.reserve(expected);
		loopGapsMs.reserve((size_t)(args.measureSeconds * 1200.0));
	}
	double cpuBegin = 0, cpuEnd = 0;
	bool measureOpened = false, measureClosed = false;
	Clock::time_point lastLoop = start;
	while (!allDone)
	{
		const Clock::time_point loopNow = Clock::now();
		const double elapsed = std::chrono::duration<double>(loopNow - start).count();
		if (args.measureSeconds > 0)
		{
			if (!measureOpened && elapsed >= args.measureFromSeconds)
			{
				measureOpened = true;
				cpuBegin = ProcessCpuSeconds();
				g_stateGapsMs = &stateGapsMs;
			}
			if (measureOpened && !measureClosed && elapsed >= args.measureFromSeconds + args.measureSeconds)
			{
				measureClosed = true;
				cpuEnd = ProcessCpuSeconds();
				g_stateGapsMs = nullptr;
			}
			if (measureOpened && !measureClosed) loopGapsMs.push_back(std::chrono::duration<float, std::milli>(loopNow - lastLoop).count());
		}
		lastLoop = loopNow;
		if (args.seconds > 0 && elapsed >= args.seconds) break;
		const uint64_t target = (uint64_t)(elapsed * wire::kTickRate) + (uint64_t)args.lead;
		while (sentTicks < target)
		{
			for (const std::unique_ptr<CBot>& bot : bots) bot->SendNextInput(args.breakMirror);
			++sentTicks;
		}
		allDone = true;
		for (const std::unique_ptr<CBot>& bot : bots)
		{
			bot->Pump();
			if (bot->IsOpen() && !bot->HasResult()) allDone = false;
		}
		const uint64_t second = (uint64_t)elapsed;
		if (second >= lastReportSecond + 30)
		{
			lastReportSecond = second;
			std::printf("PROGRESS %llus kills=%llu shots=%llu compared=%llu\n", (unsigned long long)second, (unsigned long long)bots[0]->GetChecks().kills, (unsigned long long)bots[0]->GetChecks().shots, (unsigned long long)bots[0]->GetCompared());
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	timeEndPeriod(1);
	size_t botsOpen = 0;
	for (const std::unique_ptr<CBot>& bot : bots)
	{
		if (bot->IsOpen()) ++botsOpen;
		bot->Close();
	}

	// ─── 부하 시험 — 끝까지 안 가므로 결과 대조는 없다. 도중 값이 서버와 같은지만 ──
	if (args.seconds > 0)
	{
		uint64_t compared = 0, posDiff = 0, velDiff = 0, ammoDiff = 0, flagDiff = 0, states = 0;
		for (const std::unique_ptr<CBot>& bot : bots)
		{
			compared += bot->GetCompared();
			posDiff += bot->GetPosDiff();
			velDiff += bot->GetVelDiff();
			ammoDiff += bot->GetAmmoDiff();
			flagDiff += bot->GetFlagDiff();
			states += bot->GetStates();
		}
		uint64_t ackLagSum = 0, ackLagSamples = 0;
		uint32_t ackLagMax = 0;
		for (const std::unique_ptr<CBot>& bot : bots)
		{
			ackLagSum += bot->GetAckLagSum();
			ackLagSamples += bot->GetAckLagSamples();
			ackLagMax = std::max(ackLagMax, bot->GetAckLagMax());
		}
		std::printf("BOTLAG ackLagAvg=%.2f maxAckLag=%u samples=%llu\n", ackLagSamples ? (double)ackLagSum / (double)ackLagSamples : 0.0, ackLagMax, (unsigned long long)ackLagSamples);
		std::sort(stateGapsMs.begin(), stateGapsMs.end());
		std::sort(loopGapsMs.begin(), loopGapsMs.end());
		const auto percentile = [](const std::vector<float>& sorted, double p) { return sorted.empty() ? 0.f : sorted[(size_t)((sorted.size() - 1) * p)]; };
		const bool pass = compared > 0 && posDiff == 0 && velDiff == 0 && ammoDiff == 0 && flagDiff == 0 && botsOpen == bots.size();
		std::printf("LOADBOT bots=%d open=%zu compared=%llu posDiff=%llu velDiff=%llu ammoDiff=%llu flagDiff=%llu states=%llu stateGaps=%zu stateGapP50Ms=%.2f stateGapP99Ms=%.2f stateGapMaxMs=%.2f loopP99Ms=%.2f loopMaxMs=%.2f cpuPercent=%.1f\n",
			args.bots, botsOpen, (unsigned long long)compared, (unsigned long long)posDiff, (unsigned long long)velDiff, (unsigned long long)ammoDiff, (unsigned long long)flagDiff, (unsigned long long)states, stateGapsMs.size(),
			percentile(stateGapsMs, 0.5), percentile(stateGapsMs, 0.99), percentile(stateGapsMs, 1.0), percentile(loopGapsMs, 0.99), percentile(loopGapsMs, 1.0), args.measureSeconds > 0 ? (cpuEnd - cpuBegin) / args.measureSeconds * 100.0 : 0.0);
		std::printf("RESULT %s\n", pass ? "PASS" : "FAIL");
		return pass ? 0 : 1;
	}

	// ─── 맞춰 보기 ────────────────────────────────────────
	bool ok = true;
	const CBot& first = *bots[0];
	std::map<uint32_t, const CBot*> firstOfRoom;
	std::map<uint32_t, int> botsOfRoom;
	for (const std::unique_ptr<CBot>& bot : bots)
	{
		firstOfRoom.emplace(bot->GetRoomId(), bot.get());
		++botsOfRoom[bot->GetRoomId()];
	}
	std::string record = first.GetRecord();
	for (const auto& room : firstOfRoom)
	{
		std::string roomRecord = room.second->GetRecord();
		if (args.breakRecord && !roomRecord.empty()) roomRecord[roomRecord.size() / 2] = roomRecord[roomRecord.size() / 2] == '1' ? '2' : '1';
		if (args.recordPath.empty()) continue;
		FILE* file = std::fopen(RoomRecordPath(args.recordPath, room.first).c_str(), "wb");
		if (file)
		{
			std::fwrite(roomRecord.data(), 1, roomRecord.size(), file);
			std::fclose(file);
		}
	}

	uint64_t compared = 0, posDiff = 0, velDiff = 0, ammoDiff = 0, flagDiff = 0, states = 0, detours = 0;
	uint64_t shotsWhileDead = 0, damageOutside = 0, damageImmune = 0, recordDiff = 0, tallyDiff = 0, noResult = 0, immuneHits = 0;
	double posMax = 0;
	uint32_t firstBad = 0;
	int firstBadBot = -1;
	for (size_t i = 0; i < bots.size(); ++i)
	{
		const CBot& bot = *bots[i];
		compared += bot.GetCompared();
		posDiff += bot.GetPosDiff();
		velDiff += bot.GetVelDiff();
		ammoDiff += bot.GetAmmoDiff();
		flagDiff += bot.GetFlagDiff();
		states += bot.GetStates();
		detours += bot.GetDetours();
		posMax = std::max(posMax, bot.GetPosMax());
		if (firstBad == 0 && bot.GetFirstBadSeq() != 0)
		{
			firstBad = bot.GetFirstBadSeq();
			firstBadBot = (int)i;
		}
		shotsWhileDead += bot.GetChecks().shotsWhileDead;
		damageOutside += bot.GetChecks().damageOutsidePlaying;
		damageImmune += bot.GetChecks().damageWhileImmune;
		immuneHits += bot.GetChecks().immuneBulletHits;
		if (bot.GetRecord() != firstOfRoom[bot.GetRoomId()]->GetRecord()) ++recordDiff;
		if (!bot.HasResult())
		{
			++noResult;
			continue;
		}
		const wire::Result& result = bot.GetResult();
		for (int p = 0; p < result.count; ++p)
		{
			const wire::PlayerResult& player = result.players[p];
			const auto killIt = bot.GetKillTally().find(player.id);
			const auto deathIt = bot.GetDeathTally().find(player.id);
			const int kills = killIt == bot.GetKillTally().end() ? 0 : killIt->second;
			const int deaths = deathIt == bot.GetDeathTally().end() ? 0 : deathIt->second;
			if (kills != player.kills || deaths != player.deaths) ++tallyDiff;
		}
	}

	const wire::Result& result = first.GetResult();
	std::printf("BOT bots=%d inputsSent=%llu statesSeen=%llu detours=%llu\n", args.bots, (unsigned long long)sentTicks * bots.size(), (unsigned long long)states, (unsigned long long)detours);
	std::printf("BOT 단계:");
	for (const wire::Match& match : first.GetPhaseChanges()) std::printf(" [%u 틱=%u 남은초=%.2f 목표=%u]", match.phase, match.tick, match.secondsLeft, match.targetScore);
	std::printf("\n");
	std::printf("BOT 서버 ↔ 같은 코드: compared=%llu posDiff=%llu posMax=%.5f velDiff=%llu ammoDiff=%llu flagDiff=%llu 처음 어긋난 봇=%d 번호=%u\n", (unsigned long long)compared, (unsigned long long)posDiff, posMax, (unsigned long long)velDiff, (unsigned long long)ammoDiff, (unsigned long long)flagDiff, firstBadBot, firstBad);
	std::printf("BOT 규칙: kills=%llu damages=%llu shots=%llu spawns=%llu | 죽은 뒤 발사=%llu 대기·경기 뒤 피해=%llu 무적 중 피해=%llu | 적이 맞았는데 피해 0(무적 분모)=%llu\n",
		(unsigned long long)first.GetChecks().kills, (unsigned long long)first.GetChecks().damages, (unsigned long long)first.GetChecks().shots, (unsigned long long)first.GetChecks().spawnsSeen,
		(unsigned long long)shotsWhileDead, (unsigned long long)damageOutside, (unsigned long long)damageImmune, (unsigned long long)immuneHits);
	std::printf("BOT 기록: 줄 바이트=%zu 봇끼리 다름=%llu 결과 못 받음=%llu 처치 집계 ↔ 결과 다름=%llu\n", record.size(), (unsigned long long)recordDiff, (unsigned long long)noResult, (unsigned long long)tallyDiff);
	std::printf("BOT 결과: 점수 %u:%u 목표 %u 이긴 팀 %u 틱 %u\n", result.teamScoreA, result.teamScoreB, result.targetScore, result.winningTeam, result.tick);
	for (int p = 0; p < result.count; ++p)
	{
		std::printf("SCORE id=%u team=%u kills=%u deaths=%u\n", result.players[p].id, result.players[p].team, result.players[p].kills, result.players[p].deaths);
	}

	for (const auto& room : firstOfRoom)
	{
		const wire::Result& roomResult = room.second->GetResult();
		std::printf("BOT 방 %u: 봇 %d 점수 %u:%u 이긴 팀 %u 기록 %zu바이트\n", room.first, botsOfRoom[room.first], roomResult.teamScoreA, roomResult.teamScoreB, roomResult.winningTeam, room.second->GetRecord().size());
	}

	if (noResult != 0 || recordDiff != 0 || tallyDiff != 0) ok = false;
	if (compared == 0 || posDiff != 0 || velDiff != 0 || ammoDiff != 0 || flagDiff != 0) ok = false;
	if (shotsWhileDead != 0 || damageOutside != 0 || damageImmune != 0) ok = false;
	std::printf("RESULT %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
