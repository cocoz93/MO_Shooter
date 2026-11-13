#include "AnimState.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mo
{
	namespace
	{
		// 값과 근거는 MOBSimpleAnimInstance.cpp 의 같은 이름 상수에 적혀 있다(Lyra 실행값에서 읽었거나 시험용 제안값)
		constexpr float kMoveFullSpeed = 300.f;
		constexpr float kBlendSeconds = 0.1f;
		constexpr float kMinPlayRate = 0.25f;
		constexpr float kMaxPlayRate = 1.5f;
		constexpr float kAimBlendSpeed = 10.f;
		constexpr float kJumpBlendSeconds = 0.1f;
		constexpr float kApexLeadSeconds = 0.25f;
		constexpr float kApexExitRemaining = 0.12f;
		constexpr float kLandFadeSeconds = 0.3f;
		constexpr float kLandedStandSeconds = 0.15f;
		constexpr float kLandedMoveSeconds = 0.2f;
		constexpr float kIdleBreakDelay = 6.f;
		constexpr float kPlantReleaseSeconds = 0.2f;
		constexpr float kPlantLandDelay = 0.1f;
		constexpr float kPlantTimeSlack = 0.001f;
		constexpr int kLandTableHz = 120;
		constexpr int kTransitionTableHz = 120;
		constexpr float kCardinalDeadZone = 10.f;
		constexpr float kToStartSeconds = 0.21f;
		constexpr float kStopToStartSeconds = 0.175f;
		constexpr float kToStopSeconds = 0.28f;
		constexpr float kToPivotSeconds = 0.25f;
		constexpr float kStartToCycleSeconds = 0.99f;
		constexpr float kPivotToCycleSeconds = 0.5f;
		constexpr float kStopToIdleSeconds = 0.74f;
		constexpr float kStepLeaderWeight = 0.5f;
		constexpr float kPivotMinSpeed = 10.f;
		constexpr float kDistanceRateMin = 0.25f;
		constexpr float kDistanceRateMax = 5.f;

		// 무기 묶음마다 다른 값 — 전환 클립 길이가 무기마다 달라 빠지는 시각도 다르다(소총 멈춤 1.712 를 권총에 쓰면 길이 0.675 클립에서 못 빠진다).
		// 방향 차례 = 앞 · 뒤 · 왼 · 오른. 값 = 걸음 상태 기계 칸이 바뀌는 프레임의 나가는 클립 시각(Lyra 실행값).
		// - 소총 = MOBSimpleAnimInstance.cpp 의 START_EXIT_TIMES · STOP_EXIT_TIMES · PIVOT_EXIT_TIMES · STOP_CLIP_SECONDS
		// - 권총 = 같은 방법으로 9단계 1-b 기록에서 잰 값. 오른쪽 돌아서기 1.822 만 계산값(그 클립의 TransitionToLocomotion 표시 시작 1.800 + 0.022, 잰 일곱의 평균 차이).
		//   멈춘 뒤 서기 0.35 는 소총처럼 앞 방향 기준으로 고른 값
		// - 샷건 = 소총 몸이라 소총 값(D45)
		struct BundleTiming
		{
			const char* bundle;
			float stopClipSeconds;			// 멈춘 뒤 서기 상태가 될 때까지 — 대기 몸짓 시계의 출발점
			float exitTimes[3][4];			// [출발 · 멈춤 · 돌아서기][방향] — 전환 클립이 이 시각을 지나면 달리기 · 서기로 빠진다
		};
		constexpr BundleTiming kBundleTimings[] = {
			{ "rifle", 1.35f, { { 0.791f, 1.165f, 1.185f, 1.122f }, { 1.712f, 2.046f, 2.180f, 2.277f }, { 1.344f, 1.591f, 1.565f, 1.358f } } },
			{ "pistol", 0.35f, { { 0.533f, 0.424f, 1.359f, 1.023f }, { 0.675f, 1.016f, 1.016f, 1.008f }, { 1.433f, 1.526f, 1.734f, 1.822f } } },
			{ "shotgun", 1.35f, { { 0.791f, 1.165f, 1.185f, 1.122f }, { 1.712f, 2.046f, 2.180f, 2.277f }, { 1.344f, 1.591f, 1.565f, 1.358f } } },
		};

		constexpr float kSmallNumber = 1.e-8f;			// 엔진 UE_SMALL_NUMBER
		constexpr float kKindaSmall = 1.e-4f;			// 엔진 UE_KINDA_SMALL_NUMBER
		constexpr float kZeroWeight = 0.00001f;			// 엔진 ZERO_ANIMWEIGHT_THRESH
		constexpr float kPi = 3.1415926535897932f;		// 엔진 UE_PI

		// 패킷 시각의 눈금 — 1/6000초 칸. 소총 대기 몸짓(8.33초)이 들어가도록 고른 값으로, 2바이트 최대는 10.92초다. 0xFFFF 는 「안 함」 표시로 비워 둔다
		constexpr float kWireTimeScale = 6000.f;
		constexpr float kWireTimeMax = 65534.f / kWireTimeScale;
		// 대기 몸짓 칸만 1/3000초(D47) — 권총 몸짓 MF_Pistol_IdleBreak_Scan 이 13.8초라 10.92초를 넘는다. 옛 칸 두 개를 한 칸으로 써 최대 21.84초,
		// 한 칸 0.33ms 는 60Hz 한 틱(16.7ms)의 1/50 이다. 서버 · 화면 · 봇이 같은 칸 값을 풀어 쓰므로 셋 사이 차이는 안 생긴다
		constexpr float kWireFidgetScale = 3000.f;
		constexpr float kWireFidgetMax = 65534.f / kWireFidgetScale;
		constexpr float kWirePlantScale = 100.f;		// 발 디딤 내림 cm → 0.01cm 칸
		static_assert(wire::kAnimMontagesOnWire == kAnimMontageMax, "패킷 몽타주 칸 수 = 서버가 들고 도는 칸 수. 서버가 버린 뒤의 칸을 그대로 싣는다(플랜 넘칠 때 규칙)");
		static_assert(MONTAGE_KIND_COUNT <= (1 << wire::kAnimMontageKindBits), "몽타주 종류가 칸 번호 아래 4비트에 들어가야 한다");
		constexpr uint8_t kWireMontageKindMask = (uint8_t)((1 << wire::kAnimMontageKindBits) - 1);

		// 쏜 뒤 윗몸 덮기(D45). 0.5 = Lyra 층 변수 RaiseWeaponAfterFiringDuration(세 무기 같은 값, 실측).
		// 덮기 비중은 그 뒤 InterpTo 속도 1 로 줄어든다 — 기록에서 프레임마다 ×(1 − 1/60) 인 것을 읽은 값(D45)
		constexpr float kRaiseWeaponSeconds = 0.5f;
		constexpr float kOverrideFallSpeed = 1.f;
		constexpr float kNeverFiredSeconds = 1000.f;	// 아직 안 쐈다 — 덮기 문턱보다 훨씬 큰 아무 값이고, 그 위로는 세지 않는다(제 판단)

		constexpr float kBigNumber = 3.4e+38f;			// 엔진 UE_BIG_NUMBER
		constexpr int kMontageMaxIterations = 10;		// 엔진 FAnimMontageInstance::Advance MaxIterations
		constexpr float kMontageMaxTimeLeft = 10.f;		// 엔진 같은 함수 — 남은 시간이 이보다 작을 때만 이번 dt 를 더한다
		constexpr float kMontageTestFrame = 1.f / 60.f;	// 일부러 틀림 「한 프레임 늦게」의 한 프레임

		enum { kClipIdle, kClipFwd, kClipBwd, kClipLeft, kClipRight };
		enum { kJumpStart, kJumpStartLoop, kJumpApex, kJumpFallLoop, kJumpLand };
		enum { kTransitionStart, kTransitionStop, kTransitionPivot };
		enum { kDirFwd, kDirBwd, kDirLeft, kDirRight };
		constexpr int kJumpNone = -1;

		float Clamp(float x, float low, float high) { return std::max(low, std::min(x, high)); }

		float Smooth(float x)
		{
			x = Clamp(x, 0.f, 1.f);
			return x * x * (3.f - 2.f * x);
		}

		int CardinalFromAngle(float angle)
		{
			const float value = std::abs(angle);
			if (value <= 45.f + kCardinalDeadZone) return kDirFwd;
			if (value >= 135.f - kCardinalDeadZone) return kDirBwd;
			return angle > 0.f ? kDirRight : kDirLeft;
		}

		float AngleOf(const double v[3])
		{
			return std::atan2((float)v[1], (float)v[0]) * (180.f / kPi);
		}

		int Gcd(int a, int b)
		{
			while (b != 0)
			{
				const int t = a % b;
				a = b;
				b = t;
			}
			return a;
		}

		uint8_t ToWireByte(float value) { return (uint8_t)std::lround(Clamp(value, 0.f, 1.f) * 255.f); }
		uint16_t ToWireTime(float seconds) { return (uint16_t)std::lround(Clamp(seconds, 0.f, kWireTimeMax) * kWireTimeScale); }
		float FromWireTime(uint16_t value) { return (float)value / kWireTimeScale; }
		uint16_t ToWireFidget(float seconds) { return (uint16_t)std::lround(Clamp(seconds, 0.f, kWireFidgetMax) * kWireFidgetScale); }
		float FromWireFidget(uint16_t value) { return (float)value / kWireFidgetScale; }

		// 엔진 FRotator::NormalizeAxis 뒤 ±90 으로 자른다 — 언리얼 쪽 Pitch 와 같은 식
		float NormalizePitch(float degrees)
		{
			float d = std::fmod(degrees, 360.f);
			if (d < 0.f) d += 360.f;
			if (d > 180.f) d -= 360.f;
			return Clamp(d, -90.f, 90.f);
		}

		// 엔진 FMath::FInterpTo (float)
		float InterpTo(float current, float target, float deltaSeconds, float speed)
		{
			if (speed <= 0.f) return target;
			const float distance = target - current;
			if (distance * distance < kSmallNumber) return target;
			return current + distance * Clamp(deltaSeconds * speed, 0.f, 1.f);
		}

		// 엔진 FAlphaBlend::AlphaToBlendOption 의 세 갈래(kBlendOptionMax 까지 — 읽기에서 그 밖은 거부한다)
		float BlendCurve(uint8_t option, float alpha)
		{
			switch (option)
			{
			case 1:		// Cubic = FMath::CubicInterp(0, 0, 1, 0, a)
			{
				const float a2 = alpha * alpha;
				const float a3 = a2 * alpha;
				return Clamp((-2.f * a3) + (3.f * a2), 0.f, 1.f);
			}
			case 2:		// HermiteCubic = FMath::SmoothStep(0, 1, a)
				if (alpha < 0.f) return 0.f;
				if (alpha >= 1.f) return 1.f;
				return Clamp(alpha * alpha * (3.f - 2.f * alpha), 0.f, 1.f);
			default:	// 선형
				return Clamp(alpha, 0.f, 1.f);
			}
		}

		// 몽타주 곡선 값 — hz 칸 사이는 선형, 끝 밖은 끝 값
		float SampleCurve(const MontageCurve& curve, float seconds)
		{
			const std::vector<float>& values = curve.values;
			const float x = std::max(seconds, 0.f) * (float)curve.hz;
			const size_t index = (size_t)x;
			if (index + 1 >= values.size()) return values.back();
			const float frac = x - (float)index;
			return values[index] + (values[index + 1] - values[index]) * frac;
		}
	}

	// ─── 엔진 FAlphaBlend ───

	void CAnimState::MontageBlend::SetValueRange(float beginValue, float desiredValue)
	{
		begin = beginValue;
		desired = desiredValue;
		resetAlpha = true;
	}

	void CAnimState::MontageBlend::SetBlendTime(float seconds)
	{
		blendTime = std::max(seconds, 0.f);
		resetTime = true;
	}

	void CAnimState::MontageBlend::SetAlpha(float value)
	{
		alpha = Clamp(value, 0.f, 1.f);
		blended = begin + (desired - begin) * BlendCurve(option, alpha);
	}

	bool CAnimState::MontageBlend::IsComplete() const
	{
		return begin + (desired - begin) * BlendCurve(option, 1.f) == blended;
	}

	void CAnimState::MontageBlend::Update(float deltaSeconds)
	{
		if (resetAlpha)
		{
			if (begin == desired) SetAlpha(1.f);
			else SetAlpha((blended - begin) / (desired - begin));
			resetAlpha = false;
		}
		if (resetTime)
		{
			if (blendTime <= 0.f)
			{
				remaining = 0.f;
				SetAlpha(1.f);
			}
			else
			{
				remaining = blendTime * std::fabs(1.f - alpha);
			}
			resetTime = false;
		}
		if (!IsComplete())
		{
			if (remaining > deltaSeconds)
			{
				const float delta = 1.f - alpha;
				alpha += (delta / remaining) * deltaSeconds;
				remaining -= deltaSeconds;
				SetAlpha(alpha);
			}
			else
			{
				remaining = 0.f;
				SetAlpha(1.f);
			}
		}
	}

	// ─── 엔진 FAnimMontageInstance ───

	bool CAnimState::PlayMontage(uint8_t kind, float playRate, int bundle)
	{
		if (!_clips) return false;
		if (bundle < 0) bundle = _bundleIndex;
		const std::vector<ClipBundle>& bundles = _clips->Bundles();
		if (bundle < 0 || bundle >= (int)bundles.size()) return false;
		const ClipMontage* montage = bundles[(size_t)bundle].FindMontage(kind);
		if (!montage) return false;
		const bool linear = (_montageTest & kMontageTestLinear) != 0;
		const uint8_t inOption = linear ? 0 : montage->blendInOption;

		// 같은 무리를 새 몽타주의 넣기 섞기로 끈다 — 뒤 칸부터(엔진 StopAllMontagesByGroupName)
		if ((_montageTest & kMontageTestNoGroup) == 0)
		{
			for (int i = _montageCount - 1; i >= 0; --i)
			{
				if (_montages[i].montage->group == montage->group) StopMontage(_montages[i], inOption, montage->blendIn);
			}
		}
		// 칸이 찼으면 다 빠진 칸(목표 0 · 섞기 끝)부터 걷는다 — 사건은 갱신 앞에 틀어서(D46) 이 칸은 다음 UpdateMontages 가 자세에 넣기 전에 지운다. 그래도 차야 살아 있는 칸을 버린다(플랜 넘칠 때 규칙)
		if (_montageCount == kAnimMontageMax)
		{
			int kept = 0;
			for (int i = 0; i < _montageCount; ++i)
			{
				if (_montages[i].blend.desired == 0.f && _montages[i].blend.IsComplete()) continue;
				if (kept != i) _montages[kept] = _montages[i];
				++kept;
			}
			_montageCount = kept;
		}
		if (_montageCount == kAnimMontageMax) DropMontage();

		MontageInstance& instance = _montages[_montageCount++];
		instance = MontageInstance{};
		instance.montage = montage;
		instance.bundle = bundle;
		instance.position = (_montageTest & kMontageTestLateFrame) != 0 ? std::min(kMontageTestFrame, montage->length) : 0.f;
		instance.blend.SetValueRange(0.f, 1.f);				// Initialize
		instance.playing = true;							// Play — 지금 비중에서 1 로, 넣기 시간 · 곡선으로
		instance.playRate = playRate;
		const float current = instance.blend.blended;
		instance.blend.option = inOption;
		instance.blend.SetBlendTime(montage->blendIn);
		instance.blend.SetValueRange(current, 1.f);
		instance.autoBlendOut = montage->autoBlendOut;
		return true;
	}

	bool CAnimState::StopMontageKind(uint8_t kind, int bundle)
	{
		if (!_clips) return false;
		if (bundle < 0) bundle = _bundleIndex;
		const std::vector<ClipBundle>& bundles = _clips->Bundles();
		if (bundle < 0 || bundle >= (int)bundles.size()) return false;
		const ClipMontage* montage = bundles[(size_t)bundle].FindMontage(kind);
		if (!montage) return false;
		for (int i = _montageCount - 1; i >= 0; --i)
		{
			MontageInstance& instance = _montages[i];
			if (instance.montage != montage || instance.blend.desired == 0.f) continue;
			const uint8_t outOption = (_montageTest & kMontageTestLinear) != 0 ? 0 : montage->blendOutOption;
			StopMontage(instance, outOption, montage->blendOut);
			return true;
		}
		return false;
	}

	bool CAnimState::PlayFireMontage(float playRate, int bundle, bool& cancelledReload)
	{
		cancelledReload = false;
		for (int i = 0; i < _montageCount; ++i)
		{
			const MontageInstance& instance = _montages[i];
			if (instance.montage->kind != MONTAGE_RELOAD || instance.blend.desired == 0.f) continue;
			cancelledReload = StopMontageKind(MONTAGE_RELOAD, instance.bundle);
			break;
		}
		return PlayMontage(MONTAGE_FIRE, playRate, bundle);
	}

	void CAnimState::StopMontage(MontageInstance& instance, uint8_t option, float blendSeconds)
	{
		MontageBlend& blend = instance.blend;
		if (blend.desired != 0.f)
		{
			blend.option = option;
			blend.SetBlendTime(blendSeconds);
			blend.SetValueRange(blend.blended, 0.f);
			blend.Update(0.f);
		}
		else if (blendSeconds < blend.blendTime)
		{
			// 이미 빠지는 중이면 더 짧은 시간으로만 바꾼다
			blend.SetBlendTime(blendSeconds);
			blend.SetValueRange(blend.blended, 0.f);
		}
		if (blend.blendTime <= 0.f) instance.playing = false;
	}

	// 칸이 찼다 — 비중이 가장 작은 것을 버린다. 같으면 빠지는 중인 것, 그다음 오래된 것(제 판단 — Lyra 기록은 3칸을 안 넘는다, D45)
	void CAnimState::DropMontage()
	{
		int pick = 0;
		for (int i = 1; i < _montageCount; ++i)
		{
			const MontageBlend& a = _montages[i].blend;
			const MontageBlend& b = _montages[pick].blend;
			if (a.blended < b.blended || (a.blended == b.blended && a.desired == 0.f && b.desired != 0.f)) pick = i;
		}
		for (int i = pick; i + 1 < _montageCount; ++i) _montages[i] = _montages[i + 1];
		--_montageCount;
		++_montageDrops;
	}

	// 구역 하나(0 ~ 길이)만 옮겼다 — 구역이 여럿이면 묶는 도구가 멈춘다(D48). 분기점 · 시간 늘이기 곡선 · 뿌리 이동 · 알림은 옮기지 않았다(클립 파일에 그 칸이 없다)
	bool CAnimState::AdvanceMontage(MontageInstance& instance, float deltaSeconds)
	{
		const ClipMontage& asset = *instance.montage;
		if (instance.playing)
		{
			if (instance.timeLeft < kMontageMaxTimeLeft) instance.timeLeft += deltaSeconds;
			int iterations = 0;
			while (instance.playing && instance.timeLeft > kSmallNumber && ++iterations < kMontageMaxIterations)
			{
				// FMontageSubStepper::Advance
				const float rate = instance.playRate * asset.rate;
				const bool zeroRate = std::fabs(rate) <= kSmallNumber;
				const bool forward = rate > 0.f;
				bool reachedEnd = false;
				bool moved = false;
				if (!zeroRate)
				{
					float deltaMove = instance.timeLeft * rate;
					if (forward)
					{
						const float maxMove = asset.length - instance.position;
						if (deltaMove >= maxMove)
						{
							deltaMove = maxMove;
							reachedEnd = true;
						}
					}
					else
					{
						const float minMove = -instance.position;
						if (deltaMove <= minMove)
						{
							deltaMove = minMove;
							reachedEnd = true;
						}
					}
					if (std::fabs(deltaMove) > 0.f)
					{
						instance.position += deltaMove;
						instance.timeLeft = std::max(instance.timeLeft - deltaMove / rate, 0.f);
						moved = true;
					}
				}

				// 자동 빼기 — 끝까지 남은 재생 시간이 빼기 시간(또는 자산의 빼기 시작 시각) 안이면 끈다
				if (instance.blend.desired != 0.f && instance.autoBlendOut)
				{
					const float toEnd = zeroRate ? kBigNumber : std::fabs(((forward ? asset.length : 0.f) - instance.position) / rate);
					const bool customTrigger = asset.blendOutTrigger >= 0.f;
					const float trigger = customTrigger ? asset.blendOutTrigger : asset.blendOut;
					if (toEnd <= std::max(trigger, kKindaSmall))
					{
						const uint8_t outOption = (_montageTest & kMontageTestLinear) != 0 ? 0 : asset.blendOutOption;
						StopMontage(instance, outOption, customTrigger ? asset.blendOut : toEnd);
					}
				}

				if (reachedEnd)
				{
					instance.playing = false;
					instance.position = forward ? asset.length - kKindaSmall / 2.f : 0.f;
					break;
				}
				if (!moved) break;
			}
		}
		return !(instance.blend.desired == 0.f && instance.blend.IsComplete());
	}

	// 엔진 UAnimInstance::UpdateMontage — 비중을 다 갱신한 뒤 시각을 다 넘기고, 다 빠진 칸은 뺀다(차례는 그대로)
	void CAnimState::UpdateMontages(float deltaSeconds)
	{
		for (int i = 0; i < _montageCount; ++i) _montages[i].blend.Update(deltaSeconds);
		int kept = 0;
		for (int i = 0; i < _montageCount; ++i)
		{
			if (!AdvanceMontage(_montages[i], deltaSeconds)) continue;
			if (kept != i) _montages[kept] = _montages[i];
			++kept;
		}
		_montageCount = kept;
	}

	// 이번 자세에 들어갈 목록과 그 자세가 내는 DisableLHandIK. 엔진은 비중이 ZERO_ANIMWEIGHT_THRESH 이하인 몽타주를 자세 계산에 안 넣는다
	void CAnimState::SnapshotMontages()
	{
		_poseMontageCount = 0;
		for (int i = 0; i < _montageCount; ++i)
		{
			const MontageInstance& instance = _montages[i];
			if (instance.blend.blended <= kZeroWeight) continue;
			AnimMontageSlot& slot = _poseMontages[_poseMontageCount++];
			slot.bundle = (int8_t)instance.bundle;
			slot.kind = instance.montage->kind;
			slot.position = instance.position;
			slot.weight = instance.blend.blended;
		}
		_disableLeftHand = LeftHandCurve();
	}

	// 몽타주 곡선 DisableLHandIK × 비중의 합. 몽타주 자체 곡선과, 덧붙임이 아닌 자리에 든 줄의 클립 곡선만 더한다 —
	// 소총 재장전은 윗몸 · 윗몸 덧붙임 두 줄에 같은 곡선이 있는데, 둘 다 더하면 Lyra 기록과 1.0 까지 어긋나고 앞 줄만 더하면 0.02 안이다(3-b curve_hyp, 실측)
	float CAnimState::LeftHandCurve() const
	{
		float sum = 0.f;
		const std::vector<ClipSlot>& slots = _clips->Slots();
		for (int i = 0; i < _poseMontageCount; ++i)
		{
			const AnimMontageSlot& slot = _poseMontages[i];
			const ClipMontage& montage = *_clips->Bundles()[(size_t)slot.bundle].FindMontage(slot.kind);
			for (const MontageCurve& curve : montage.curves)
			{
				if (curve.name != "DisableLHandIK") continue;
				if (curve.owner < 0)
				{
					sum += slot.weight * SampleCurve(curve, slot.position);
					continue;
				}
				const MontageTrack& track = montage.tracks[(size_t)curve.owner];
				if (slots[track.slot].mode == ESlotMode::Additive) continue;
				const float clipTime = Clamp(track.clipStart + (slot.position - track.startPos) * track.playRate, track.clipStart, track.clipEnd);
				sum += slot.weight * SampleCurve(curve, clipTime);
			}
		}
		return sum;
	}

	CAnimState::MontageView CAnimState::MontageAt(int index) const
	{
		MontageView view;
		if (index < 0 || index >= _montageCount) return view;
		const MontageInstance& instance = _montages[index];
		view.montage = instance.montage;
		view.bundle = instance.bundle;
		view.position = instance.position;
		view.weight = instance.blend.blended;
		view.desired = instance.blend.desired;
		view.blendTime = instance.blend.blendTime;
		view.playRate = instance.playRate;
		return view;
	}

	// 쏜 뒤 시간 → 덮기 비중 → 조준 보정 비중(D45). Lyra 차례 = 기본 애니(쏜 뒤 시간)가 먼저, 무기 층(덮기 · 조준)이 그 값을 같은 갱신에 읽는다
	void CAnimState::UpdateFiring(bool accelerating, float deltaSeconds)
	{
		_timeSinceFired = _firing ? 0.f : std::min(_timeSinceFired + deltaSeconds, kNeverFiredSeconds);
		_overrideWeight = _timeSinceFired < kRaiseWeaponSeconds ? 1.f : InterpTo(_overrideWeight, 0.f, deltaSeconds, kOverrideFallSpeed);
		// 쏜 지 0.5초 안(덮기 비중 1)이면 조준 보정 비중도 보간 없이 1 — 3-b 소총 층 4,394줄 중 그런 줄 640개가 다 1, 달리며 쏜 세 번(0.9568 · 0.3266 · 0.9568)이 쏜 줄에 바로 1(실측)
		_aimBlend = _timeSinceFired < kRaiseWeaponSeconds ? 1.f : InterpTo(_aimBlend, accelerating ? _overrideWeight : 1.f, deltaSeconds, kAimBlendSpeed);
	}

	bool CAnimState::Init(const CAnimClips& clips, const char* bundleName)
	{
		if (!LoadBundle(clips, bundleName)) return false;
		Reset();
		return true;
	}

	// Lyra 가 무기를 바꾸면 무기 층(애니 레이어)이 통째로 바뀐다. 새 층의 첫 프레임을 9단계 1-c 기록(ev_rifle · ev_shotgun 의 무기 교체 6번 — 서서 4 · 달리며 2, 실측)대로 옮긴다
	// - 멈춤 · 서기 → 서기. 서기 클립은 처음부터(소총 멈춤 → 권총 서기 0.017 = 한 프레임)
	// - 출발 · 돌아서기 · 달리기 → 전환 클립 없이 달리기 반복. 걸음 번호 · 진행률은 그대로 둬 발 표시로 잇는다(권총 달리기 0.936 → 소총 0.008)
	// - 공중 → 그대로(기록에 없다, 제 판단)
	// - 조준 보정 비중은 새 층의 처음 값 1 에서 이 틱 갱신부터 따라간다(교체 다음 줄 0.8333 → 0.6944 — 3-b 소총 2011 · 4481, 실측).
	//   교체 사건 줄에 찍힌 1 은 기록이 새 층의 처음 값을 한 줄 먼저 보이는 것이고, 그 줄 자세는 앞 층으로 만든다(D46)
	// - 왼손 비중은 이 틱만 1 — 새 층은 아직 곡선을 읽은 적이 없다(같은 두 줄이 1.0, 앞 곡선 0.364 로 셈하면 0.636, 실측)
	// - 대기 몸짓은 끊고, 서기에 막 들어온 때로 본다(새 층의 시계가 처음부터 — 제 판단)
	bool CAnimState::ChangeBundle(const CAnimClips& clips, const char* bundleName)
	{
		CAnimState next = *this;		// 표를 읽다 실패하면 지금 상태를 그대로 둔다
		if (!next.LoadBundle(clips, bundleName)) return false;
		*this = next;

		switch (_moveState)
		{
		case EMoveState::Idle:
		case EMoveState::Stop:
			_moveState = EMoveState::Idle;
			_idleTime = 0.f;
			_count = 0;
			break;
		case EMoveState::Start:
		case EMoveState::Pivot:
		case EMoveState::Cycle:
			_moveState = EMoveState::Cycle;
			_count = 0;
			break;
		case EMoveState::Air:
			break;
		}
		_followClip = -1;
		_step %= _stepCycle;		// 두 묶음의 주기가 다 짝수라 왼발 · 오른발 차례는 안 바뀐다
		_aimBlend = 1.f;
		_handFresh = true;
		// 덮기 비중은 새 층의 처음 값 0 에서 다시 센다 — 교체 다음 줄 1806 이 0 이고 앞 층 값 0.519 를 잇지 않는다.
		// 쏜 뒤 시간은 기본 애니 쪽이라 그대로다 — 무기 교체를 지나도 기록과 0.0004 안(3-b 소총, 실측)
		// 도는 몽타주도 기본 애니 쪽이라 그대로 이어 돈다
		_overrideWeight = 0.f;
		_fidgetTime = -1.f;
		_fidgetLeaving = false;
		_standTime = _stopClipSeconds;
		return true;
	}

	bool CAnimState::LoadBundle(const CAnimClips& clips, const char* bundleName)
	{
		const ClipBundle* bundle = clips.FindBundle(bundleName);
		if (!bundle || bundle->clips.size() < ACLIP_COUNT) return false;		// 30개 뒤는 몽타주 · 덮기 자세 클립(9단계 3-a)
		_clips = &clips;
		_bundleIndex = (int)(bundle - clips.Bundles().data());
		const BundleTiming* timing = nullptr;
		for (const BundleTiming& candidate : kBundleTimings)
		{
			if (std::strcmp(candidate.bundle, bundleName) == 0) timing = &candidate;
		}
		if (!timing) return false;
		_stopClipSeconds = timing->stopClipSeconds;
		for (int kind = 0; kind < 3; ++kind)
		{
			for (int direction = 0; direction < 4; ++direction) _exitTimes[kind][direction] = timing->exitTimes[kind][direction];
		}

		_stepCycle = 1;
		for (int i = 0; i < kAnimWalkClips; ++i)
		{
			const AnimClip& clip = bundle->clips[(size_t)ACLIP_IDLE + i];
			_lengths[i] = std::max(clip.length, 0.01f);
			_rootSpeeds[i] = clip.rootSpeed;

			// 발 표시를 왼발부터 줄 세운다. 번갈아 있고 개수가 짝수여야 한 바퀴 돌아도 발 순서가 안 깨진다
			const std::vector<ClipMarker>& markers = clip.markers;
			size_t firstLeft = 0;
			while (firstLeft < markers.size() && !markers[firstLeft].left) ++firstLeft;
			_stepTimes[i].clear();
			for (size_t m = 0; m < markers.size() && firstLeft < markers.size(); ++m)
			{
				const size_t index = (firstLeft + m) % markers.size();
				_stepTimes[i].push_back(markers[index].time + (index < firstLeft ? _lengths[i] : 0.f));
			}
			if (i != kClipIdle)
			{
				const int count = (int)_stepTimes[i].size();
				if (count == 0 || count % 2 != 0) return false;
				_stepCycle = _stepCycle / Gcd(_stepCycle, count) * count;
			}
		}

		if (_stepCycle > 255) return false;		// 패킷의 걸음 번호는 1바이트다

		for (int i = 0; i < kAnimStackClips; ++i) _stackLengths[i] = std::max(bundle->clips[(size_t)ACLIP_JUMP_START + i].length, 0.01f);
		for (size_t c = 0; c < ACLIP_COUNT; ++c)
		{
			// 패킷의 시각은 이 길이까지만 담는다 — 대기 몸짓 칸만 눈금이 다르다. 30개 뒤 몽타주 클립은 클립 시각이 아니라 몽타주 시각이 패킷에 실린다(조각 4)
			if (bundle->clips[c].length > (c == ACLIP_FIDGET ? kWireFidgetMax : kWireTimeMax)) return false;
		}
		// 몽타주 칸(조각 4) — 몽타주 시각이 패킷 시각 칸에, 묶음 번호가 칸 번호의 위 4비트에 들어가야 한다. 다른 묶음 몽타주도 틀 수 있어 전부 본다
		if (clips.Bundles().size() > ((size_t)1 << (8 - wire::kAnimMontageKindBits))) return false;
		for (const ClipBundle& each : clips.Bundles())
		{
			for (const ClipMontage& montage : each.montages)
			{
				if (montage.length > kWireTimeMax) return false;
			}
		}
		for (int t = 0; t < kAnimTransitions; ++t)
		{
			const AnimClip& clip = bundle->clips[(size_t)ACLIP_TRANSITION_FIRST + t];
			if (!clip.hasCurve || clip.curveHz != (uint32_t)kTransitionTableHz) return false;
			_transitionDistances[t] = &clip.curve;
			_transitionMarkers[t] = &clip.markers;
		}
		const AnimClip& land = bundle->clips[ACLIP_JUMP_LAND];
		if (!land.hasCurve || land.curveHz != (uint32_t)kLandTableHz) return false;
		_landDistances = &land.curve;
		_recoveryLength = std::max(bundle->clips[ACLIP_RECOVERY].length, 0.01f);
		_fidgetLength = std::max(bundle->clips[ACLIP_FIDGET].length, 0.01f);

		_groundFriction = bundle->groundFriction;
		_brakingFriction = bundle->brakingFriction;
		_brakingFrictionFactor = bundle->brakingFrictionFactor;
		_brakingDecelerationWalking = bundle->brakingDecelerationWalking;
		_gravity = -bundle->gravityZ;
		_separateBraking = bundle->separateBraking;
		return true;
	}

	void CAnimState::Reset()
	{
		for (int i = 0; i < kAnimWalkClips; ++i) _weights[i] = i == kClipIdle ? 1.f : 0.f;
		_step = 0;
		_stepFrac = 0.f;
		_timesStep = 0;
		_timesStepFrac = 0.f;
		// 판 시작(순간이동 + 멈춤)도 「방금 멈췄다」로 본다 — 대기 몸짓 시계와 같은 기준
		_idleTime = _lengths[kClipIdle] > _stopClipSeconds ? _lengths[kClipIdle] - _stopClipSeconds : 0.f;
		_timesIdle = _idleTime;
		_groundTime = kPlantLandDelay;
		_airTime = kPlantReleaseSeconds;
		_plantDrop = 0.f;
		_aimBlend = 1.f;
		_count = 0;
		_wasFalling = false;
		_recoveryTime = -1.f;
		_standTime = 0.f;
		_fidgetTime = -1.f;
		_fidgetLeaving = false;
		_moveState = EMoveState::Idle;
		_moveDirection = 0;
		_pivotAccelDirection[0] = _pivotAccelDirection[1] = 0.0;
		_beforePivot = false;
		_followClip = -1;
		// 판 시작 = 새 몸 — 몽타주 없음 · 아직 안 쏨(제 판단. Lyra 의 새 폰은 쏜 뒤 시간이 0 에서 셀 수 있는데 기록에서 보지 않았다 — 미확인)
		_montageCount = 0;
		_poseMontageCount = 0;
		_disableLeftHand = 0.f;
		_handLeftAlpha = 1.f;
		_handFresh = false;
		_timeSinceFired = kNeverFiredSeconds;
		_overrideWeight = 0.f;
		_firing = false;
	}

	void CAnimState::StepSpan(int clip, int step, float& outStart, float& outEnd) const
	{
		const std::vector<float>& table = _stepTimes[clip];
		const size_t k = (size_t)step % table.size();
		outStart = table[k];
		outEnd = k + 1 < table.size() ? table[k + 1] : table[0] + _lengths[clip];
	}

	void CAnimState::UpdateIdleBreak(bool still, float deltaSeconds)
	{
		if (_fidgetTime >= 0.f)
		{
			_fidgetTime += deltaSeconds;
			if (!still) _fidgetLeaving = true;
			if (_fidgetTime >= _fidgetLength || (_fidgetLeaving && _weights[kClipIdle] <= kZeroWeight))
			{
				_fidgetTime = -1.f;
				_fidgetLeaving = false;
				_standTime = 0.f;
			}
			return;
		}
		_standTime = still ? _standTime + deltaSeconds : 0.f;
		if (_standTime >= _stopClipSeconds + kIdleBreakDelay)
		{
			_fidgetTime = 0.f;
			_standTime = 0.f;
		}
	}

	void CAnimState::PushJump(int clip, float fadeSeconds)
	{
		if (_count == 0)
		{
			_stack[0] = Layer();
			_count = 1;
		}
		if (_count == kAnimLayerMax)
		{
			for (int i = 1; i < kAnimLayerMax; ++i) _stack[i - 1] = _stack[i];
			--_count;
			++_stackOverflows;
		}
		Layer& layer = _stack[_count++];
		layer.clip = clip;
		layer.time = 0.f;
		layer.fade = fadeSeconds > 0.f ? 0.f : 1.f;
		layer.fadeSeconds = fadeSeconds;
	}

	float CAnimState::TimeFromDistance(int transition, float value) const
	{
		const std::vector<float>& table = *_transitionDistances[transition];
		const int num = (int)table.size();
		if (num < 2) return 0.f;
		int first = 1;
		int count = num - 2;
		while (count > 0)
		{
			const int half = count / 2;
			const int middle = first + half;
			if (value > table[(size_t)middle])
			{
				first = middle + 1;
				count -= half + 1;
			}
			else
			{
				count = half;
			}
		}
		const float span = table[(size_t)first] - table[(size_t)first - 1];
		const float alpha = std::abs(span) > kSmallNumber ? Clamp((value - table[(size_t)first - 1]) / span, 0.f, 1.f) : 0.f;
		return std::min(((float)(first - 1) + alpha) / kTransitionTableHz, _stackLengths[kAnimJumpClips + transition]);
	}

	float CAnimState::DistanceAtTime(int transition, float time) const
	{
		const std::vector<float>& table = *_transitionDistances[transition];
		if (table.empty()) return 0.f;
		const float position = Clamp(time * kTransitionTableHz, 0.f, (float)(table.size() - 1));
		const int index = std::min((int)std::floor(position), (int)table.size() - 2);
		if (index < 0) return table[0];
		const float a = table[(size_t)index];
		const float b = table[(size_t)index + 1];
		return a + (position - (float)index) * (b - a);
	}

	float CAnimState::TransitionClipTime(const Layer& layer, float deltaSeconds) const
	{
		const int transition = layer.clip - kAnimJumpClips;
		const auto advanceByTravel = [&]()
		{
			const float target = TimeFromDistance(transition, DistanceAtTime(transition, layer.time) + _frameTravel);
			const float rate = deltaSeconds > 0.f ? Clamp((target - layer.time) / deltaSeconds, kDistanceRateMin, kDistanceRateMax) : 1.f;
			return layer.time + rate * deltaSeconds;
		};
		switch (_moveState)
		{
		case EMoveState::Start:
			return advanceByTravel();
		case EMoveState::Stop:
			return _stopDistance > 0.f ? std::max(layer.time, TimeFromDistance(transition, -_stopDistance)) : layer.time + deltaSeconds;
		case EMoveState::Pivot:
			return _beforePivot ? std::max(layer.time, TimeFromDistance(transition, -_pivotDistance)) : advanceByTravel();
		case EMoveState::Idle:
		case EMoveState::Cycle:
		case EMoveState::Air:
			break;
		}
		return layer.time + deltaSeconds;
	}

	bool CAnimState::HasJumpLayer() const
	{
		for (int i = 0; i < _count; ++i)
		{
			if (_stack[i].clip != kJumpNone && _stack[i].clip < kAnimJumpClips) return true;
		}
		return false;
	}

	void CAnimState::EnterTransition(EMoveState state, int direction, float fadeSeconds)
	{
		const int kind = state == EMoveState::Start ? kTransitionStart : state == EMoveState::Stop ? kTransitionStop : kTransitionPivot;
		_moveState = state;
		_moveDirection = direction;
		_followClip = -1;
		PushJump(kAnimJumpClips + kind * 4 + direction, fadeSeconds);
	}

	void CAnimState::EnterLocomotion(EMoveState state, float fadeSeconds)
	{
		_followClip = -1;
		if (state == EMoveState::Cycle && _count > 0 && _stack[_count - 1].clip >= kAnimJumpClips)
		{
			const Layer& top = _stack[_count - 1];
			const int transition = top.clip - kAnimJumpClips;
			const int marker = StepMarkerAt(transition, top.time);
			if (marker != -1)
			{
				_step = (*_transitionMarkers[transition])[(size_t)marker - 1].left ? 0 : _stepCycle - 1;
				_followClip = top.clip;
				_followBaseStep = _step;
				_followBaseMarker = marker;
			}
		}
		if (state == EMoveState::Idle) _idleTime = 0.f;
		_moveState = state;
		PushJump(kJumpNone, fadeSeconds);
	}

	int CAnimState::StepMarkerAt(int transition, float time) const
	{
		const std::vector<ClipMarker>& markers = *_transitionMarkers[transition];
		for (size_t k = 1; k < markers.size(); ++k)
		{
			if (time >= markers[k - 1].time && time < markers[k].time) return (int)k;
		}
		return -1;
	}

	bool CAnimState::FollowStepsOfTransition(float deltaSeconds)
	{
		if (_followClip == -1) return false;
		float layers[kAnimLayerMax] = {};
		JumpWeights(layers);
		for (int i = 0; i < _count; ++i)
		{
			if (_stack[i].clip != _followClip) continue;
			if (layers[i] < kStepLeaderWeight) break;
			const int transition = _followClip - kAnimJumpClips;
			const float time = _stack[i].time + deltaSeconds;
			const int marker = StepMarkerAt(transition, time);
			if (marker == -1) break;
			const std::vector<ClipMarker>& markers = *_transitionMarkers[transition];
			_step = (_followBaseStep + marker - _followBaseMarker) % _stepCycle;
			_stepFrac = Clamp((time - markers[(size_t)marker - 1].time) / std::max(markers[(size_t)marker].time - markers[(size_t)marker - 1].time, 0.001f), 0.f, 1.f);
			return true;
		}
		_followClip = -1;
		return false;
	}

	float CAnimState::PredictStopDistance(float speed) const
	{
		const float friction = std::max(0.f, (_separateBraking ? _brakingFriction : _groundFriction) * std::max(0.f, _brakingFrictionFactor));
		const float braking = std::max(0.f, _brakingDecelerationWalking);
		const float divisor = friction * speed + braking;
		if (divisor <= 0.f) return 0.f;
		const float time = speed / divisor;
		return std::max(0.f, speed * time + 0.5f * (-friction * speed - braking) * time * time);
	}

	float CAnimState::PredictPivotDistance(const double acceleration[3], const double velocity[3]) const
	{
		const float size = (float)std::sqrt(acceleration[0] * acceleration[0] + acceleration[1] * acceleration[1]);
		double direction[2] = {};
		if (size > kSmallNumber)
		{
			const float oneOver = 1.0f / size;
			direction[0] = acceleration[0] * oneOver;
			direction[1] = acceleration[1] * oneOver;
		}
		const float along = (float)(velocity[0] * direction[0] + velocity[1] * direction[1]);
		if (along >= 0.f) return 0.f;
		const float speedAlong = -along;
		const float time = speedAlong / (size + 2.f * speedAlong * _groundFriction);
		const double speed2D = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
		const double forceX = acceleration[0] - (velocity[0] - direction[0] * speed2D) * _groundFriction;
		const double forceY = acceleration[1] - (velocity[1] - direction[1] * speed2D) * _groundFriction;
		const double x = velocity[0] * time + 0.5 * forceX * time * time;
		const double y = velocity[1] * time + 0.5 * forceY * time * time;
		return (float)std::sqrt(x * x + y * y);
	}

	void CAnimState::UpdateTransitions(const AnimMoveInput& input, float deltaSeconds)
	{
		if (input.falling)
		{
			_moveState = EMoveState::Air;
			return;
		}
		const bool accelerating = input.accelerating;
		if (_moveState == EMoveState::Air) _moveState = accelerating ? EMoveState::Cycle : EMoveState::Idle;

		const double* velocity = input.velocity;
		const double* acceleration = input.acceleration;
		const float speed = (float)std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
		_frameTravel = speed * deltaSeconds;

		// 엔진 FVector2D::GetSafeNormal
		double accelDirection[2] = {};
		const double squareSum = acceleration[0] * acceleration[0] + acceleration[1] * acceleration[1];
		if (squareSum > (double)kSmallNumber)
		{
			const double scale = 1.0 / std::sqrt(squareSum);
			accelDirection[0] = acceleration[0] * scale;
			accelDirection[1] = acceleration[1] * scale;
		}

		_beforePivot = accelerating && velocity[0] * accelDirection[0] + velocity[1] * accelDirection[1] < 0.0;
		_stopDistance = PredictStopDistance(speed);
		_pivotDistance = _beforePivot ? PredictPivotDistance(acceleration, velocity) : 0.f;

		const bool pivotNeeded = _beforePivot && speed > kPivotMinSpeed;
		const float topTime = _count > 0 ? _stack[_count - 1].time : 0.f;
		const auto enterPivot = [&]()
		{
			_pivotAccelDirection[0] = accelDirection[0];
			_pivotAccelDirection[1] = accelDirection[1];
			EnterTransition(EMoveState::Pivot, CardinalFromAngle(AngleOf(velocity)), kToPivotSeconds);
		};
		const auto enterStop = [&]() { EnterTransition(EMoveState::Stop, CardinalFromAngle(AngleOf(velocity)), kToStopSeconds); };
		switch (_moveState)
		{
		case EMoveState::Idle:
			if (accelerating) EnterTransition(EMoveState::Start, CardinalFromAngle(AngleOf(acceleration)), kToStartSeconds);
			break;
		case EMoveState::Start:
			if (!accelerating && speed < 1.f)
			{
				_moveState = EMoveState::Idle;
				PushJump(kJumpNone, kToStartSeconds);
			}
			else if (!accelerating) enterStop();
			else if (pivotNeeded) enterPivot();
			else if (topTime >= _exitTimes[kTransitionStart][_moveDirection]) EnterLocomotion(EMoveState::Cycle, kStartToCycleSeconds);
			break;
		case EMoveState::Cycle:
			if (!accelerating) enterStop();
			else if (pivotNeeded) enterPivot();
			break;
		case EMoveState::Stop:
			if (accelerating) EnterTransition(EMoveState::Start, CardinalFromAngle(AngleOf(acceleration)), kStopToStartSeconds);
			else if (speed < 1.f && topTime >= _exitTimes[kTransitionStop][_moveDirection]) EnterLocomotion(EMoveState::Idle, kStopToIdleSeconds);
			break;
		case EMoveState::Pivot:
			if (!accelerating) enterStop();
			else if (pivotNeeded && accelDirection[0] * _pivotAccelDirection[0] + accelDirection[1] * _pivotAccelDirection[1] < 0.0) enterPivot();
			else if (topTime >= _exitTimes[kTransitionPivot][_moveDirection]) EnterLocomotion(EMoveState::Cycle, kPivotToCycleSeconds);
			break;
		case EMoveState::Air:
			break;
		}
	}

	void CAnimState::JumpWeights(float out[kAnimLayerMax]) const
	{
		float remain = 1.f;
		for (int i = _count - 1; i >= 0; --i)
		{
			const float alpha = i == 0 ? 1.f : Smooth(_stack[i].fade);
			out[i] = remain * alpha;
			remain -= out[i];
		}
	}

	float CAnimState::LandTimeFromDistance(float groundDistance) const
	{
		const std::vector<float>& table = *_landDistances;
		const int num = (int)table.size();
		if (num == 0 || groundDistance >= table[0]) return 0.f;
		for (int k = 1; k < num; ++k)
		{
			if (table[(size_t)k] <= groundDistance)
			{
				const float span = table[(size_t)k - 1] - table[(size_t)k];
				const float frac = span > kSmallNumber ? (table[(size_t)k - 1] - groundDistance) / span : 1.f;
				return std::min(((float)(k - 1) + frac) / kLandTableHz, _stackLengths[kJumpLand]);
			}
		}
		return _stackLengths[kJumpLand];
	}

	void CAnimState::UpdateJump(bool falling, bool accelerating, float velocityZ, float timeToApex, float groundDistance, float deltaSeconds)
	{
		const std::vector<float>& land = *_landDistances;
		const int topClip = _count > 0 ? _stack[_count - 1].clip : kJumpNone;
		const float topTime = _count > 0 ? _stack[_count - 1].time : 0.f;
		if (falling && !_wasFalling)
		{
			PushJump(velocityZ > 0.f ? kJumpStart : kJumpFallLoop, kJumpBlendSeconds);
		}
		else if (falling)
		{
			switch (topClip)
			{
			case kJumpStart:
				if (topTime >= _stackLengths[kJumpStart] - kKindaSmall) PushJump(kJumpStartLoop, 0.f);
				break;
			case kJumpStartLoop:
				if (timeToApex < kApexLeadSeconds) PushJump(kJumpApex, kJumpBlendSeconds);
				break;
			case kJumpApex:
				if (_stackLengths[kJumpApex] - topTime < kApexExitRemaining) PushJump(kJumpFallLoop, kJumpBlendSeconds);
				break;
			case kJumpFallLoop:
				if (!land.empty() && groundDistance < land[0]) PushJump(kJumpLand, kLandFadeSeconds);
				break;
			default:
				break;
			}
		}
		else if (_wasFalling)
		{
			PushJump(kJumpNone, accelerating ? kLandedMoveSeconds : kLandedStandSeconds);
			_recoveryTime = 0.f;
			_idleTime = 0.f;
		}
		_wasFalling = falling;

		for (int i = 0; i < _count; ++i)
		{
			Layer& layer = _stack[i];
			if (i == _count - 1 && layer.clip >= kAnimJumpClips) layer.time = TransitionClipTime(layer, deltaSeconds);
			else layer.time = layer.clip == kJumpLand && !land.empty() ? std::max(layer.time, LandTimeFromDistance(groundDistance)) : layer.time + deltaSeconds;
			if (layer.fadeSeconds > 0.f) layer.fade = std::min(layer.fade + deltaSeconds / layer.fadeSeconds, 1.f);
		}
		// 다 덮인 아래 상태는 버린다. 걷기 · 서기만 남으면 점프 쪽을 끈다
		for (int i = _count - 1; i > 0; --i)
		{
			if (_stack[i].fade < 1.f) continue;
			for (int j = i; j < _count; ++j) _stack[j - i] = _stack[j];
			_count -= i;
			break;
		}
		if (_count == 1 && _stack[0].clip == kJumpNone) _count = 0;

		if (_recoveryTime >= 0.f)
		{
			_recoveryTime += deltaSeconds;
			if (_recoveryTime >= _recoveryLength) _recoveryTime = -1.f;
		}
	}

	void CAnimState::Update(const AnimMoveInput& input)
	{
		const float deltaSeconds = input.deltaSeconds;
		const double* velocity = input.velocity;
		const float speed = (float)std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1]);

		float targets[kAnimWalkClips] = {};
		const float moveAlpha = Clamp(speed / kMoveFullSpeed, 0.f, 1.f);
		targets[kClipIdle] = 1.f - moveAlpha;
		if (speed > 1.f)
		{
			// 0 = 앞, +90 = 오른쪽, ±180 = 뒤, -90 = 왼쪽 — 이웃한 두 방향 클립을 각도로 나눈다
			const float angle = AngleOf(velocity);
			const float value = std::abs(angle);
			const int side = angle >= 0.f ? kClipRight : kClipLeft;
			if (value <= 90.f)
			{
				targets[kClipFwd] = (1.f - value / 90.f) * moveAlpha;
				targets[side] = (value / 90.f) * moveAlpha;
			}
			else
			{
				targets[side] = (1.f - (value - 90.f) / 90.f) * moveAlpha;
				targets[kClipBwd] = ((value - 90.f) / 90.f) * moveAlpha;
			}
		}

		const float follow = 1.f - std::exp(-deltaSeconds / kBlendSeconds);
		float sum = 0.f;
		for (int i = 0; i < kAnimWalkClips; ++i)
		{
			_weights[i] += (targets[i] - _weights[i]) * follow;
			sum += _weights[i];
		}
		if (sum > kSmallNumber)
		{
			for (float& weight : _weights) weight /= sum;
		}

		// 달리기 진행 — 클립의 원래 속도와 실제 속도의 비로 재생하고, 발걸음 단위로 함께 넘어가 섞이는 두 클립의 발이 같은 때 닿게 한다
		float directionWeight = 0.f;
		float rootSpeed = 0.f;
		float stepSeconds = 0.f;
		for (int i = kClipFwd; i <= kClipRight; ++i)
		{
			float start, end;
			StepSpan(i, _step, start, end);
			directionWeight += _weights[i];
			rootSpeed += _weights[i] * _rootSpeeds[i];
			stepSeconds += _weights[i] * (end - start);
		}
		const bool stepsFollow = FollowStepsOfTransition(deltaSeconds);
		if (!stepsFollow && directionWeight > 0.001f && stepSeconds > kSmallNumber)
		{
			rootSpeed /= directionWeight;
			stepSeconds /= directionWeight;
			const float playRate = rootSpeed > 1.f ? Clamp(speed / rootSpeed, kMinPlayRate, kMaxPlayRate) : 1.f;
			_stepFrac += deltaSeconds * playRate / stepSeconds;
			while (_stepFrac >= 1.f)
			{
				_stepFrac -= 1.f;
				_step = (_step + 1) % _stepCycle;
			}
		}
		_idleTime = _lengths[kClipIdle] > 0.f ? std::fmod(_idleTime + deltaSeconds, _lengths[kClipIdle]) : 0.f;

		_times[kClipIdle] = _idleTime;
		_timesStep = _step;
		_timesStepFrac = _stepFrac;
		_timesIdle = _idleTime;
		for (int i = kClipFwd; i <= kClipRight; ++i)
		{
			float start, end;
			StepSpan(i, _step, start, end);
			_times[i] = std::fmod(start + _stepFrac * (end - start), _lengths[i]);
		}

		_pitch = input.pitch;
		const bool falling = input.falling;
		const bool accelerating = input.accelerating;
		UpdateFiring(accelerating, deltaSeconds);
		const float timeToApex = velocity[2] > 0.0 && _gravity > kSmallNumber ? (float)velocity[2] / _gravity : 0.f;
		const float groundDistance = input.groundDistance;

		// 발 디딤 내림 — 땅에선 잰 거리만큼, 뜬 직후엔 마지막으로 잰 거리만큼 내린다
		if (falling)
		{
			_airTime = _wasFalling ? _airTime + deltaSeconds : 0.f;
			_groundTime = 0.f;
		}
		else
		{
			_groundTime = _wasFalling ? 0.f : _groundTime + deltaSeconds;
			if (input.hasFloorGap) _plantDrop = input.plantGap;
		}
		const bool plant = falling ? _airTime < kPlantReleaseSeconds - kPlantTimeSlack : _groundTime >= kPlantLandDelay - kPlantTimeSlack;
		_plantOffset[0] = 0.f;
		_plantOffset[1] = 0.f;
		_plantOffset[2] = plant ? -_plantDrop : 0.f;

		UpdateTransitions(input, deltaSeconds);
		UpdateJump(falling, accelerating, (float)velocity[2], timeToApex, groundDistance, deltaSeconds);
		UpdateIdleBreak(!falling && !accelerating && speed < 1.f && !HasJumpLayer(), deltaSeconds);

		// 왼손 비중은 앞 자세의 곡선으로 — 기록에서 HandIK_Left_Alpha 가 곡선보다 한 프레임 늦다(실측). 그다음 몽타주를 넘기고 이번 자세 목록을 뜬다
		_handLeftAlpha = _handFresh ? 1.f : Clamp(1.f - _disableLeftHand, 0.f, 1.f);
		_handFresh = false;
		UpdateMontages(deltaSeconds);
		SnapshotMontages();
	}

	void CAnimState::ToWire(wire::AnimStateWire& out) const
	{
		for (int i = 0; i < kAnimWalkClips; ++i) out.weights[i] = ToWireByte(_weights[i]);
		out.step = (uint8_t)_timesStep;
		out.stepFrac = (uint16_t)std::lround(Clamp(_timesStepFrac, 0.f, 1.f) * 65535.f);
		out.idleTime = ToWireTime(_timesIdle);
		out.aimBlend = ToWireByte(_aimBlend);
		out.fidgetTime = _fidgetTime < 0.f ? wire::kAnimTimeNone : ToWireFidget(_fidgetTime);
		out.recoveryTime = _recoveryTime < 0.f ? wire::kAnimTimeNone : ToWireTime(_recoveryTime);
		out.plantDrop = (int16_t)std::lround(Clamp(-_plantOffset[2], -300.f, 300.f) * kWirePlantScale);
		out.layerCount = (uint8_t)_count;
		for (int i = 0; i < _count; ++i)
		{
			out.layers[i].clip = (int8_t)_stack[i].clip;
			out.layers[i].time = ToWireTime(_stack[i].time);
			out.layers[i].fade = ToWireByte(_stack[i].fade);
		}
		// 몽타주 · 덮기 · 왼손 — 자세를 만든 값(사건 앞 목록, ToPoseInput 과 같은 것)을 싣는다. 비중이 0 칸으로 반올림되면 받는 쪽에서 비중 0 이라 자세를 안 바꾸므로 뺀다(D51)
		out.overrideWeight = ToWireByte(_overrideWeight);
		out.handLeftAlpha = ToWireByte(_handLeftAlpha);
		out.montageCount = 0;
		for (int i = 0; i < _poseMontageCount; ++i)
		{
			const AnimMontageSlot& slot = _poseMontages[i];
			const uint8_t weight = ToWireByte(slot.weight);
			if (weight == 0) continue;
			wire::AnimMontageWire& cell = out.montages[out.montageCount++];
			cell.montage = (uint8_t)(((uint8_t)slot.bundle << wire::kAnimMontageKindBits) | slot.kind);
			cell.time = ToWireTime(slot.position);
			cell.weight = weight;
		}
	}

	void CAnimState::FromWire(const wire::AnimStateWire& in, float pitch, AnimPoseInput& out) const
	{
		float sum = 0.f;
		for (int i = 0; i < kAnimWalkClips; ++i)
		{
			out.weights[i] = (float)in.weights[i] / 255.f;
			sum += out.weights[i];
		}
		if (sum > kSmallNumber)
		{
			for (float& weight : out.weights) weight /= sum;
		}
		else
		{
			out.weights[kClipIdle] = 1.f;
		}

		out.times[kClipIdle] = FromWireTime(in.idleTime);
		const float frac = (float)in.stepFrac / 65535.f;
		for (int i = kClipFwd; i <= kClipRight; ++i)
		{
			float start, end;
			StepSpan(i, in.step, start, end);
			out.times[i] = std::fmod(start + frac * (end - start), _lengths[i]);
		}

		out.fidgetTime = in.fidgetTime == wire::kAnimTimeNone ? -1.f : FromWireFidget(in.fidgetTime);
		out.recoveryTime = in.recoveryTime == wire::kAnimTimeNone ? -1.f : FromWireTime(in.recoveryTime);
		out.aimBlend = (float)in.aimBlend / 255.f;
		out.pitch = NormalizePitch(pitch);
		out.plantOffset[0] = 0.f;
		out.plantOffset[1] = 0.f;
		out.plantOffset[2] = -(float)in.plantDrop / kWirePlantScale;
		out.layerCount = std::min((int)in.layerCount, kAnimLayerMax);
		for (int i = 0; i < out.layerCount; ++i)
		{
			out.layers[i].clip = in.layers[i].clip;
			out.layers[i].time = FromWireTime(in.layers[i].time);
			out.layers[i].fade = (float)in.layers[i].fade / 255.f;
		}
		// 몽타주 칸은 묶음 번호 · 종류로 찾는다 — 없는 몽타주면 자세 계산(CAnimPoseEvaluator)이 그 칸을 건너뛰고 센다(montageMisses)
		out.montageCount = std::min((int)in.montageCount, kAnimMontageMax);
		for (int i = 0; i < out.montageCount; ++i)
		{
			const wire::AnimMontageWire& cell = in.montages[i];
			out.montages[i].bundle = (int8_t)(cell.montage >> wire::kAnimMontageKindBits);
			out.montages[i].kind = (uint8_t)(cell.montage & kWireMontageKindMask);
			out.montages[i].position = FromWireTime(cell.time);
			out.montages[i].weight = (float)cell.weight / 255.f;
		}
		for (int i = out.montageCount; i < kAnimMontageMax; ++i) out.montages[i] = AnimMontageSlot{};
		out.overrideWeight = (float)in.overrideWeight / 255.f;
		out.handLeftAlpha = (float)in.handLeftAlpha / 255.f;
	}

	void CAnimState::ToPoseInput(AnimPoseInput& out) const
	{
		for (int i = 0; i < kAnimWalkClips; ++i)
		{
			out.weights[i] = _weights[i];
			out.times[i] = _times[i];
		}
		out.fidgetTime = _fidgetTime;
		out.layerCount = _count;
		for (int i = 0; i < kAnimLayerMax; ++i)
		{
			out.layers[i].clip = (int16_t)_stack[i].clip;
			out.layers[i].time = _stack[i].time;
			out.layers[i].fade = _stack[i].fade;
		}
		out.recoveryTime = _recoveryTime;
		out.aimBlend = _aimBlend;
		out.pitch = _pitch;
		for (int c = 0; c < 3; ++c) out.plantOffset[c] = _plantOffset[c];
		out.montageCount = _poseMontageCount;
		for (int i = 0; i < kAnimMontageMax; ++i) out.montages[i] = i < _poseMontageCount ? _poseMontages[i] : AnimMontageSlot{};
		out.overrideWeight = _overrideWeight;
		out.handLeftAlpha = _handLeftAlpha;
	}
}
