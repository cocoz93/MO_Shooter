// 대시 대조 도구 — Lyra 대시 기록과 Shared 이동(대시 힘)을 맞춰 본다(대시 D-1).
//
//   DashCheck <cprobe_<tag>.log> <cprobe_<tag>.csv> <map_gym.mocl>
//
// 기록 = Tools/LyraPilot/record_content_probe.ps1 대시 판(D-0). 기록 표 한 줄 = Lyra 한 프레임의 이동 뒤 상태. 기록 도구의 틱은 이동 뒤에 돈다 —
// 그 틱에서 넣은 이동 입력 · 누른 키 · 앉기 키는 다음 프레임에 먹는다. 능력이 켜진 줄(ev 의 dash_start · melee_start)은 그 프레임 이동 앞에 켜졌다.
// 칸(step)마다 첫 능력이 켜진 줄의 앞 줄(자리 · 속도 · 이동 모드 · 앉음)에서 시작해 칸 끝까지 프레임마다 Shared 이동을 돌려 자리 · 속도를 견준다
//   이동 입력 = 앞 줄 inX · inY(월드 축) · 걷기 최대 속도 = 그 줄 maxSpeed(앉은 줄은 600 — 이동 코드가 앉으면 300 으로 바꾼다)
//   대시가 켜진 줄 = 그 줄 이동 앞에 힘을 넣는다. 방향 종류 = 그 줄 몽타주(고르기는 D-2 가 견준다), 입력 · 카메라 = 앞 줄 lastInX · lastInY · ctlPitch · ctlYaw
//     (켜지는 순간의 GetLastMovementInputVector = 앞 프레임 이동이 가져간 입력 = 앞 줄 lastIn). 앞 줄이 앉았으면 일어서기(그래프 bIsCrouched → UnCrouch),
//     근접 밀기가 남았으면 지운다(대시가 근접을 끊음 → 작업 OnDestroy → RemoveRootMotionSourceByID)
//   근접이 켜진 줄 = 밀기 힘을 넣는다. 크기 · 방향 = 그 줄 Lyra 속도 − 힘 없이 같은 입력으로 돌린 속도(이 기록은 맞은 거리를 안 남긴다 — 그 한 프레임은 맞춰 넣은 값이다)
//   앉기 키(crouch_toggle)가 있는 줄 = 다음 줄 이동 앞에 ToggleCrouch
//   새 점프(jumpActive 0 → 1)가 켜진 줄부터는 안 견준다(재생은 점프 키를 안 누른다 — 기대 0번)
//   매 프레임 누름 칸(spam)에서 대시가 꺼진 줄 = 기록 도구가 집으로 옮긴 뒤 값 — 견주지 않고 그 줄에서 다시 시작한다(kHomeAfterDashStep)
// 대조군 = 같은 재생을 힘 없이(일어서기 · 앉기 키는 그대로) — 거의 모든 칸이 어긋나야 이 대조가 대시를 본다
// 능력 대조(D-2) = 기록 전체를 한 줄기로(쿨다운이 칸을 넘는다) 입력마다 CDashAbility 를 서버 차례(NextInput → TryActivate → RememberInput)로 돌려 Lyra 사건과 견준다
//   입력 r = 기록 r−1 줄 틱에서 넣은 것(누름 = dash_press · 이동 = inX · inY · 요 · 피치 = ctlYaw · ctlPitch), r 프레임이 먹는다
//   견주는 것 = 켠 입력(r 줄 dash_start) · 고른 방향(r 줄 몽타주) · 막힘(r 줄 dash_start 이거나 r−1 줄 dashActive) · 쿨다운(r−1 줄 dashCooldown > 0) — 입력마다 전부
//   앞 입력 사본 = 서버가 쓰는 앞 입력(r−2 줄 inX · inY · ctlYaw)이 Lyra 가 켤 때 본 값(r−1 줄 lastInX · lastInY · 몸 yaw)과 같은지 누름마다 센다(다르면 이 대조의 전제가 틀림)
// 막기 대조(D-2-b) = 같은 한 줄기에 무기 순환(권총) · 조준 · 점프 · 근접을 서버 차례로 얹어 줄마다 사격 · 재장전 · 조준 · 점프 · 근접 켜짐과 탄창을 견준다(CompareBlocking)
// 예측 대조(D-3-a) = 내 화면 예측(CSelfPrediction)의 대시 — 서버 값으로 되돌려 다시 돌리기 · 서버 소식으로 고치기 · 근접 누름 미리 보기(ComparePrediction, 기록 없이 만든 입력)
// 끝 코드: 0 = 전부 통과, 1 = 불일치, 2 = 입력 오류
#include "Dash.h"
#include "MapCollision.h"
#include "MatchRules.h"
#include "Movement.h"
#include "Prediction.h"
#include "Weapon.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace mo;

namespace
{
	// 기록 표가 자리 · 속도를 소수 다섯째 자리로 남긴다. 근접 밀기 대조(MeleeCheck kPushPosTolerance · kPushVelTolerance)와 같은 기준(E-2-1)
	constexpr double kPosTolerance = 0.01;
	constexpr double kVelTolerance = 0.01;
	constexpr float kFrameSeconds = 1.f / 60.f;		// Lyra 기록 프레임(t 칸 0.0167 간격)
	constexpr double kUnrealModeFalling = 3.0;		// EMovementMode::MOVE_Falling
	// 매 프레임 누름 칸 — 기록 도구가 대시가 꺼질 때마다 집으로 옮기고 멈춘다(MOBContentProbeSubsystem.cpp 켜짐 가장자리 `if (S.Act == EAct::DashSpam) GoHome();`
	// = SetActorLocationAndRotation + StopMovementImmediately). 그 줄은 이동 뒤가 아니라 옮긴 뒤 값이라 견주지 않고 그 줄에서 다시 시작한다(속도 0 인지는 센다)
	constexpr const char* kHomeAfterDashStep = "spam";

	bool ReadFile(const char* path, std::vector<char>& out)
	{
		FILE* file = nullptr;
		if (fopen_s(&file, path, "rb") != 0 || !file) return false;
		std::fseek(file, 0, SEEK_END);
		const long size = std::ftell(file);
		std::fseek(file, 0, SEEK_SET);
		out.resize((size_t)size);
		const size_t read = std::fread(out.data(), 1, out.size(), file);
		std::fclose(file);
		return read == out.size();
	}

	double Dist(const Vec3& a, const Vec3& b) { const Vec3 d = a - b; return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z); }

	struct Table
	{
		std::vector<std::string> header;
		std::vector<std::vector<std::string>> rows;
		int Column(const char* name) const
		{
			const auto it = std::find(header.begin(), header.end(), std::string(name));
			return it == header.end() ? -1 : (int)(it - header.begin());
		}
	};

	struct Columns
	{
		int step, ev, px, py, pz, vx, vy, vz, speed, maxSpeed, crouched, mode, montage, jumpActive, ctlPitch, ctlYaw, lastInX, lastInY, inX, inY;
		int yaw, dashActive, dashCooldown;		// 능력 대조(D-2)
		int fireActive, reloadActive, meleeActive, adsActive, mag;		// 막기 대조(D-2-b)
		bool Ok() const
		{
			const int all[] = { step, ev, px, py, pz, vx, vy, vz, speed, maxSpeed, crouched, mode, montage, jumpActive, ctlPitch, ctlYaw, lastInX, lastInY, inX, inY,
				yaw, dashActive, dashCooldown, fireActive, reloadActive, meleeActive, adsActive, mag };
			for (int c : all) if (c < 0) return false;
			return true;
		}
	};

	struct StepRange
	{
		std::string name;
		size_t begin = 0, end = 0;		// 기록 표 줄 [begin, end)
	};

	struct Replay
	{
		int frames = 0;
		int dashes = 0;
		int melees = 0;
		int crouchKeys = 0;
		int cutRow = -1;				// 새 점프로 멈춘 줄(-1 = 칸 끝까지)
		int firstDiffRow = -1;
		int badMontage = 0;
		int homes = 0;					// 기록 도구가 집으로 옮긴 줄(견주지 않음)
		int badHome = 0;				// 옮긴 줄인데 속도가 0 이 아님(전제가 틀림)
		double maxPos = 0, maxVel = 0;
		Vec3 firstForce;
		bool fail = false;
	};

	double Cell(const std::vector<std::string>& cells, int col) { return (int)cells.size() > col ? std::atof(cells[(size_t)col].c_str()) : 0.0; }
	const std::string& Text(const std::vector<std::string>& cells, int col)
	{
		static const std::string empty;
		return (int)cells.size() > col ? cells[(size_t)col] : empty;
	}

	bool HasEvent(const std::string& ev, const char* name)
	{
		const std::string token = std::string(name) + ";";
		return ev.find(token) != std::string::npos;
	}

	bool MontageDir(const std::string& montage, EDashDir& out)
	{
		if (montage == "AM_MM_Dash_Forward") out = EDashDir::Forward;
		else if (montage == "AM_MM_Dash_Backward") out = EDashDir::Backward;
		else if (montage == "AM_MM_Dash_Left") out = EDashDir::Left;
		else if (montage == "AM_MM_Dash_Right") out = EDashDir::Right;
		else return false;
		return true;
	}

	class CReplayer
	{
	public:
		CReplayer(const Table& table, const Columns& c, const CMapCollision& map, const MoveParams& params)
			: _t(table), _c(c), _params(params), _sim(map, params), _scratch(map, params) {}

		const MoveCounters& Counters() const { return _sim.GetCounters(); }

		Replay Run(const StepRange& step, size_t startRow, bool withForce)
		{
			Replay out;
			MoveState s;
			const std::vector<std::string>& before = Row(startRow);
			s.location = Vec3(Cell(before, _c.px), Cell(before, _c.py), Cell(before, _c.pz));
			s.velocity = Vec3(Cell(before, _c.vx), Cell(before, _c.vy), Cell(before, _c.vz));
			s.lastUpdateVelocity = s.velocity;
			s.mode = Cell(before, _c.mode) == kUnrealModeFalling ? EMoveMode::Falling : EMoveMode::Walking;
			s.crouched = s.wantsToCrouch = Cell(before, _c.crouched) != 0.0;
			s.maxSpeed = MaxSpeedOf(before);
			_sim.RefreshFloor(s);
			uint16_t meleeId = 0;
			for (size_t r = startRow + 1; r < step.end; ++r)
			{
				const std::vector<std::string>& prev = Row(r - 1);
				const std::vector<std::string>& now = Row(r);
				if (Cell(now, _c.jumpActive) != 0.0 && Cell(prev, _c.jumpActive) == 0.0)
				{
					out.cutRow = (int)r;
					break;
				}
				const Vec3 input(Cell(prev, _c.inX), Cell(prev, _c.inY), 0.0);
				const std::string& ev = Text(now, _c.ev);
				// 이 프레임 입력 처리 — 앞 줄 틱의 앉기 키, 이 줄에 켜진 능력
				if (HasEvent(Text(prev, _c.ev), "crouch_toggle"))
				{
					_sim.ToggleCrouch(s);
					++out.crouchKeys;
				}
				s.maxSpeed = MaxSpeedOf(now);
				if (HasEvent(ev, "melee_start"))
				{
					++out.melees;
					if (withForce)
					{
						MoveState probe = s;
						_scratch.Tick(probe, input, kFrameSeconds);
						const Vec3 force(Cell(now, _c.vx) - probe.velocity.x, Cell(now, _c.vy) - probe.velocity.y, 0.0);
						meleeId = _sim.ApplyRootForce(s, force, kMeleePushDuration);
					}
				}
				if (HasEvent(ev, "dash_start"))
				{
					++out.dashes;
					if (meleeId != 0)
					{
						_sim.RemoveRootForce(s, meleeId);
						meleeId = 0;
					}
					if (Cell(prev, _c.crouched) != 0.0) _sim.RequestUnCrouch(s);
					EDashDir dir = EDashDir::Forward;
					if (!MontageDir(Text(now, _c.montage), dir)) ++out.badMontage;
					const Vec3 force = DashForce(dir, Vec3(Cell(prev, _c.lastInX), Cell(prev, _c.lastInY), 0.0), (float)Cell(prev, _c.ctlPitch), (float)Cell(prev, _c.ctlYaw));
					if (out.dashes == 1) out.firstForce = force;
					if (withForce) _sim.ApplyRootForce(s, force, kDashForceDuration, DashForceSettings());
				}
				_sim.Tick(s, input, kFrameSeconds);
				const Vec3 lyraPos(Cell(now, _c.px), Cell(now, _c.py), Cell(now, _c.pz));
				const Vec3 lyraVel(Cell(now, _c.vx), Cell(now, _c.vy), Cell(now, _c.vz));
				if (step.name == kHomeAfterDashStep && HasEvent(ev, "dash_end"))
				{
					++out.homes;
					if (Dist(lyraVel, Vec3()) != 0.0) ++out.badHome;
					s.location = lyraPos;
					s.velocity = s.lastUpdateVelocity = Vec3();
					s.mode = Cell(now, _c.mode) == kUnrealModeFalling ? EMoveMode::Falling : EMoveMode::Walking;
					_sim.RefreshFloor(s);
					continue;
				}
				const double posDiff = Dist(s.location, lyraPos);
				const double velDiff = Dist(s.velocity, lyraVel);
				out.maxPos = std::max(out.maxPos, posDiff);
				out.maxVel = std::max(out.maxVel, velDiff);
				if ((posDiff > kPosTolerance || velDiff > kVelTolerance) && out.firstDiffRow < 0) out.firstDiffRow = (int)r;
				++out.frames;
			}
			out.fail = out.frames == 0 || out.firstDiffRow >= 0 || out.badMontage > 0 || out.badHome > 0;
			return out;
		}

	private:
		const std::vector<std::string>& Row(size_t r) const { return _t.rows[r]; }
		// 기록 maxSpeed = GetMaxSpeed — 앉아 걸으면 앉은 속도라, 앉은 줄은 서서 걷는 값을 준다(이동 코드가 앉음을 보고 바꾼다)
		float MaxSpeedOf(const std::vector<std::string>& cells) const
		{
			return Cell(cells, _c.crouched) != 0.0 ? _params.maxWalkSpeed : (float)Cell(cells, _c.maxSpeed);
		}

		const Table& _t;
		const Columns& _c;
		const MoveParams& _params;
		CMoveSim _sim;
		CMoveSim _scratch;		// 근접 밀기 크기 구하기 전용 — 계수가 본 재생에 안 섞이게
	};

	// ── 능력 대조(D-2)
	constexpr double kCopyTolerance = 0.001;		// 앞 입력 사본 — 기록 표가 넷째 자리까지 남긴다

	struct AbilityStep
	{
		std::string name;
		int presses = 0, started = 0, lyraStarted = 0, diffs = 0;
		int firstDiffRow = -1;
		std::string dirs;			// 켠 방향(F · B · L · R) 차례
	};

	struct AbilityReport
	{
		int inputs = 0, presses = 0, lyraStarted = 0;
		int results[7] = {};		// EDashResult 마다(누른 입력만)
		int dirs[4] = {};			// 켠 방향마다
		int startDiff = 0, dirDiff = 0, badMontage = 0, activeDiff = 0, cooldownDiff = 0;
		int moveCopyDiff = 0, yawCopyDiff = 0;
		std::vector<AbilityStep> steps;
		bool Fail() const { return presses == 0 || startDiff + dirDiff + badMontage + activeDiff + cooldownDiff + moveCopyDiff + yawCopyDiff > 0; }
	};

	const char kDirLetter[4] = { 'F', 'B', 'L', 'R' };

	double AngleDiff(double a, double b)
	{
		double d = std::fmod(a - b, 360.0);
		if (d < 0.0) d += 360.0;
		return d > 180.0 ? 360.0 - d : d;
	}

	AbilityReport CompareAbility(const Table& t, const Columns& c)
	{
		AbilityReport out;
		CDashAbility ability;
		for (size_t r = 1; r < t.rows.size(); ++r)
		{
			const std::vector<std::string>& prev = t.rows[r - 1];
			const std::vector<std::string>& now = t.rows[r];
			const std::string& stepName = Text(now, c.step);
			if (out.steps.empty() || out.steps.back().name != stepName)
			{
				AbilityStep s;
				s.name = stepName;
				out.steps.push_back(s);
			}
			AbilityStep& step = out.steps.back();
			const Vec3 move(Cell(prev, c.inX), Cell(prev, c.inY), 0.0);
			const float viewPitch = (float)Cell(prev, c.ctlPitch);
			const float viewYaw = (float)Cell(prev, c.ctlYaw);
			const bool pressed = HasEvent(Text(prev, c.ev), "dash_press");
			int diffs = 0;

			ability.NextInput();
			++out.inputs;
			if ((ability.CooldownLeft() > 0) != (Cell(prev, c.dashCooldown) > 0.0)) { ++out.cooldownDiff; ++diffs; }

			DashStart res;
			if (pressed)
			{
				++out.presses;
				++step.presses;
				if (r >= 2)
				{
					const std::vector<std::string>& prev2 = t.rows[r - 2];
					if (std::fabs(Cell(prev2, c.inX) - Cell(prev, c.lastInX)) > kCopyTolerance || std::fabs(Cell(prev2, c.inY) - Cell(prev, c.lastInY)) > kCopyTolerance) { ++out.moveCopyDiff; ++diffs; }
					if (AngleDiff(Cell(prev2, c.ctlYaw), Cell(prev, c.yaw)) > kCopyTolerance) { ++out.yawCopyDiff; ++diffs; }
				}
				res = ability.TryActivate(viewPitch, viewYaw, false, false);
				++out.results[(int)res.result];
			}
			ability.RememberInput(move, viewYaw);

			const bool mine = res.result == EDashResult::Started;
			const bool lyra = HasEvent(Text(now, c.ev), "dash_start");
			if (lyra) { ++out.lyraStarted; ++step.lyraStarted; }
			if (mine)
			{
				++step.started;
				++out.dirs[(int)res.dir];
				step.dirs += kDirLetter[(int)res.dir];
			}
			if (mine != lyra) { ++out.startDiff; ++diffs; }
			if (mine && lyra)
			{
				EDashDir lyraDir;
				if (!MontageDir(Text(now, c.montage), lyraDir)) { ++out.badMontage; ++diffs; }
				else if (lyraDir != res.dir) { ++out.dirDiff; ++diffs; }
			}
			const bool lyraBlocked = lyra || Cell(prev, c.dashActive) != 0.0;
			if (ability.Active() != lyraBlocked) { ++out.activeDiff; ++diffs; }

			if (diffs > 0)
			{
				++step.diffs;
				if (step.firstDiffRow < 0) step.firstDiffRow = (int)r;
			}
		}
		return out;
	}

	// 로그 한 줄에서 「 key=값」 의 값(다음 빈칸까지)
	std::string Token(const std::string& line, const char* key)
	{
		const size_t at = line.find(key);
		if (at == std::string::npos) return std::string();
		const size_t from = at + std::strlen(key);
		const size_t to = line.find(' ', from);
		return line.substr(from, to == std::string::npos ? std::string::npos : to - from);
	}

	// ── 막기 대조(D-2-b)
	constexpr const char* kAmmoStepPrefix = "ammo_";	// 기록 도구가 탄을 바로 바꾸는 칸 — 견주지 않는다
	constexpr int32_t kCheckSpare = 48;					// 권총 startSpareAmmo — 막히지 않으면 재장전이 켜질 만큼

	struct BlockStep
	{
		std::string name;
		bool compared = false;
		int rows = 0, presses = 0, diffs = 0;
		int firstDiffRow = -1;
		std::string firstDiffWhat;
	};

	struct BlockReport
	{
		int rows = 0, badStart = 0;
		int fireDiff = 0, reloadDiff = 0, magDiff = 0, adsDiff = 0, jumpDiff = 0, meleeDiff = 0;
		// 지나간 갈래 — 0 이면 이 기록이 그 갈래를 안 본 것
		int firesBlocked = 0, reloadsBlocked = 0, meleeBlocked = 0, jumpsBlocked = 0, adsBlocked = 0;
		int fireCancels = 0, reloadCancels = 0, meleeCancels = 0, jumpCancels = 0, adsCancels = 0, shotsOnStart = 0;
		std::vector<BlockStep> steps;
		bool Fail() const { return rows == 0 || badStart + fireDiff + reloadDiff + magDiff + adsDiff + jumpDiff + meleeDiff > 0; }
	};

	// 기록의 누름 사건을 입력으로 되살려 대시 · 무기 순환(권총) · 조준 · 점프 · 근접을 Room::ApplyInput 차례 그대로 Shared 함수로 돌리고,
	// 줄마다 Lyra 능력 켜짐(사격 · 재장전 · 조준 · 점프 · 근접)과 탄창을 견준다. 대시 시계는 기록 전체를 한 줄기로(능력 대조와 같다)
	//   입력 r 의 누름 = r−1 줄 사건(act_press · act_release · melee_press · jump_press · jump_release · dash_press — 기록 도구 틱이 누른 것은 다음 프레임이 먹는다).
	//   예외 = reload_press 는 r 줄 사건이 입력 r 이다 — 칸 준비에서 눌러 그 줄에 이미 켜져 있다(dash6 3864 reload_press · reloadActive 1)
	//   act 누름의 단추 = 로그 칸 줄 act=(기록 도구 DASH_ACTIONS). 쥠(fire_hold · jump_hold · ads_hold)은 칸이 바뀔 때 뗀다(EndDash)
	//   근접 켜짐 줄 = 그 입력 뒤에도 켜져 있나(GetInputsLeft > 1) — 근접은 마지막 입력의 입력 처리 뒤에 끝난다(Weapon.h MeleeAbilityInputs)
	//   견주는 칸 = ammo_ 로 시작하지 않는 칸. 칸 첫 줄에서 앞 줄 탄창으로 다시 맞추고 다른 능력은 끈 채 시작한다 — 앞 줄에 켜진 능력이 있으면 badStart(전제가 틀림)
	BlockReport CompareBlocking(const Table& t, const Columns& c, const std::map<std::string, std::string>& stepActs)
	{
		BlockReport out;
		const WeaponDef& pistol = GetWeaponDef(EWeapon::Pistol);
		CDashAbility dash;
		CWeaponCycle cycle;
		cycle.Init(EWeapon::Pistol);
		AdsAbility ads;
		JumpAbility jump;
		CMeleeAbility melee;
		bool fireHeld = false, adsHeld = false, jumpHeld = false;
		for (size_t r = 1; r < t.rows.size(); ++r)
		{
			const std::vector<std::string>& prev = t.rows[r - 1];
			const std::vector<std::string>& now = t.rows[r];
			const std::string& stepName = Text(now, c.step);
			const bool compare = stepName.compare(0, std::strlen(kAmmoStepPrefix), kAmmoStepPrefix) != 0;
			if (out.steps.empty() || Text(prev, c.step) != stepName)
			{
				BlockStep s;
				s.name = stepName;
				s.compared = compare;
				out.steps.push_back(s);
				fireHeld = adsHeld = jumpHeld = false;		// 쥔 단추는 칸 끝에 뗀다
				if (compare)
				{
					cycle.ResetCycleForCheck();
					cycle.SetAmmoForCheck((int32_t)Cell(prev, c.mag), kCheckSpare);
					ads.Reset();
					ads.granted = true;		// 능력은 판 내내 붙어 있다 — 칸 첫 입력의 누름도 받는다
					jump.Reset();
					melee.Reset();
					if (Cell(prev, c.fireActive) != 0.0 || Cell(prev, c.reloadActive) != 0.0 || Cell(prev, c.meleeActive) != 0.0 ||
						Cell(prev, c.adsActive) != 0.0 || Cell(prev, c.jumpActive) != 0.0) ++out.badStart;
				}
			}
			BlockStep& step = out.steps.back();

			// 이 입력의 단추 — 앞 줄 사건(칸 준비 재장전만 이 줄)
			const std::string& evPrev = Text(prev, c.ev);
			const auto actIt = stepActs.find(Text(prev, c.step));
			const std::string act = actIt == stepActs.end() ? std::string() : actIt->second;
			bool reloadPressed = HasEvent(Text(now, c.ev), "reload_press");
			bool meleePressed = HasEvent(evPrev, "melee_press");
			if (HasEvent(evPrev, "jump_press")) jumpHeld = true;
			if (HasEvent(evPrev, "jump_release")) jumpHeld = false;
			const bool actPress = HasEvent(evPrev, "act_press");
			const bool actRelease = HasEvent(evPrev, "act_release");
			if (actPress || actRelease)
			{
				if (act == "fire_tap" || act == "fire_hold") fireHeld = actPress;
				else if (act == "jump_tap" || act == "jump_hold") jumpHeld = actPress;
				else if (act == "ads_hold") adsHeld = actPress;
				else if (act == "reload") reloadPressed = reloadPressed || actPress;
				else if (act == "melee") meleePressed = meleePressed || actPress;
			}
			if (reloadPressed || meleePressed || actPress || HasEvent(evPrev, "jump_press")) ++step.presses;

			// 서버 차례(Room::ApplyInput) — 대시 켜기 · 끊기 → 점프 → 근접 → 조준 → 무기 순환 → 앞 입력 기억
			const Vec3 move(Cell(prev, c.inX), Cell(prev, c.inY), 0.0);
			const float viewPitch = (float)Cell(prev, c.ctlPitch);
			const float viewYaw = (float)Cell(prev, c.ctlYaw);
			melee.NextInput();
			dash.NextInput();
			bool started = false;
			if (HasEvent(evPrev, "dash_press")) started = dash.TryActivate(viewPitch, viewYaw, false, false).result == EDashResult::Started;
			if (started)
			{
				if (melee.Cut()) ++out.meleeCancels;
				if (jump.Cut()) ++out.jumpCancels;
			}
			if (jump.Apply(jumpHeld, dash.BlocksPresses()) == EJumpStep::Blocked) ++out.jumpsBlocked;
			bool meleeStarted = false;
			if (meleePressed)
			{
				if (dash.BlocksPresses()) ++out.meleeBlocked;
				else meleeStarted = melee.TryActivate(pistol, kFrameSeconds);
			}
			const bool adsWasActive = ads.active;
			const bool adsWasHeld = ads.held;
			ads.Apply(adsHeld, dash.BlocksPresses());
			if (dash.JustStarted() && adsWasActive) ++out.adsCancels;
			else if (adsHeld && !adsWasHeld && dash.BlocksPresses()) ++out.adsBlocked;
			WeaponInput wi;
			wi.fireHeld = fireHeld;
			wi.reloadPressed = reloadPressed;
			wi.meleeActive = melee.IsActive();
			wi.meleeStarted = meleeStarted;
			wi.dashActive = dash.BlocksHeldFire();
			wi.dashStarted = dash.JustStarted();
			if (wi.fireHeld && wi.meleeActive) wi.fireHeld = false;
			if (wi.fireHeld && wi.dashActive)
			{
				++out.firesBlocked;
				wi.fireHeld = false;
			}
			if (wi.reloadPressed && dash.BlocksPresses()) ++out.reloadsBlocked;
			const WeaponEvents ev = cycle.Tick(kFrameSeconds, wi, WeaponMotion());
			if (ev.fired && started) ++out.shotsOnStart;
			if (ev.fireCancelledByDash) ++out.fireCancels;
			if (ev.reloadCancelledByDash) ++out.reloadCancels;
			dash.RememberInput(move, viewYaw);

			if (!compare) continue;
			++out.rows;
			++step.rows;
			std::string what;
			const auto check = [&what](bool differs, int& counter, const char* name)
			{
				if (!differs) return;
				++counter;
				if (!what.empty()) what += ',';
				what += name;
			};
			check(cycle.IsFireActive() != (Cell(now, c.fireActive) != 0.0), out.fireDiff, "fire");
			check(cycle.IsReloading() != (Cell(now, c.reloadActive) != 0.0), out.reloadDiff, "reload");
			check(cycle.GetMagazine() != (int32_t)Cell(now, c.mag), out.magDiff, "mag");
			check(ads.active != (Cell(now, c.adsActive) != 0.0), out.adsDiff, "ads");
			check(jump.active != (Cell(now, c.jumpActive) != 0.0), out.jumpDiff, "jump");
			check((melee.IsActive() && melee.GetInputsLeft() > 1) != (Cell(now, c.meleeActive) != 0.0), out.meleeDiff, "melee");
			if (!what.empty())
			{
				++step.diffs;
				if (step.firstDiffRow < 0)
				{
					step.firstDiffRow = (int)r;
					step.firstDiffWhat = what;
				}
			}
		}
		return out;
	}

	// ── 예측 대조(D-3-a) — CSelfPrediction 의 대시를 와이어 없이 함수 인자로 본다. 「서버」 = 같은 입력을 돌린 예측기 하나(서버도 같은 Shared 이동 · 능력 · 차례)
	constexpr double kPredictTolerance = 0.01;		// MeleeCheck kReplayTolerance 와 같다
	constexpr uint32_t kPredictInputs = 70;			// 대시(10) 뒤 힘 15입력 · 미끄러짐이 끝나고 한참 뒤(60)까지
	constexpr uint32_t kPredictDashSeq = 10;
	constexpr uint32_t kPredictLateDashSeq = 50;	// 두 번째 누름 — 10 에 켰으면 쿨다운(10 + 91 = 101 전)이라 안 켜진다
	constexpr uint32_t kPredictMeleeSeq = 8;		// 근접 — 10 의 대시가 끊는다(밀기 힘 0.05초 = 3입력이 10 에도 남아 있다)

	struct PredictScript
	{
		const char* name = "";
		bool dash = true;			// 10 에 대시 누름
		bool lateDash = true;		// 50 에 대시 누름
		bool dashAtFirst = false;	// 1 에 대시 누름(부활 뒤 첫 입력)
		bool melee = false;			// 8 에 근접 + 밀기
		bool crouch = false;		// 2 에 앉기 키 — 10 의 대시가 일으켜 세운다
		bool jump = false;			// 5~30 점프 쥠(10 의 대시가 끊는다) · 45 에 다시 누름
		bool jumpHeldThrough = false;	// 20~65 점프 쥠 — 대시 중 누름이라 막히고, 대시가 끝난 뒤(42~) 땅에서 쥐고 있어도 안 뛴다(D-0 during_jump_hold).
										// 되돌린 뒤 점프 능력(쥠)을 칸에서 못 이으면 61 에서 새로 눌린 것으로 보고 뛴다 — 점프 쥠 시간이 0 이라 「떼기」는 걸음에 안 보이고 이 갈래만 보인다
		bool ads = false;			// 3~20 조준 쥠(10 의 대시가 끄고, 쥔 채로는 안 돌아온다)
		float pitchAtDash = 0.f;	// 10 의 피치 — 앞 대시가 위로도 민다
		float yawBeforeDash = 0.f;	// 9 의 요 — 10 의 대시 방향을 고르는 몸 요
	};

	wire::Input PredictInput(const PredictScript& s, uint32_t i)
	{
		wire::Input input = wire::Input();
		input.moveX = 1.f;		// 월드 +X 로 걷는다 — 대시 「앞 입력」
		if (s.dash && i == kPredictDashSeq) input.buttons |= wire::BUTTON_DASH;
		if (s.lateDash && i == kPredictLateDashSeq) input.buttons |= wire::BUTTON_DASH;
		if (s.dashAtFirst && i == 1) input.buttons |= wire::BUTTON_DASH;
		if (s.melee && i == kPredictMeleeSeq) input.buttons |= wire::BUTTON_MELEE;
		if (s.crouch && i == 2) input.buttons |= wire::BUTTON_CROUCH;
		if (s.jump && ((i >= 5 && i <= 30) || i == 45)) input.buttons |= wire::BUTTON_JUMP;
		if (s.jumpHeldThrough && i >= 20 && i <= 65) input.buttons |= wire::BUTTON_JUMP;
		if (s.ads && i >= 3 && i <= 20) input.buttons |= wire::BUTTON_ADS;
		if (i == kPredictDashSeq) input.pitch = s.pitchAtDash;
		if (i == kPredictDashSeq - 1) input.yaw = s.yawBeforeDash;
		return input;
	}

	void RunPredict(CSelfPrediction& pred, const PredictScript& s, const Vec3& start, const Vec3& push)
	{
		pred.OnSpawn(start.x, start.y, start.z, 0, 0.f);		// 놓인 요 0 — 1 의 대시는 앞 입력 이동 없음 · 요 0 = 앞
		for (uint32_t i = 1; i <= kPredictInputs; ++i)
		{
			wire::Input input = PredictInput(s, i);
			pred.AddInput(input, s.melee && i == kPredictMeleeSeq ? &push : nullptr);
		}
	}

	const CSelfPrediction::Slot* SlotAt(const CSelfPrediction& pred, uint32_t seq)
	{
		wire::PlayerState probe = wire::PlayerState();
		probe.lastSeq = seq;
		return pred.FindComparable(probe);
	}

	// 그 번호 칸을 서버 상태로 — 와이어처럼 float 로 줄인다. 앉음도 싣는다(Reconcile 이 앉음이 다르면 서버 값으로 덮는다)
	bool StateAt(const CSelfPrediction& pred, uint32_t seq, wire::PlayerState& state)
	{
		const CSelfPrediction::Slot* slot = SlotAt(pred, seq);
		if (!slot) return false;
		state = wire::PlayerState();
		state.lastSeq = seq;
		state.x = (float)slot->move.location.x; state.y = (float)slot->move.location.y; state.z = (float)slot->move.location.z;
		state.vx = (float)slot->move.velocity.x; state.vy = (float)slot->move.velocity.y; state.vz = (float)slot->move.velocity.z;
		state.flags = (uint8_t)((slot->move.mode == EMoveMode::Falling ? wire::STATE_FALLING : 0) | (slot->move.crouched ? wire::STATE_CROUCHED : 0));
		return true;
	}

	struct PredictReport
	{
		int scriptFails = 0;
		int replayCompared = 0, replayFails = 0;
		int fixCompared = 0, fixFails = 0;
		int willCompared = 0, willFails = 0;
		int respawnCompared = 0, respawnFails = 0;
		bool Fail() const { return scriptFails != 0 || replayFails != 0 || fixFails != 0 || willFails != 0 || respawnFails != 0 || replayCompared == 0 || fixCompared == 0 || respawnCompared == 0; }
	};

	PredictReport ComparePrediction(const CMapCollision& map, const MoveParams& params, const Vec3& start)
	{
		PredictReport out;
		MatchRules rules;
		rules.spawnImmunitySeconds = 0.0;		// 부활 이동 막음(2.75 × 0.75초 = 124입력)을 끈다 — 걸어야 대시 「앞 입력」이 생긴다
		CMoveSim sim(map, params);
		// 밀기는 걸음(+X)과 다른 쪽(+Y)으로 — 같은 쪽(+X)일 때는 밀기 번호를 칸에 안 남겨도 끝 자리가 같았다(일부러 틀림 1차). +Y 면 6.5cm 갈린다(2차).
		// 지운 힘의 속도는 덧붙이기 전 속도에 남는다(CleanUpRootForce) — 같은 쪽에서 안 갈린 까닭은 좇지 않았다(서버 · 예측이 같은 걸음 코드라 어긋남은 아니다)
		const Vec3 push = MeleePushForce(Vec3(0.0, 1.0, 0.0), 60.0);
		// 예측기는 힙에 둔다 — 하나가 고리 1024칸이라 크다(MeleeCheck 「예측기는 힙에 둔다」)
		const auto make = [&]() { return std::make_unique<CSelfPrediction>(sim, rules, params.maxWalkSpeed); };

		// ① 다시 돌리기 — 대시 앞(9) · 켠 입력(10) · 힘 중(18) · 힘 끝(25) · 미끄러짐(35) · 한참 뒤(60)에서 서버 값으로 되돌려도 끝 자리가 안 고친 판과 같나.
		//    되돌린 뒤 걸음은 칸에 남긴 점프 능력 · 근접 밀기 번호 · 조준 · 대시 켬(힘 · 끊기)으로 다시 돈다 — 못 이으면 끝 자리가 갈린다
		//    (일부러 틀림으로 본 자리: 밀기 번호 = meleeDash 9, 점프 능력 = jumpHeldThrough 60)
		PredictScript meleeDash;
		meleeDash.name = "meleeDash";
		meleeDash.melee = true;
		meleeDash.crouch = true;
		meleeDash.ads = true;
		PredictScript jumpDash;
		jumpDash.name = "jumpDash";
		jumpDash.jump = true;
		jumpDash.pitchAtDash = 15.f;
		PredictScript jumpHold;
		jumpHold.name = "jumpHeldThrough";
		jumpHold.jumpHeldThrough = true;
		double maxEnd = 0;
		for (const PredictScript* s : { &meleeDash, &jumpDash, &jumpHold })
		{
			const auto straightOwner = make();
			CSelfPrediction& straight = *straightOwner;
			RunPredict(straight, *s, start, push);
			const Vec3 straightEnd = straight.GetMove().location;
			PredictScript none = *s;
			none.dash = none.lateDash = false;
			const auto noneOwner = make();
			RunPredict(*noneOwner, none, start, push);
			const double noDash = Dist(noneOwner->GetMove().location, straightEnd);		// 대조군 — 대시가 끝 자리를 바꿔야 이 대조가 대시를 본다
			const CSelfPrediction::Slot* dashSlot = SlotAt(straight, kPredictDashSeq);
			const CSelfPrediction::Slot* lateSlot = SlotAt(straight, kPredictLateDashSeq);
			const CSelfPrediction::DashCounters& sc = straight.GetDashCounters();
			const bool scriptOk = dashSlot && dashSlot->dashStarted && lateSlot && !lateSlot->dashStarted && sc.presses == 2 && sc.started == 1 && noDash > kPredictTolerance;
			if (!scriptOk) ++out.scriptFails;
			std::printf("DASH predictScript %s script=%s started=%d lateStarted=%d presses=%llu startedCount=%llu dir=%u force=%.3f,%.3f,%.3f cutMelee=%d noDashEndDiff=%.2f\n",
				scriptOk ? "ok" : "DIFF", s->name, dashSlot && dashSlot->dashStarted ? 1 : 0, lateSlot && lateSlot->dashStarted ? 1 : 0, (unsigned long long)sc.presses, (unsigned long long)sc.started,
				dashSlot ? (unsigned)dashSlot->dashDir : 0u, dashSlot ? dashSlot->dashForce.x : 0.0, dashSlot ? dashSlot->dashForce.y : 0.0, dashSlot ? dashSlot->dashForce.z : 0.0,
				dashSlot && dashSlot->dashCutMelee ? 1 : 0, noDash);
			for (const uint32_t ack : { 9u, 10u, 18u, 25u, 35u, 60u })
			{
				const auto predOwner = make();
				CSelfPrediction& pred = *predOwner;
				RunPredict(pred, *s, start, push);
				++out.replayCompared;
				wire::PlayerState server;
				if (!StateAt(pred, ack, server) || !dashSlot)
				{
					++out.replayFails;
					std::printf("DASH predictReplay DIFF script=%s ack=%u 칸 없음\n", s->name, ack);
					continue;
				}
				// 서버가 그 입력을 돌렸으면 소식이 상태보다 먼저 온다. 같은 값이라 고칠 것이 없다 — 안 주면 AckMelee · AckDash 가 지운다
				if (s->melee && ack >= kPredictMeleeSeq) pred.OnMeleeResult(kPredictMeleeSeq, true, push);
				if (ack >= kPredictDashSeq) pred.OnDashResult(kPredictDashSeq, dashSlot->dashDir, dashSlot->dashForce);
				const CSelfPrediction::ReconcileResult rr = pred.Reconcile(server, -1.0);		// 허용 차 음수 = 늘 되돌린다
				const double diff = Dist(pred.GetMove().location, straightEnd);
				maxEnd = std::max(maxEnd, diff);
				const CSelfPrediction::DashCounters& dc = pred.GetDashCounters();
				const CSelfPrediction::MeleeCounters& mc = pred.GetMeleeCounters();
				const uint64_t wantResults = ack >= kPredictDashSeq ? 1 : 0;
				const bool fail = !rr.corrected || diff > kPredictTolerance || dc.results != wantResults || dc.startMismatches != 0 || dc.forceFixes != 0 || dc.unconfirmed != 0
					|| dc.replaysMissed != 0 || dc.resultsNoSlot != 0 || mc.unconfirmed != 0 || mc.pushHitFixes != 0 || mc.pushForceFixes != 0 || mc.rerunsMissed != 0;
				if (fail) ++out.replayFails;
				std::printf("DASH predictReplay %s script=%s ack=%u corrected=%d endDiff=%.6f results=%llu\n", fail ? "DIFF" : "ok", s->name, ack, rr.corrected ? 1 : 0, diff, (unsigned long long)dc.results);
			}
		}
		std::printf("DASH predictReplay compared=%d fails=%d scriptFails=%d maxEndDiff=%.6f inputs=%u dashAt=%u\n", out.replayCompared, out.replayFails, out.scriptFails, maxEnd, kPredictInputs, kPredictDashSeq);

		// ② 서버 소식으로 고치기 — 내 대시와 서버 대시가 다를 때. 소식(OnDashResult)이나 소식 없이 지나감(AckDash)으로 고쳐 다시 돈 끝 자리가
		//    서버 판(ref)의 끝 자리와 같고, ref 칸을 서버 상태로 줘도 되돌리지 않아야 한다. 세는 값은 갈래마다 하나씩만
		const auto fixCase = [&](const char* name, const PredictScript& serverScript, const PredictScript& mine, std::initializer_list<uint32_t> messages, uint32_t ack,
			uint64_t wantStart, uint64_t wantForce, uint64_t wantUnconfirmed, uint64_t wantMeleeInReplay, bool resultCloses)
		{
			const auto refOwner = make();
			const auto predOwner = make();
			CSelfPrediction& ref = *refOwner;
			CSelfPrediction& pred = *predOwner;
			RunPredict(ref, serverScript, start, push);
			RunPredict(pred, mine, start, push);
			const Vec3 refEnd = ref.GetMove().location;
			const double before = Dist(pred.GetMove().location, refEnd);
			if (mine.melee) pred.OnMeleeResult(kPredictMeleeSeq, true, push);		// 근접은 두 판이 같다 — 소식이 대시 소식보다 먼저(입력 차례)
			bool slotsOk = true;
			for (const uint32_t seq : messages)
			{
				const CSelfPrediction::Slot* slot = SlotAt(ref, seq);
				if (!slot || !slot->dashStarted) { slotsOk = false; continue; }
				pred.OnDashResult(seq, slot->dashDir, slot->dashForce);
			}
			const double afterResult = Dist(pred.GetMove().location, refEnd);
			wire::PlayerState state;
			if (!StateAt(ref, ack, state)) slotsOk = false;
			const CSelfPrediction::ReconcileResult rr = pred.Reconcile(state, kPredictTolerance);
			const double after = Dist(pred.GetMove().location, refEnd);
			const CSelfPrediction::DashCounters& dc = pred.GetDashCounters();
			const CSelfPrediction::MeleeCounters& mc = pred.GetMeleeCounters();
			const bool counters = dc.startMismatches == wantStart && dc.forceFixes == wantForce && dc.unconfirmed == wantUnconfirmed && dc.meleeInReplay == wantMeleeInReplay
				&& dc.replaysMissed == 0 && dc.resultsNoSlot == 0 && mc.unconfirmed == 0 && mc.rerunsMissed == 0;
			const bool ok = slotsOk && rr.compared && !rr.corrected && after <= kPredictTolerance && counters && before > kPredictTolerance		// 다르게 시작해야 고친 게 보인다
				&& (!resultCloses || afterResult <= kPredictTolerance);		// 소식만으로 닫히는 갈래는 상태를 맞춰 보기 전에 이미 같다
			++out.fixCompared;
			if (!ok) ++out.fixFails;
			std::printf("DASH predictFix %s %s ack=%u before=%.4f afterResult=%.4f after=%.6f corrected=%d startMismatches=%llu forceFixes=%llu unconfirmed=%llu meleeInReplay=%llu replays=%llu\n",
				ok ? "ok" : "DIFF", name, ack, before, afterResult, after, rr.corrected ? 1 : 0, (unsigned long long)dc.startMismatches, (unsigned long long)dc.forceFixes,
				(unsigned long long)dc.unconfirmed, (unsigned long long)dc.meleeInReplay, (unsigned long long)dc.replays);
		};
		PredictScript noDashAt10 = meleeDash;
		noDashAt10.dash = false;
		PredictScript leftJump = jumpDash;
		leftJump.yawBeforeDash = 90.f;		// 몸 요 90 · 앞 이동 +X → 옆(왼) 대시, 힘 = 앞 이동 쪽 수평. 내 판은 요 0 → 앞 대시(피치 15 로 위로도)
		PredictScript plain;
		plain.name = "plain";
		plain.dash = plain.lateDash = false;
		PredictScript firstDash = plain;
		firstDash.dashAtFirst = true;
		// 서버는 켰는데 나는 안 켬(서버의 앞 입력이 달랐다) — 소식으로 켜고, 내 50 누름은 서버 쿨다운으로 꺼진다. 켠 대시가 근접을 끊어 끊음이 바뀐다(meleeInReplay 1)
		fixCase("serverStart", meleeDash, noDashAt10, { kPredictDashSeq }, 55, 1, 0, 0, 1, true);
		// 둘 다 켰는데 방향 · 힘이 다름
		fixCase("serverForce", leftJump, jumpDash, { kPredictDashSeq }, 30, 0, 1, 0, 0, true);
		// 내가 켰는데 서버는 안 켬 — 소식 없이 30 까지 지나감. 끄면 쿨다운이 풀려 내 50 누름이 켜진다(서버도 50 에 켠다)
		fixCase("unconfirmed", noDashAt10, meleeDash, {}, 30, 0, 0, 1, 1, false);
		// 위와 같은데 서버의 50 소식이 먼저 온다(내 50 은 쿨다운으로 안 켬) → 그 뒤 60 상태로 10 을 끈다
		fixCase("unconfirmedThenLate", noDashAt10, meleeDash, { kPredictLateDashSeq }, 60, 1, 0, 1, 1, false);
		// 부활 뒤 첫 입력 — 앞 칸 = 부활 직후 대시 상태(이동 없음 · 놓인 요)
		fixCase("spawnBase", firstDash, plain, { 1u }, 3, 1, 0, 0, 0, true);
		std::printf("DASH predictFix compared=%d fails=%d\n", out.fixCompared, out.fixFails);

		// ④ 부활 다시 돌리기에서 켜짐이 바뀐 칸 세기(D-3-b) — 서버는 경기 시작에 산 사람도 다시 세워 쿨다운을 지운다(EnterPhase Playing).
		//    예측은 부활 번호 뒤 입력을 지운 대시로 다시 고르는데(OnSpawn) 거기서는 started 를 안 센다 → respawnGained · respawnLost 로 센다(봇 판정 「서버 켬 = 예측 켬」의 한 항).
		//    누름 10 · 50 · 105(+X 걷기 · 요 0 = 앞 대시): 처음엔 10 켬 · 50 쿨다운(다시 켜는 첫 입력 101) · 105 켬. 부활 번호 20 → 50 새로 켬 · 105 는 50 의 쿨다운(141 전)이라 꺼짐.
		//    부활 번호 5 → 10 · 50 · 105 가 처음과 같다(대조군 — 0 · 0)
		{
			constexpr uint32_t kRespawnInputs = 130;
			const auto respawnCase = [&](uint32_t spawnSeq, bool want50, bool want105, uint64_t wantGained, uint64_t wantLost)
			{
				const auto predOwner = make();
				CSelfPrediction& pred = *predOwner;
				pred.OnSpawn(start.x, start.y, start.z, 0, 0.f);
				for (uint32_t i = 1; i <= kRespawnInputs; ++i)
				{
					wire::Input input = wire::Input();
					input.moveX = 1.f;
					if (i == 10 || i == 50 || i == 105) input.buttons = wire::BUTTON_DASH;
					pred.AddInput(input);
				}
				const CSelfPrediction::Slot* s10 = SlotAt(pred, 10);
				const CSelfPrediction::Slot* s50 = SlotAt(pred, 50);
				const CSelfPrediction::Slot* s105 = SlotAt(pred, 105);
				const bool firstOk = s10 && s10->dashStarted && s50 && !s50->dashStarted && s105 && s105->dashStarted && pred.GetDashCounters().started == 2;
				pred.OnSpawn(start.x, start.y, start.z, spawnSeq, 0.f);
				const CSelfPrediction::DashCounters& dc = pred.GetDashCounters();
				s10 = SlotAt(pred, 10);
				s50 = SlotAt(pred, 50);
				s105 = SlotAt(pred, 105);
				const bool slotsOk = s50 && s50->dashStarted == want50 && s105 && s105->dashStarted == want105;
				const bool ok = firstOk && slotsOk && dc.respawnGained == wantGained && dc.respawnLost == wantLost && dc.started == 2 && dc.presses == 3;
				++out.respawnCompared;
				if (!ok) ++out.respawnFails;
				std::printf("DASH predictRespawn %s spawnSeq=%u first=%d started50=%d started105=%d respawnGained=%llu respawnLost=%llu started=%llu presses=%llu\n", ok ? "ok" : "DIFF", spawnSeq,
					firstOk ? 1 : 0, s50 && s50->dashStarted ? 1 : 0, s105 && s105->dashStarted ? 1 : 0, (unsigned long long)dc.respawnGained, (unsigned long long)dc.respawnLost,
					(unsigned long long)dc.started, (unsigned long long)dc.presses);
			};
			respawnCase(20, true, false, 1, 1);
			respawnCase(5, false, true, 0, 0);
			std::printf("DASH predictRespawn compared=%d fails=%d\n", out.respawnCompared, out.respawnFails);
		}

		// ③ 근접 누름 미리 보기(WillMeleeStart) — 화면 근접 판정을 켜질 입력에만 하려고 넣기 전에 본다. 대시가 켜지거나 켜져 있으면 안 켜진다
		{
			const auto predOwner = make();
			CSelfPrediction& pred = *predOwner;
			pred.OnSpawn(start.x, start.y, start.z, 0, 0.f);
			wire::Input both = wire::Input();
			both.moveX = 1.f;
			both.buttons = (uint8_t)(wire::BUTTON_MELEE | wire::BUTTON_DASH);
			wire::Input meleeOnly = wire::Input();
			meleeOnly.moveX = 1.f;
			meleeOnly.buttons = wire::BUTTON_MELEE;
			const bool sameInput = pred.WillMeleeStart(both);		// 같은 입력에 대시가 켜진다 → 막힘
			const bool free = pred.WillMeleeStart(meleeOnly);
			wire::Input dash = wire::Input();
			dash.moveX = 1.f;
			dash.buttons = wire::BUTTON_DASH;
			pred.AddInput(dash);		// 1 에 켬 — 막힘 = 1 … 32(kDashActiveInputs)
			for (uint32_t i = 2; i < (uint32_t)kDashActiveInputs; ++i)
			{
				wire::Input walk = wire::Input();
				walk.moveX = 1.f;
				pred.AddInput(walk);
			}
			const bool lastBlocked = pred.WillMeleeStart(meleeOnly);		// 다음 = 32 — 아직 막힘
			wire::Input press32 = meleeOnly;
			pred.AddInput(press32);
			const CSelfPrediction::Slot* at32 = SlotAt(pred, (uint32_t)kDashActiveInputs);
			const bool afterEnd = pred.WillMeleeStart(meleeOnly);			// 다음 = 33 — 풀림
			wire::Input press33 = meleeOnly;
			pred.AddInput(press33);
			const CSelfPrediction::Slot* at33 = SlotAt(pred, (uint32_t)kDashActiveInputs + 1);
			const bool checks[] = { !sameInput, free, !lastBlocked, at32 && !at32->meleeStarted, afterEnd, at33 && at33->meleeStarted };
			for (const bool c : checks)
			{
				++out.willCompared;
				if (!c) ++out.willFails;
			}
			std::printf("DASH predictWillMelee %s compared=%d fails=%d sameInput=%d free=%d at32=%d started32=%d at33=%d started33=%d\n", out.willFails == 0 ? "ok" : "DIFF",
				out.willCompared, out.willFails, sameInput ? 1 : 0, free ? 1 : 0, lastBlocked ? 1 : 0, at32 && at32->meleeStarted ? 1 : 0, afterEnd ? 1 : 0, at33 && at33->meleeStarted ? 1 : 0);
		}
		return out;
	}
}

int main(int argc, char** argv)
{
	if (argc < 4)
	{
		std::printf("사용: DashCheck <cprobe.log> <cprobe.csv> <map_gym.mocl>\n");
		return 2;
	}

	std::vector<char> mapData;
	CMapCollision map;
	if (!ReadFile(argv[3], mapData) || map.LoadFromMemory(mapData.data(), mapData.size()) != EMapLoad::Ok)
	{
		std::printf("DASHCHECK FAIL 맵을 못 읽음: %s\n", argv[3]);
		return 2;
	}

	// ── 로그 — 칸마다 대시가 켜진 횟수(starts) 합. 기록 표의 dash_start 수와 같아야 대시를 빠짐없이 견준다
	std::ifstream log(argv[1]);
	if (!log)
	{
		std::printf("DASHCHECK FAIL 로그를 못 엶: %s\n", argv[1]);
		return 2;
	}
	int logStarts = 0, logSteps = 0, badLines = 0;
	std::map<std::string, std::string> stepActs;
	std::string line;
	while (std::getline(log, line))
	{
		const size_t at = line.find("MOB_CPROBE_DASH ");
		if (at == std::string::npos) continue;
		const size_t key = line.find(" starts=", at);
		if (key == std::string::npos) { ++badLines; continue; }
		logStarts += std::atoi(line.c_str() + key + 8);
		++logSteps;
		// 칸의 대시 중 누름 종류(act= — 기록 도구 DASH_ACTIONS 이름, 없으면 -) — 막기 대조가 act_press · act_release 를 어느 단추로 볼지
		const std::string stepName = Token(line, " step=");
		const std::string actName = Token(line, " act=");
		if (!stepName.empty() && !actName.empty() && actName != "-") stepActs[stepName] = actName;
	}

	// ── 기록 표
	std::ifstream csv(argv[2]);
	Table table;
	if (!csv || !std::getline(csv, line))
	{
		std::printf("DASHCHECK FAIL 기록 표를 못 엶: %s\n", argv[2]);
		return 2;
	}
	{
		std::istringstream in(line);
		std::string cell;
		while (std::getline(in, cell, ',')) table.header.push_back(cell);
	}
	while (std::getline(csv, line))
	{
		std::vector<std::string> cells;
		std::istringstream in(line);
		std::string cell;
		while (std::getline(in, cell, ',')) cells.push_back(cell);
		table.rows.push_back(cells);
	}
	Columns c;
	c.step = table.Column("step"); c.ev = table.Column("ev");
	c.px = table.Column("px"); c.py = table.Column("py"); c.pz = table.Column("pz");
	c.vx = table.Column("vx"); c.vy = table.Column("vy"); c.vz = table.Column("vz");
	c.speed = table.Column("speed2d"); c.maxSpeed = table.Column("maxSpeed"); c.crouched = table.Column("crouched"); c.mode = table.Column("mode");
	c.montage = table.Column("montage"); c.jumpActive = table.Column("jumpActive");
	c.ctlPitch = table.Column("ctlPitch"); c.ctlYaw = table.Column("ctlYaw");
	c.lastInX = table.Column("lastInX"); c.lastInY = table.Column("lastInY"); c.inX = table.Column("inX"); c.inY = table.Column("inY");
	c.yaw = table.Column("yaw"); c.dashActive = table.Column("dashActive"); c.dashCooldown = table.Column("dashCooldown");
	c.fireActive = table.Column("fireActive"); c.reloadActive = table.Column("reloadActive"); c.meleeActive = table.Column("meleeActive");
	c.adsActive = table.Column("adsActive"); c.mag = table.Column("mag");
	if (!c.Ok())
	{
		std::printf("DASHCHECK FAIL 기록 표 머리에 대시 대조 칸이 없음(대시 판 기록인가 — D-0 record_content_probe.ps1)\n");
		return 2;
	}

	std::vector<StepRange> steps;
	for (size_t r = 0; r < table.rows.size(); ++r)
	{
		const std::string name = (int)table.rows[r].size() > c.step ? table.rows[r][(size_t)c.step] : std::string();
		if (steps.empty() || steps.back().name != name)
		{
			if (!steps.empty()) steps.back().end = r;
			StepRange s;
			s.name = name;
			s.begin = r;
			steps.push_back(s);
		}
	}
	if (!steps.empty()) steps.back().end = table.rows.size();

	int csvStarts = 0;
	for (const auto& cells : table.rows)
	{
		if ((int)cells.size() > c.ev && HasEvent(cells[(size_t)c.ev], "dash_start")) ++csvStarts;
	}
	if (csvStarts == 0 || csvStarts != logStarts || badLines != 0)
	{
		std::printf("DASHCHECK FAIL 입력 — 대시 %d번(로그 %d칸) · %d번(표) · 못 읽은 줄 %d\n", logStarts, logSteps, csvStarts, badLines);
		return 2;
	}

	// ── 대조 — 칸마다 힘 넣은 재생 · 힘 없는 대조군
	const MoveParams params = MakeLyraMoveParams();
	CReplayer replayer(table, c, map, params);
	CReplayer control(table, c, map, params);
	int compared = 0, fails = 0, frames = 0, dashes = 0, melees = 0, crouchKeys = 0, cuts = 0, badMontage = 0, homes = 0, badHome = 0;
	int controlFails = 0;
	double maxPos = 0, maxVel = 0, controlMinPos = 1e30;
	for (const StepRange& step : steps)
	{
		size_t first = step.end;
		for (size_t r = step.begin; r < step.end; ++r)
		{
			const std::string& ev = table.rows[r].size() > (size_t)c.ev ? table.rows[r][(size_t)c.ev] : std::string();
			if (HasEvent(ev, "dash_start") || HasEvent(ev, "melee_start")) { first = r; break; }
		}
		if (first == step.end) continue;		// 대시가 안 켜진 칸(still_yaw90 취소 · 준비 칸)
		if (first == 0 || first - 1 < step.begin)
		{
			std::printf("DASH INPUT step=%s 첫 능력 줄 앞에 같은 칸 줄이 없음\n", step.name.c_str());
			++fails;
			continue;
		}
		const Replay mine = replayer.Run(step, first - 1, true);
		const Replay none = control.Run(step, first - 1, false);
		++compared;
		if (mine.fail) ++fails;
		if (none.fail) ++controlFails;
		frames += mine.frames;
		dashes += mine.dashes;
		melees += mine.melees;
		crouchKeys += mine.crouchKeys;
		badMontage += mine.badMontage;
		homes += mine.homes;
		badHome += mine.badHome;
		if (mine.cutRow >= 0) ++cuts;
		maxPos = std::max(maxPos, mine.maxPos);
		maxVel = std::max(maxVel, mine.maxVel);
		controlMinPos = std::min(controlMinPos, none.maxPos);
		std::printf("DASH %s step=%s dashes=%d melee=%d crouchKeys=%d homes=%d frames=%d force=%.3f,%.3f,%.3f maxPosDiff=%.4f maxVelDiff=%.4f firstDiffFrame=%s cutFrame=%s noForceMaxPosDiff=%.2f\n",
			mine.fail ? "DIFF" : "ok", step.name.c_str(), mine.dashes, mine.melees, mine.crouchKeys, mine.homes, mine.frames, mine.firstForce.x, mine.firstForce.y, mine.firstForce.z, mine.maxPos, mine.maxVel,
			mine.firstDiffRow >= 0 ? table.rows[(size_t)mine.firstDiffRow][0].c_str() : "-", mine.cutRow >= 0 ? table.rows[(size_t)mine.cutRow][0].c_str() : "-", none.maxPos);
	}

	const MoveCounters& mc = replayer.Counters();
	std::printf("DASH move compared=%d fails=%d frames=%d maxPosDiff=%.4f maxVelDiff=%.4f dashes=%d melee=%d crouchKeys=%d cut=%d badMontage=%d homes=%d badHome=%d\n",
		compared, fails, frames, maxPos, maxVel, dashes, melees, crouchKeys, cuts, badMontage, homes, badHome);
	std::printf("DASH noForce compared=%d fails=%d minMaxPosDiff=%.2f\n", compared, controlFails, compared > 0 ? controlMinPos : 0.0);
	std::printf("DASH forceCount applied=%llu second=%llu replaced=%llu prepared=%llu finished=%llu removed=%llu clamped=%llu zAdds=%llu liftoffs=%llu restores=%llu adds=%llu external=%llu apexRefund=%llu\n",
		(unsigned long long)mc.rootForceApplied, (unsigned long long)mc.rootForceSecond, (unsigned long long)mc.rootForceReplaced, (unsigned long long)mc.rootForcePrepared,
		(unsigned long long)mc.rootForceFinished, (unsigned long long)mc.rootForceRemoved, (unsigned long long)mc.rootForceClamped, (unsigned long long)mc.rootForceZAdds,
		(unsigned long long)mc.rootForceLiftoffs, (unsigned long long)mc.rootForceRestores, (unsigned long long)mc.rootForceAdds, (unsigned long long)mc.rootForceExternal,
		(unsigned long long)mc.rootForceApexRefund);

	// ── 능력 대조(D-2)
	const AbilityReport ab = CompareAbility(table, c);
	int abilitySteps = 0, abilityStepFails = 0;
	for (const AbilityStep& s : ab.steps)
	{
		if (s.presses == 0 && s.lyraStarted == 0 && s.diffs == 0) continue;
		++abilitySteps;
		if (s.diffs > 0) ++abilityStepFails;
		std::printf("DASH ability %s step=%s presses=%d started=%d lyraStarted=%d dirs=%s diffInputs=%d firstDiffFrame=%s\n", s.diffs > 0 ? "DIFF" : "ok", s.name.c_str(),
			s.presses, s.started, s.lyraStarted, s.dirs.empty() ? "-" : s.dirs.c_str(), s.diffs, s.firstDiffRow >= 0 ? table.rows[(size_t)s.firstDiffRow][0].c_str() : "-");
	}
	std::printf("DASH ability compared=%d fails=%d inputs=%d presses=%d started=%d lyraStarted=%d noDirection=%d whileActive=%d onCooldown=%d dirF=%d dirB=%d dirL=%d dirR=%d\n",
		abilitySteps, abilityStepFails, ab.inputs, ab.presses, ab.results[(int)EDashResult::Started], ab.lyraStarted, ab.results[(int)EDashResult::NoDirection],
		ab.results[(int)EDashResult::WhileActive], ab.results[(int)EDashResult::OnCooldown], ab.dirs[0], ab.dirs[1], ab.dirs[2], ab.dirs[3]);
	std::printf("DASH abilityDiff start=%d dir=%d badMontage=%d active=%d cooldown=%d moveCopy=%d yawCopy=%d\n",
		ab.startDiff, ab.dirDiff, ab.badMontage, ab.activeDiff, ab.cooldownDiff, ab.moveCopyDiff, ab.yawCopyDiff);

	// ── 막기 대조(D-2-b)
	const BlockReport bl = CompareBlocking(table, c, stepActs);
	int blockSteps = 0, blockStepFails = 0;
	for (const BlockStep& s : bl.steps)
	{
		if (!s.compared) continue;
		++blockSteps;
		if (s.diffs > 0) ++blockStepFails;
		if (s.presses == 0 && s.diffs == 0) continue;		// 누름이 없는 칸은 줄을 안 찍는다(합에는 든다)
		std::printf("DASH block %s step=%s rows=%d presses=%d diffRows=%d firstDiffFrame=%s what=%s\n", s.diffs > 0 ? "DIFF" : "ok", s.name.c_str(), s.rows, s.presses, s.diffs,
			s.firstDiffRow >= 0 ? table.rows[(size_t)s.firstDiffRow][0].c_str() : "-", s.firstDiffWhat.empty() ? "-" : s.firstDiffWhat.c_str());
	}
	std::printf("DASH block compared=%d fails=%d rows=%d badStart=%d diff fire=%d reload=%d mag=%d ads=%d jump=%d melee=%d\n",
		blockSteps, blockStepFails, bl.rows, bl.badStart, bl.fireDiff, bl.reloadDiff, bl.magDiff, bl.adsDiff, bl.jumpDiff, bl.meleeDiff);
	std::printf("DASH blockPaths blocked fire=%d reload=%d melee=%d jump=%d ads=%d cancel fire=%d reload=%d melee=%d jump=%d ads=%d shotsOnStart=%d\n",
		bl.firesBlocked, bl.reloadsBlocked, bl.meleeBlocked, bl.jumpsBlocked, bl.adsBlocked, bl.fireCancels, bl.reloadCancels, bl.meleeCancels, bl.jumpCancels, bl.adsCancels, bl.shotsOnStart);

	// ── 예측 대조(D-3-a) — 시작 자리 = 첫 대시 칸의 켜기 앞 줄(바닥 위 · 기록이 그 자리에서 대시했다)
	Vec3 predictStart;
	for (size_t r = 1; r < table.rows.size(); ++r)
	{
		if ((int)table.rows[r].size() > c.ev && HasEvent(table.rows[r][(size_t)c.ev], "dash_start"))
		{
			predictStart = Vec3(Cell(table.rows[r - 1], c.px), Cell(table.rows[r - 1], c.py), Cell(table.rows[r - 1], c.pz));
			break;
		}
	}
	const PredictReport pr = ComparePrediction(map, params, predictStart);

	const bool pass = fails == 0 && !ab.Fail() && !bl.Fail() && !pr.Fail();
	std::printf("DASHCHECK %s steps=%d dashes=%d fails=%d abilityFails=%d blockFails=%d\n", pass ? "PASS" : "FAIL", compared, dashes, fails, abilityStepFails + (ab.presses == 0 ? 1 : 0),
		blockStepFails + bl.badStart + (bl.rows == 0 ? 1 : 0));
	return pass ? 0 : 1;
}
