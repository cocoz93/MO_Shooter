// 같은 입력 누름 대조 도구 — L-1-c 기록(같은 프레임 두 누름)을 예측(CSelfPrediction)에 넣어 Lyra 결과와 견준다(L-5-b).
//
//   PressCheck <map_gym.mocl> <기록 폴더> <태그>...		(태그 = cprobe_<태그>.log · .csv — L-1-c 수류탄 l1c1 · l1c2 · 대시 l1cd1 · l1cd2)
//
// 기록 = 기록 도구 L-1-c 각본(Client/Lyra/Plugins/MOBridge MOBContentProbeSubsystem — 수류탄 GRENADE_L1C_SCRIPT · 대시 l1c 칸). 칸(step) 이름이 s_ 로 시작하고
// 로그에 칸 결과 줄(DCOMBO · GCOMBO)이 있는 칸마다(탄 준비 칸 s_ammo1 · 2 는 건너뛰고 skipped 로 센다):
//   입력 r = 표 r−1 줄 사건의 누름(기록 도구 틱이 누른 것은 다음 프레임이 먹는다 — DashCheck · GrenadeCheck 와 같다). 한 줄에 누름이 여럿이면 사건 칸의 차례가
//     누른 차례다(기록 도구가 PressTag 를 부른 차례 = Lyra InputPressedSpecHandles 차례, lyra-match-progress L-1-c) → buttons · pressOrder(wire::MakePressOrder)
//     누름 이름 = dash_press(대시) · g_press(수류탄) · act_press(대시 칸 act= 단추, 로그 DCOMBO) · other_press(수류탄 칸 other= 능력, 로그 GCOMBO)
//   이동 입력 = 앞 줄 inX · inY(월드 축 — 대시 기록만 칸이 있고 없으면 0) · 요 · 피치 = 앞 줄 ctlYaw · ctlPitch · 무기 = 권총
//   칸 시작 = 칸 첫 줄 자리에서 OnSpawn(능력 · 쿨다운이 꺼진 새 몸, 부활 막음 0) · 탄창 = GCOMBO magBegin(없으면 권총 가득)
// 견주는 것(칸마다 — 칸 안 입력 전부와 칸 끝):
//   근접 켬(Lyra GABIL on GA_Melee) · 같은 입력에 끊김(on 과 end 가 같은 프레임) · 막힘(fail) / 대시 켬 / 던짐 / 재장전 켬(on 이고 그 프레임에 안 끝남) ·
//   재장전 누름 끊김(on 과 end 가 같은 프레임 — 예측은 reloadPressCutByGrenade) / 칸 끝 탄창(GCOMBO magEnd, 있을 때만)
//   몽타주는 예측에 없어 안 본다(서버 몽타주는 ServerProbe --pressCells, L-5-a)
// 대조군 = 같은 입력을 차례 없이(pressOrder 0 → 판 13 고정 차례 대시 → 수류탄 → 근접) 한 번 더 — Lyra 와 다른 칸이 0 이면 이 대조가 차례를 못 본다(FAIL)
// 끝 코드: 0 = 전부 통과, 1 = 불일치, 2 = 입력 오류
#include "GameProtocol.h"
#include "MapCollision.h"
#include "MatchRules.h"
#include "Movement.h"
#include "Prediction.h"
#include "Weapon.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace mo;

namespace
{
	constexpr int16_t kPistolSpare = 48;		// 권총 startSpareAmmo(DashCheck kCheckSpare 와 같다) — 재장전이 켜질 만큼
	constexpr const char* kComboStepPrefix = "s_";	// L-1-c 칸 이름(기록 도구 각본) — 앞 칸(settle · foe_*)은 준비라 안 본다

	bool ReadFile(const std::string& path, std::vector<char>& out)
	{
		std::ifstream f(path, std::ios::binary);
		if (!f) return false;
		out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		return true;
	}

	std::vector<std::string> Split(const std::string& text, char sep)
	{
		std::vector<std::string> out;
		std::string cell;
		std::istringstream in(text);
		while (std::getline(in, cell, sep)) out.push_back(cell);
		return out;
	}

	bool StartsWith(const std::string& text, const char* prefix)
	{
		return text.compare(0, std::strlen(prefix), prefix) == 0;
	}

	// " key=" 뒤 값(빈칸까지)
	std::string Token(const std::string& line, const char* key)
	{
		const std::string k = std::string(" ") + key + "=";
		const size_t at = line.find(k);
		if (at == std::string::npos) return std::string();
		const size_t from = at + k.size();
		const size_t to = line.find(' ', from);
		return line.substr(from, to == std::string::npos ? std::string::npos : to - from);
	}

	struct Row
	{
		std::string step, ev;
		Vec3 pos;
		float yaw = 0.f, ctlPitch = 0.f, ctlYaw = 0.f, inX = 0.f, inY = 0.f;
	};

	// 능력마다 첫 켬 · 그 뒤 첫 끝 · 첫 못 켬 프레임(없으면 −1)
	struct AbilityTimes
	{
		int on = -1, end = -1, fail = -1;
	};

	struct LyraStep
	{
		AbilityTimes melee, dash, grenade, reload;
		std::string act, other;		// DCOMBO act= · GCOMBO other=
		int magBegin = -1, magEnd = -1;
		bool combo = false;			// 칸 결과 줄(DCOMBO · GCOMBO)이 있다 = 두 누름 칸. 없는 s_ 칸(탄 준비 s_ammo1 · 2)은 안 본다
	};

	AbilityTimes* Pick(LyraStep& s, const std::string& ability)
	{
		if (StartsWith(ability, "GA_Melee")) return &s.melee;
		if (StartsWith(ability, "GA_Hero_Dash")) return &s.dash;
		if (StartsWith(ability, "GA_Grenade")) return &s.grenade;
		if (StartsWith(ability, "GA_Weapon_Reload")) return &s.reload;
		return nullptr;
	}

	struct Outcome
	{
		bool meleeOn = false, meleeCut = false, meleeBlocked = false;
		bool dashOn = false, grenadeOn = false, reloadOn = false, reloadCut = false;
		int mag = -1;		// −1 = 안 견줌

		bool operator==(const Outcome& o) const
		{
			return meleeOn == o.meleeOn && meleeCut == o.meleeCut && meleeBlocked == o.meleeBlocked && dashOn == o.dashOn &&
				grenadeOn == o.grenadeOn && reloadOn == o.reloadOn && reloadCut == o.reloadCut && mag == o.mag;
		}
		std::string Text() const
		{
			char buf[160];
			std::snprintf(buf, sizeof(buf), "melee=%d cut=%d blocked=%d dash=%d grenade=%d reload=%d reloadCut=%d mag=%d",
				meleeOn, meleeCut, meleeBlocked, dashOn, grenadeOn, reloadOn, reloadCut, mag);
			return buf;
		}
	};

	Outcome LyraOutcome(const LyraStep& s)
	{
		Outcome o;
		o.meleeOn = s.melee.on >= 0;
		o.meleeCut = s.melee.on >= 0 && s.melee.end == s.melee.on;
		o.meleeBlocked = s.melee.fail >= 0;
		o.dashOn = s.dash.on >= 0;
		o.grenadeOn = s.grenade.on >= 0;
		o.reloadOn = s.reload.on >= 0 && s.reload.end != s.reload.on;
		o.reloadCut = s.reload.on >= 0 && s.reload.end == s.reload.on;
		o.mag = s.magEnd;
		return o;
	}

	// 사건 칸의 누름 → 누른 차례(EPressKey). 같은 단추를 두 번 적었으면 처음 것만
	int PressKeys(const std::string& ev, const LyraStep& s, uint8_t (&keys)[wire::kPressKeyCount])
	{
		int n = 0;
		const auto add = [&](uint8_t key)
		{
			for (int k = 0; k < n; ++k) if (keys[k] == key) return;
			if (n < wire::kPressKeyCount) keys[n++] = key;
		};
		for (const std::string& name : Split(ev, ';'))
		{
			if (name == "dash_press") add(wire::PRESS_DASH);
			else if (name == "g_press") add(wire::PRESS_GRENADE);
			else if (name == "act_press")
			{
				if (s.act == "melee") add(wire::PRESS_MELEE);
				else if (s.act == "reload") add(wire::PRESS_RELOAD);
			}
			else if (name == "other_press")
			{
				if (StartsWith(s.other, "GA_Melee")) add(wire::PRESS_MELEE);
				else if (StartsWith(s.other, "GA_Weapon_Reload")) add(wire::PRESS_RELOAD);
			}
		}
		return n;
	}

	const char* KeyLetter(uint8_t key)
	{
		switch (key)
		{
		case wire::PRESS_MELEE: return "M";
		case wire::PRESS_DASH: return "D";
		case wire::PRESS_GRENADE: return "G";
		case wire::PRESS_RELOAD: return "R";
		default: return "?";
		}
	}

	struct RunCounts
	{
		int inputs = 0, presses = 0, multi = 0;
		std::string order;		// 누름이 둘 이상인 입력의 누른 차례(M>D …)
	};

	// 칸 하나를 예측에 넣는다. ordered = false 면 pressOrder 0(판 13 고정 차례 — 대조군)
	Outcome RunStep(CMoveSim& sim, const MatchRules& rules, float maxWalk, const std::map<int, Row>& rows, const std::vector<int>& frames,
		const LyraStep& lyra, bool ordered, RunCounts& counts)
	{
		const auto pred = std::make_unique<CSelfPrediction>(sim, rules, maxWalk);		// 고리가 커 힙에 둔다(DashCheck 와 같다)
		const Row& first = rows.at(frames.front());
		pred->OnSpawn(first.pos.x, first.pos.y, first.pos.z, 0, first.yaw);
		if (lyra.magBegin >= 0) (void)pred->OnPlace(first.pos.x, first.pos.y, first.pos.z, (uint8_t)EWeapon::Pistol, (int16_t)lyra.magBegin, kPistolSpare);
		Outcome out;
		for (size_t i = 1; i < frames.size(); ++i)
		{
			const Row& prev = rows.at(frames[i - 1]);
			uint8_t keys[wire::kPressKeyCount] = {};
			const int n = PressKeys(prev.ev, lyra, keys);
			wire::Input input = wire::Input();
			input.moveX = prev.inX;
			input.moveY = prev.inY;
			input.yaw = prev.ctlYaw;
			input.pitch = prev.ctlPitch;
			input.weapon = (uint8_t)EWeapon::Pistol;
			for (int k = 0; k < n; ++k) input.buttons = (uint8_t)(input.buttons | wire::kPressKeyButtons[keys[k]]);
			input.pressOrder = ordered && n >= 2 ? wire::MakePressOrder(keys, n) : 0;
			++counts.inputs;
			counts.presses += n;
			if (n >= 2)
			{
				++counts.multi;
				for (int k = 0; k < n; ++k) counts.order += std::string(k ? ">" : "") + KeyLetter(keys[k]);
			}
			const bool meleePressed = (input.buttons & wire::BUTTON_MELEE) != 0;
			pred->AddInput(input);
			wire::PlayerState probe = wire::PlayerState();
			probe.lastSeq = input.seq;
			const CSelfPrediction::Slot* slot = pred->FindComparable(probe);
			if (!slot) continue;
			if (slot->meleeStarted)
			{
				out.meleeOn = true;
				if (!pred->IsMeleeActive()) out.meleeCut = true;		// 이 입력의 뒤 차례(대시 · 던짐)가 끊었다
			}
			else if (meleePressed) out.meleeBlocked = true;
			if (slot->dashStarted) out.dashOn = true;
			if (slot->grenadeThrown) out.grenadeOn = true;
			const WeaponEvents& ev = pred->GetLastWeaponEvents();
			if (ev.reloadStarted) out.reloadOn = true;
			if (ev.reloadPressCutByGrenade) out.reloadCut = true;
		}
		if (lyra.magEnd >= 0) out.mag = pred->GetWeapon().GetMagazine();
		return out;
	}

	struct Totals
	{
		int steps = 0, fails = 0, fixedDiff = 0, inputs = 0, multi = 0, noMulti = 0, skipped = 0;
	};

	bool RunTag(CMoveSim& sim, const MatchRules& rules, float maxWalk, const std::string& dir, const std::string& tag, Totals& total)
	{
		std::vector<char> logData, csvData;
		const std::string base = dir + "/cprobe_" + tag;
		if (!ReadFile(base + ".log", logData) || !ReadFile(base + ".csv", csvData))
		{
			std::printf("PRESSCHECK input tag=%s 읽기 실패\n", tag.c_str());
			return false;
		}

		// 로그 — 능력 알림(GABIL) · 칸 결과(DCOMBO act · GCOMBO other · 탄창)
		std::map<std::string, LyraStep> lyra;
		for (const std::string& raw : Split(std::string(logData.begin(), logData.end()), '\n'))
		{
			const std::string line = !raw.empty() && raw.back() == '\r' ? raw.substr(0, raw.size() - 1) : raw;
			const size_t at = line.find("MOB_CPROBE_");
			if (at == std::string::npos) continue;
			const size_t space = line.find(' ', at);
			if (space == std::string::npos) continue;
			const std::string kind = line.substr(at + 11, space - at - 11);
			const std::string tail = line.substr(space);		// 앞 빈칸 포함 — Token 이 " key=" 로 찾는다
			const std::string step = Token(tail, "step");
			if (!StartsWith(step, kComboStepPrefix)) continue;
			LyraStep& s = lyra[step];
			if (kind == "GABIL")
			{
				const int frame = std::atoi(Token(tail, "frame").c_str());
				const std::string ev = Token(tail, "ev");
				if (ev == "on" || ev == "end")
				{
					AbilityTimes* t = Pick(s, Token(tail, "ability"));
					if (!t) continue;
					if (ev == "on" && t->on < 0) t->on = frame;
					else if (ev == "end" && t->on >= 0 && t->end < 0) t->end = frame;
				}
				else if (ev == "fail")
				{
					const std::string what = Token(tail, "what");
					AbilityTimes* t = Pick(s, what.substr(0, what.find('[')));
					if (t && t->fail < 0) t->fail = frame;
				}
			}
			else if (kind == "DCOMBO")
			{
				s.act = Token(tail, "act");
				s.combo = true;
			}
			else if (kind == "GCOMBO")
			{
				s.combo = true;
				s.other = Token(tail, "other");
				s.magBegin = std::atoi(Token(tail, "magBegin").c_str());
				s.magEnd = std::atoi(Token(tail, "magEnd").c_str());
			}
		}

		// 표 — 프레임마다(같은 프레임 줄이 여럿이면 값은 마지막 줄, 사건은 이어 붙임 — GrenadeCheck 와 같다)
		std::map<int, Row> rows;
		std::map<std::string, std::vector<int>> stepFrames;
		{
			const std::vector<std::string> lines = Split(std::string(csvData.begin(), csvData.end()), '\n');
			if (lines.empty()) return false;
			std::vector<std::string> head = Split(lines[0], ',');
			if (!head.empty() && !head.back().empty() && head.back().back() == '\r') head.back().pop_back();
			std::map<std::string, size_t> col;
			for (size_t i = 0; i < head.size(); ++i) if (!col.count(head[i])) col[head[i]] = i;
			for (const char* need : { "frame", "step", "ev", "px", "py", "pz", "yaw", "ctlPitch", "ctlYaw" })
				if (!col.count(need))
				{
					std::printf("PRESSCHECK input tag=%s 표에 %s 칸 없음\n", tag.c_str(), need);
					return false;
				}
			const bool hasMove = col.count("inX") && col.count("inY");
			for (size_t i = 1; i < lines.size(); ++i)
			{
				const std::vector<std::string> c = Split(lines[i], ',');
				if (c.size() < head.size()) continue;
				const auto num = [&](const char* n) { return std::atof(c[col[n]].c_str()); };
				const int frame = std::atoi(c[col["frame"]].c_str());
				Row& r = rows[frame];
				r.step = c[col["step"]];
				r.ev += c[col["ev"]];
				r.pos = Vec3(num("px"), num("py"), num("pz"));
				r.yaw = (float)num("yaw");
				r.ctlPitch = (float)num("ctlPitch");
				r.ctlYaw = (float)num("ctlYaw");
				if (hasMove) { r.inX = (float)num("inX"); r.inY = (float)num("inY"); }
			}
			for (const auto& kv : rows)
				if (StartsWith(kv.second.step, kComboStepPrefix)) stepFrames[kv.second.step].push_back(kv.first);
		}

		int tagFails = 0;
		for (const auto& kv : stepFrames)
		{
			const std::string& name = kv.first;
			const std::vector<int>& frames = kv.second;
			const auto it = lyra.find(name);
			if (it != lyra.end() && !it->second.combo)
			{
				++total.skipped;
				continue;
			}
			if (frames.size() < 2 || it == lyra.end())
			{
				std::printf("PRESSCHECK tag=%s step=%s 기록 부족 rows=%zu lyra=%d\n", tag.c_str(), name.c_str(), frames.size(), it != lyra.end());
				++tagFails;
				continue;
			}
			const LyraStep& s = it->second;
			const Outcome want = LyraOutcome(s);
			RunCounts counts, fixedCounts;
			const Outcome got = RunStep(sim, rules, maxWalk, rows, frames, s, true, counts);
			const Outcome fixed = RunStep(sim, rules, maxWalk, rows, frames, s, false, fixedCounts);
			const bool ok = got == want && counts.multi > 0;
			const bool fixedSame = fixed == want;
			++total.steps;
			total.inputs += counts.inputs;
			total.multi += counts.multi;
			if (counts.multi == 0) ++total.noMulti;
			if (!ok) ++tagFails;
			if (!fixedSame) ++total.fixedDiff;
			std::printf("PRESSCHECK tag=%s step=%s %s order=%s inputs=%d presses=%d multi=%d ours=[%s] lyra=[%s] fixed=%s%s\n",
				tag.c_str(), name.c_str(), ok ? "ok" : "DIFF", counts.order.empty() ? "-" : counts.order.c_str(), counts.inputs, counts.presses, counts.multi,
				got.Text().c_str(), want.Text().c_str(), fixedSame ? "same" : "diff", fixedSame ? "" : (" [" + fixed.Text() + "]").c_str());
		}
		if (stepFrames.empty())
		{
			std::printf("PRESSCHECK tag=%s s_ 칸 없음\n", tag.c_str());
			++tagFails;
		}
		total.fails += tagFails;
		return true;
	}
}

int main(int argc, char** argv)
{
	if (argc < 4)
	{
		std::printf("사용: PressCheck <map_gym.mocl> <기록 폴더> <태그>...\n");
		return 2;
	}
	std::vector<char> mapData;
	CMapCollision map;
	if (!ReadFile(argv[1], mapData) || map.LoadFromMemory(mapData.data(), mapData.size()) != EMapLoad::Ok)
	{
		std::printf("PRESSCHECK FAIL 맵을 못 읽음: %s\n", argv[1]);
		return 2;
	}
	const MoveParams params = MakeLyraMoveParams();
	CMoveSim sim(map, params);
	MatchRules rules;
	rules.spawnImmunitySeconds = 0.0;		// 부활 이동 막음을 끈다 — 칸 첫 입력부터 받는다(DashCheck ComparePrediction 과 같다)
	Totals total;
	for (int i = 3; i < argc; ++i)
		if (!RunTag(sim, rules, (float)params.maxWalkSpeed, argv[2], argv[i], total)) return 2;
	const bool pass = total.steps > 0 && total.fails == 0 && total.noMulti == 0 && total.fixedDiff > 0;
	std::printf("PRESSCHECK steps=%d fails=%d fixedOrderDiff=%d inputs=%d multi=%d noMulti=%d skipped=%d\n",
		total.steps, total.fails, total.fixedDiff, total.inputs, total.multi, total.noMulti, total.skipped);
	std::printf("PRESSCHECK %s\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
