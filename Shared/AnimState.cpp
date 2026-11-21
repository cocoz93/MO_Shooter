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
		constexpr float kPlantAlphaSeconds = 0.2f;		// 발 붙이기 장치 켜고 끄기 — ABP 컨트롤 릭 노드 AlphaBoolBlend 넣기 · 빼기 0.2초 선형(weapons_l2a.txt:2989, 원본 기준)
		// 장치 셈(L-6-c-2) — CR_Mannequin_FootPlant 그래프 값(원본 기준, 덤프 길은 진행 파일 L-2-a)
		constexpr float kPlantAlphaRelevant = 0.00001f;	// 알파가 이 값 이하면 장치를 안 돌린다 — 스프링이 멈췄다 이어 간다(AnimNode_ControlRigBase.cpp:197 IsRelevant · ZERO_ANIMWEIGHT_THRESH)
		constexpr float kPlantTraceReach = 55.f;		// 발마다 구 쓸기 발 +50 → 발 −50 · 반지름 5 — 평지면 발 높이 − 55 가 땅 이하일 때 맞는다(ProcessFootTrace)
		constexpr float kPlantSpringHz = 2.5f;			// 골반 SpringInterpV2 세기(Hz) · 감쇠 1 · bUseCurrentInput
		constexpr float kPlantSpringTargetRate = 0.2f;	// 같은 노드의 목표 속도 몫(TargetVelocityAmount)
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
		// 조준이 바뀌어 출발 · 돌아서기 → 달리기 반복, 멈춤 → 서기로 빠질 때의 섞기(D60 3) — 그 전환 규칙에 시간이 안 적혀 엔진 기본값 0.2초(AnimStateTransitionNode.cpp:84, 원본 기준).
		// Lyra 기록에서 나가는 출발 클립 비중이 10프레임쯤에 0 이 된다(animcmp_ads_rifle_probe.csv 3506~3516, 실측)
		constexpr float kAdsChangeSeconds = 0.2f;
		// 서기 안 앉기 · 일어서기 전환에서 서기로 돌아가는 남은 시간(D62 4) — 엔진 자동 전환 규칙(AnimNode_StateMachine.cpp:850-854 「남은 시간 ≤ 전환 시간」, 원본 기준).
		// 길이 0.733 · 0.900 · 0.733 인 세 클립이 다 시각 0.483 · 0.650 · 0.483 을 마지막으로 서기로 갔다(animcmp_crouch_<무기>_probe.csv c_on · c_off, 실측).
		// 덤 1e-4 = 1/60초 간격 시각의 float 더하기 어긋남이 문턱을 넘나들지 않게(한 프레임 앞 0.2667 과는 0.0166 떨어져 있다, 제 판단)
		constexpr float kStanceExitRemaining = 0.25f;
		constexpr float kStanceExitSlack = 1.e-4f;
		// 빠지는 시각 표(kBundleTimings)는 Lyra 실행값을 소수 셋째 자리로 반올림한 값이다 — 반 칸(0.0005)만큼 이른 시각도 그 프레임으로 본다. 덤 1e-4 는 kStanceExitSlack 과 같은 까닭.
		// 없으면 소총 멈춤 → 서기가 한 프레임 늦었다(우리 클립 시각 2.17954 < 표 2.180, 조준 기록 「그 밖」 12줄 — content-add-progress.md 미결 (가))
		constexpr float kExitTimeSlack = 0.0005f + 1.e-4f;
		constexpr float kPivotMinSpeed = 10.f;
		constexpr float kDistanceRateMin = 0.25f;
		constexpr float kDistanceRateMax = 5.f;

		// 무기 묶음마다 다른 값 — 전환 클립 길이가 무기마다 달라 빠지는 시각도 다르다(소총 멈춤 1.712 를 권총에 쓰면 길이 0.675 클립에서 못 빠진다).
		// 방향 차례 = 앞 · 뒤 · 왼 · 오른. 값 = 걸음 상태 기계 칸이 바뀌는 프레임의 나가는 클립 시각(Lyra 실행값).
		// - 소총 = MOBSimpleAnimInstance.cpp 의 START_EXIT_TIMES · STOP_EXIT_TIMES · PIVOT_EXIT_TIMES · STOP_CLIP_SECONDS
		// - 권총 = 같은 방법으로 9단계 1-b 기록에서 잰 값. 오른쪽 돌아서기 1.822 만 계산값(그 클립의 TransitionToLocomotion 표시 시작 1.800 + 0.022, 잰 일곱의 평균 차이).
		//   멈춘 뒤 서기 0.35 는 소총처럼 앞 방향 기준으로 고른 값
		// - 샷건 = 소총 몸이라 소총 값(D45)
		// - 조준 전환(D60 3) = 같은 방법으로 컨텐츠 C-3 기록 animcmp_ads_<무기>_probe.csv 에서 잰 값(/c/tmp/mob/c3/exit_ads.py). 같은 방향을 2~3번 잰 값이 다 같다.
		//   조준을 바꿔 빠진 것(start_toggle · piv_toggle 구간)은 뺐다
		// - 앉은 전환(D62 3) = 같은 방법으로 컨텐츠 D-3-a 기록 animcmp_crouch_<무기>_probe.csv 에서 잰 값(/c/tmp/mob/d3/exit_crouch.py). 같은 방향을 1~3번 잰 값이 다 같다.
		//   앉음 · 조준을 바꿔 빠진 것(stop_toggle · c_ads_off)과 돌아서기 → 멈춤(stop_end)은 뺐다. 샷건은 소총과 프레임까지 같다
		// - 앉아 쏜 뒤 들기 = Lyra 무기 층 RaiseWeaponAfterFiringWhenCrouched — 소총 · 샷건 참 · 권총 거짓(무기 층 기본값 원본 기준 + 기록 권총 앉은 덮기 자세 0프레임, 실측. D62 5)
		struct BundleTiming
		{
			const char* bundle;
			float stopClipSeconds;			// 멈춘 뒤 서기 상태가 될 때까지 — 대기 몸짓 시계의 출발점
			float exitTimes[3][4];			// [출발 · 멈춤 · 돌아서기][방향] — 전환 클립이 이 시각을 지나면 달리기 · 서기로 빠진다
			float adsExitTimes[3][4];		// 조준 전환 클립의 같은 표
			float crouchExitTimes[3][4];	// 앉은 전환 클립의 같은 표
			bool raiseWhenCrouched;
		};
		constexpr BundleTiming kBundleTimings[] = {
			{ "rifle", 1.35f, { { 0.791f, 1.165f, 1.185f, 1.122f }, { 1.712f, 2.046f, 2.180f, 2.277f }, { 1.344f, 1.591f, 1.565f, 1.358f } },
			  { { 1.025f, 0.852f, 0.719f, 0.630f }, { 1.782f, 1.948f, 1.944f, 1.446f }, { 1.005f, 1.193f, 1.232f, 1.148f } },
			  { { 1.059f, 1.252f, 1.091f, 1.151f }, { 1.479f, 1.281f, 1.280f, 1.044f }, { 1.839f, 1.831f, 1.519f, 1.629f } }, true },
			{ "pistol", 0.35f, { { 0.533f, 0.424f, 1.359f, 1.023f }, { 0.675f, 1.016f, 1.016f, 1.008f }, { 1.433f, 1.526f, 1.734f, 1.822f } },
			  { { 0.864f, 0.894f, 0.745f, 0.726f }, { 1.110f, 0.936f, 1.076f, 0.691f }, { 1.851f, 2.021f, 1.798f, 2.301f } },
			  { { 1.226f, 1.156f, 1.100f, 1.167f }, { 0.727f, 0.692f, 0.941f, 0.911f }, { 1.699f, 1.871f, 1.877f, 2.113f } }, false },
			{ "shotgun", 1.35f, { { 0.791f, 1.165f, 1.185f, 1.122f }, { 1.712f, 2.046f, 2.180f, 2.277f }, { 1.344f, 1.591f, 1.565f, 1.358f } },
			  { { 1.025f, 0.852f, 0.719f, 0.630f }, { 1.782f, 1.948f, 1.944f, 1.446f }, { 1.005f, 1.193f, 1.232f, 1.148f } },
			  { { 1.059f, 1.252f, 1.091f, 1.151f }, { 1.479f, 1.281f, 1.280f, 1.044f }, { 1.839f, 1.831f, 1.519f, 1.629f } }, true },
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

		// 서기 시계 → 한 묶음의 서기 클립 시각. 시계는 지금 조준 쪽 길이로 돌아 다른 쪽 길이를 넘을 수 있다(D60 2).
		// 길이 안이면 그대로 둔다 — 패킷 반올림으로 길이를 1/12000초 넘은 값까지 0 근처로 접히지 않게(조준 전 판과 같은 수)
		float WrapIdle(float seconds, float length) { return seconds <= length ? seconds : std::fmod(seconds, length); }

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
		blended = begin + (desired - begin) * BlendOptionCurve(option, alpha);
	}

	bool CAnimState::MontageBlend::IsComplete() const
	{
		return begin + (desired - begin) * BlendOptionCurve(option, 1.f) == blended;
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

		// 같은 무리를 새 몽타주의 넣기 섞기로 끈다 — 뒤 칸부터(엔진 StopAllMontagesByGroupName).
		// 비중 표는 새 몽타주의 넣기 비중 표(엔진 BlendProfileIn)인데 Lyra 몽타주 20개가 다 없다(기록 animcmp_*_montages.csv profileIn = None) → -1
		if ((_montageTest & kMontageTestNoGroup) == 0)
		{
			for (int i = _montageCount - 1; i >= 0; --i)
			{
				if (_montages[i].montage->group == montage->group) StopMontage(_montages[i], inOption, montage->blendIn, -1);
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
			StopMontage(instance, outOption, montage->blendOut, montage->blendOutProfile);		// 엔진 Montage_Stop — 몽타주의 빼기 비중 표(AnimInstance.cpp:2908)
			return true;
		}
		return false;
	}

	bool CAnimState::PlayFireMontage(float playRate, int bundle, bool& cancelledReload)
	{
		return PlayMontageCancellingReload(MONTAGE_FIRE, playRate, bundle, cancelledReload);
	}

	bool CAnimState::PlayMontageCancellingReload(uint8_t kind, float playRate, int bundle, bool& cancelledReload)
	{
		cancelledReload = false;
		for (int i = 0; i < _montageCount; ++i)
		{
			const MontageInstance& instance = _montages[i];
			if (instance.montage->kind != MONTAGE_RELOAD || instance.blend.desired == 0.f) continue;
			cancelledReload = StopMontageKind(MONTAGE_RELOAD, instance.bundle);
			break;
		}
		return PlayMontage(kind, playRate, bundle);
	}

	// 엔진 FAnimMontageInstance::Stop(AnimMontage.cpp:1558). option · blendSeconds · profile = 끄는 쪽의 섞기 — profile 은 엔진 ActiveBlendProfile 이 될 값
	// (제 빼기 = 몽타주의 빼기 비중 표, 무리 끄기 = 새 몽타주의 넣기 비중 표 = 없음)
	void CAnimState::StopMontage(MontageInstance& instance, uint8_t option, float blendSeconds, int16_t profile)
	{
		MontageBlend& blend = instance.blend;
		// 엔진은 비중 표 뼈의 시작 비중을 끄기 직전 선형 진행(BlendStartAlpha)으로 잡는데 와이어엔 그 칸이 없어 자세 계산은 1 로 본다(L-6-b · D130).
		// 진행이 표의 1 아래 배율 중 가장 큰 값(FastFeet 0.5) 이상이면 표의 뼈마다 시작 비중이 1 로 같고, 아래면 다르다 → 다른 칸만 센다
		const int16_t active = blend.desired != 0.f ? profile : instance.profile;
		if (active >= 0 && (blend.desired != 0.f || blendSeconds < blend.blendTime) && blend.alpha < ProfileMaxFactor(active)) ++_profileStartMisses;
		if (blend.desired != 0.f)
		{
			blend.option = option;
			blend.SetBlendTime(blendSeconds);
			blend.SetValueRange(blend.blended, 0.f);
			blend.Update(0.f);
			instance.profile = profile;
		}
		else if (blendSeconds < blend.blendTime)
		{
			// 이미 빠지는 중이면 더 짧은 시간으로만 바꾼다. 비중 표는 그대로(엔진도 그대로)
			blend.SetBlendTime(blendSeconds);
			blend.SetValueRange(blend.blended, 0.f);
		}
		if (blend.blendTime <= 0.f) instance.playing = false;
	}

	// 빼기 비중 표에서 1 아래 배율 중 가장 큰 값 — 시작 진행이 이 값 이상이면 표의 뼈마다 시작 비중이 1 이다(엔진 CalculateBoneWeight 의 BeginValue = 시작 진행 ÷ 배율, 1 에서 자름).
	// 시작 진행을 안 쓰는 방식(무게 배율 · 뼈 가림)은 0
	float CAnimState::ProfileMaxFactor(int profile) const
	{
		const ClipProfile& table = _clips->Profiles()[(size_t)profile];
		if (table.mode != 0) return 0.f;		// 0 = 엔진 EBlendProfileMode::TimeFactor
		float most = 0.f;
		for (float factor : table.weights)
		{
			if (factor < 1.f - kZeroWeight) most = std::max(most, factor);
		}
		return most;
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
						StopMontage(instance, outOption, customTrigger ? asset.blendOut : toEnd, asset.blendOutProfile);		// 엔진 Stop(FAlphaBlend) — 몽타주의 빼기 비중 표(AnimMontage.cpp:1672)
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
		for (int i = 0; i < _montageCount; ++i)
		{
			_montages[i].blend.Update(deltaSeconds);
			if (_montages[i].blend.remaining < 0.0001f) _montages[i].profile = -1;		// 엔진 UpdateWeight — 섞기가 끝나면 비중 표를 뗀다(AnimMontage.cpp:2048)
		}
		int kept = 0;
		for (int i = 0; i < _montageCount; ++i)
		{
			if (!AdvanceMontage(_montages[i], deltaSeconds)) continue;
			if (kept != i) _montages[kept] = _montages[i];
			++kept;
		}
		_montageCount = kept;
	}

	// 통째(FullBody) 자리 바탕 비중 = 1 − 그 자리에 줄이 있는 몽타주 비중의 합(엔진 FAnimInstanceProxy::GetSlotWeight — 덧붙임 아닌 줄만, 합이 1 을 넘으면 나눠 맞춤).
	// 바탕 비중이 ZERO_ANIMWEIGHT_THRESH 이하이면 엔진 슬롯 노드는 바탕(걸음 상태 기계 · 무기 층)을 안 넘긴다(AnimNode_Slot.cpp:79 bUpdateSource, 원본 기준).
	// 엔진은 몽타주를 먼저 넘기고 그래프를 갱신하므로, 넘기기 전 칸을 베껴 이번 갱신만큼 넘긴 비중으로 본다(실제 칸은 Update 끝 UpdateMontages 가 넘긴다)
	bool CAnimState::FullBodyCoversSource(float deltaSeconds)
	{
		if (_fullBodySlot < 0) return false;
		const std::vector<ClipSlot>& slots = _clips->Slots();
		float total = 0.f;
		float replace = 0.f;
		for (int i = 0; i < _montageCount; ++i)
		{
			MontageInstance next = _montages[i];
			next.blend.Update(deltaSeconds);
			if (!AdvanceMontage(next, deltaSeconds)) continue;
			bool onSlot = false;
			for (const MontageTrack& track : next.montage->tracks) onSlot = onSlot || track.slot == _fullBodySlot;
			if (!onSlot) continue;
			total += next.blend.blended;
			if (slots[(size_t)_fullBodySlot].mode != ESlotMode::Additive) replace += next.blend.blended;
		}
		if (total > 1.f) replace /= total;
		return 1.f - replace <= kZeroWeight;
	}

	// 엔진 상태 기계는 앞 프레임에 갱신을 못 받았으면 다시 초기화한다(AnimNode_StateMachine.cpp:385 bReinitializeOnBecomingRelevant, 원본 기준) —
	// 걸음 상태는 첫 상태(서기)로 섞기 없이 가고, 서기 클립 · 대기 몸짓 상태 기계도 처음부터다.
	// 기록: 부활 몽타주 비중이 1 아래로 내려간 첫 프레임에 멈춤 → 서기, 서기 클립 시각은 그 다음 프레임 0.033(= 0 에서 두 프레임) · 근접 권총은 7.383 → 0.033 (animcmp_ev_rifle · melee_pistol probe, 실측)
	void CAnimState::ReinitLocomotion()
	{
		_count = 0;
		_moveState = EMoveState::Idle;
		_followClip = -1;
		_beforePivot = false;
		_idleTime = 0.f;
		_fidgetTime = -1.f;
		_fidgetLeaving = false;
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
			// 빼기 진행(L-4) — 엔진은 빼기 비중 표가 걸린 몽타주(대시 넷 = FastFeet)만 빼는 동안 선형 진행으로 뼈마다 뺀다(D103).
			// 걸린 표 = 제 빼기로 빠질 때만(StopMontage 의 profile, L-6-b) — 무리 끄기로 빠지는 대시는 표 없이 몽타주 비중대로 빠진다
			slot.blendOut = instance.profile >= 0 ? instance.blend.alpha : -1.f;
		}
		_disableLeftHand = PoseCurve("DisableLHandIK");
		_disableLegIK = PoseCurve("DisableLegIK");
	}

	// 몽타주 곡선 name × 비중의 합(DisableLHandIK · DisableLegIK). 몽타주 자체 곡선과, 덧붙임이 아닌 자리에 든 줄의 클립 곡선만 더한다 —
	// 소총 재장전은 윗몸 · 윗몸 덧붙임 두 줄에 같은 곡선이 있는데, 둘 다 더하면 Lyra 기록과 1.0 까지 어긋나고 앞 줄만 더하면 0.02 안이다(3-b curve_hyp, 실측)
	// DisableLegIK 는 앞 대시 몽타주 자체 곡선 하나다(animcmp_clips8_<무기>_clips120.csv 의 MCURVE 한 줄 · 시퀀스 SCURVE 엔 없음, 원본 기준 — 줄 클립 갈래를 안 탄다)
	float CAnimState::PoseCurve(const char* name) const
	{
		float sum = 0.f;
		const std::vector<ClipSlot>& slots = _clips->Slots();
		for (int i = 0; i < _poseMontageCount; ++i)
		{
			const AnimMontageSlot& slot = _poseMontages[i];
			const ClipMontage& montage = *_clips->Bundles()[(size_t)slot.bundle].FindMontage(slot.kind);
			for (const MontageCurve& curve : montage.curves)
			{
				if (curve.name != name) continue;
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

	const char* CAnimState::MoveStateName() const
	{
		switch (_moveState)
		{
		case EMoveState::Idle: return "Idle";
		case EMoveState::Start: return "Start";
		case EMoveState::Cycle: return "Cycle";
		case EMoveState::Stop: return "Stop";
		case EMoveState::Pivot: return "Pivot";
		case EMoveState::Air: return "Air";
		}
		return "?";
	}

	int CAnimState::StanceLayer() const
	{
		for (int i = _count - 1; i >= 0; --i)
		{
			if (_stack[i].settled) continue;
			if (_stack[i].clip == kAnimStanceEntry) return 1;
			if (_stack[i].clip == kAnimStanceExit) return 2;
		}
		return 0;
	}

	// 쏜 뒤 시간 → 덮기 비중 → 조준 보정 비중(D45). Lyra 차례 = 기본 애니(쏜 뒤 시간)가 먼저, 무기 층(덮기 · 조준)이 그 값을 같은 갱신에 읽는다
	// 조준 중(D60 4) = Lyra 층의 같은 함수가 쏜 뒤 시간보다 먼저 보는 갈래 — 땅에선 덮기 0 · 조준 보정 1, 공중에선 덮기 1 · 조준 보정 1, 둘 다 보간 없이 바로.
	// 기록에서 조준한 채 뜬 프레임에 덮기 0 → 1, 내려앉은 프레임에 1 → 0 으로 바로 바뀐다(animcmp_ads_rifle_probe.csv 2908 · 2969, 실측).
	// 앉기(D62 5) = 같은 함수의 앉기 갈래(flow_layers_all.txt:2466-2600 풀이, 원본 기준) — ① (앉았고 앉아 쏜 뒤 안 듦) 또는 (서서 땅에서 조준) → 덮기 0 · 조준 보정 1
	// ② 쏜 지 0.5초 안 또는 (조준하고 앉았거나 공중) → 덮기 1 · 조준 보정 1 ③ 그 밖 = 보간. 앉지 않으면 ① · ② 가 앞 조준 갈래와 같은 값을 낸다
	void CAnimState::UpdateFiring(bool accelerating, bool falling, bool ads, bool crouched, float deltaSeconds)
	{
		_timeSinceFired = _firing ? 0.f : std::min(_timeSinceFired + deltaSeconds, kNeverFiredSeconds);
		if ((crouched && !_raiseWhenCrouched) || (!crouched && ads && !falling))
		{
			_overrideWeight = 0.f;
			_aimBlend = 1.f;
			return;
		}
		if (ads && (crouched || falling))
		{
			_overrideWeight = 1.f;
			_aimBlend = 1.f;
			return;
		}
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
		// 조준 묶음은 새 층이 들어올 때의 조준 상태 그대로 — 새 층의 서기 · 달리기는 처음부터 그 묶음 클립을 고른다(제 판단, 무기 교체 중 조준은 기록에 없다)
		_adsBlend = _ads ? 1.f : 0.f;
		_moveAds = _ads;
		// 앉기 묶음도 같은 식(제 판단, 무기 교체 중 앉음 바꿈은 기록에 없다). 공중이면 쌓기가 남는데, 그 안 서기 안 전환 칸은 새 묶음의 같은 칸 클립으로 이어 돈다
		_crouchBlend = _crouch ? 1.f : 0.f;
		_moveCrouch = _crouch;
		return true;
	}

	bool CAnimState::LoadBundle(const CAnimClips& clips, const char* bundleName)
	{
		const ClipBundle* bundle = clips.FindBundle(bundleName);
		if (!bundle || bundle->clips.size() < ACLIP_COUNT) return false;		// 67개 뒤는 몽타주 · 덮기 자세 클립(9단계 3-a, 조준 17개는 C-3, 앉기 20개는 D-3)
		_clips = &clips;
		_bundleIndex = (int)(bundle - clips.Bundles().data());
		_fullBodySlot = clips.FindSlot("FullBody");
		const BundleTiming* timing = nullptr;
		for (const BundleTiming& candidate : kBundleTimings)
		{
			if (std::strcmp(candidate.bundle, bundleName) == 0) timing = &candidate;
		}
		if (!timing) return false;
		_stopClipSeconds = timing->stopClipSeconds;
		for (int kind = 0; kind < 3; ++kind)
		{
			for (int direction = 0; direction < 4; ++direction)
			{
				_exitTimes[0][kind][direction] = timing->exitTimes[kind][direction];
				_exitTimes[1][kind][direction] = timing->adsExitTimes[kind][direction];
				_exitTimes[2][kind][direction] = timing->crouchExitTimes[kind][direction];
			}
		}
		_raiseWhenCrouched = timing->raiseWhenCrouched;

		static_assert(ACLIP_ADS_IDLE + kAnimWalkClips <= ACLIP_ADS_TRANSITION_FIRST && ACLIP_CROUCH_IDLE + kAnimWalkClips <= ACLIP_CROUCH_TRANSITION_FIRST, "묶음마다 서기 · 걸음 넷이 잇닿아야 한다");
		const size_t setFirst[3] = { (size_t)ACLIP_IDLE, (size_t)ACLIP_ADS_IDLE, (size_t)ACLIP_CROUCH_IDLE };
		_stepCycle = 1;
		for (int i = 0; i < kAnimBaseClips; ++i)
		{
			const AnimClip& clip = bundle->clips[setFirst[i / kAnimWalkClips] + (size_t)(i % kAnimWalkClips)];
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
			if (i % kAnimWalkClips != kClipIdle)
			{
				const int count = (int)_stepTimes[i].size();
				if (count == 0 || count % 2 != 0) return false;
				_stepCycle = _stepCycle / Gcd(_stepCycle, count) * count;
			}
		}

		if (_stepCycle > 255) return false;		// 패킷의 걸음 번호는 1바이트다

		for (int i = 0; i < kAnimStackClips; ++i) _stackLengths[i] = std::max(bundle->clips[StackClipIndex(i)].length, 0.01f);
		for (size_t c = 0; c < ACLIP_COUNT; ++c)
		{
			// 패킷의 시각은 이 길이까지만 담는다 — 대기 몸짓 칸만 눈금이 다르다. 67개 뒤 몽타주 클립은 클립 시각이 아니라 몽타주 시각이 패킷에 실린다(조각 4)
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
		for (int t = 0; t < kAnimTransitionClips; ++t)
		{
			const AnimClip& clip = bundle->clips[StackClipIndex(kAnimJumpClips + t)];
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
		_plantAlpha = 1.f;
		_disableLegIK = 0.f;
		_plantCurrent = 0.f;
		_plantVelocity = 0.f;
		_plantPrevTarget = 0.f;
		_plantPrevValid = false;
		_plantFeet[0] = _plantFeet[1] = 0.0;
		_plantTarget = 0.f;
		_plantStepped = false;
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
		_sourceFrozen = false;
		// 판 시작 = 새 몸 — 몽타주 없음 · 아직 안 쏨(제 판단. Lyra 의 새 폰은 쏜 뒤 시간이 0 에서 셀 수 있는데 기록에서 보지 않았다 — 미확인)
		_montageCount = 0;
		_poseMontageCount = 0;
		_disableLeftHand = 0.f;
		_handLeftAlpha = 1.f;
		_handFresh = false;
		_timeSinceFired = kNeverFiredSeconds;
		_overrideWeight = 0.f;
		_firing = false;
		_adsBlend = 0.f;
		_ads = false;
		_moveAds = false;
		for (float& time : _adsTimes) time = 0.f;
		_crouchBlend = 0.f;
		_crouch = false;
		_moveCrouch = false;
		for (float& time : _crouchTimes) time = 0.f;
	}

	void CAnimState::StepSpan(int clip, int step, float& outStart, float& outEnd) const
	{
		const std::vector<float>& table = _stepTimes[clip];
		const size_t k = (size_t)step % table.size();
		outStart = table[k];
		outEnd = k + 1 < table.size() ? table[k + 1] : table[0] + _lengths[clip];
	}

	// 조준 중엔 대기 몸짓 시계를 「서기에 막 들어온 때」로 되돌린다 — Lyra 는 조준 중 CanPlayIdleBreak 가 거짓이라 매 갱신 ResetIdleBreakTransitionLogic(D60 5, 원본 기준).
	// 그래서 서서 조준을 풀면 6초 뒤에 몸짓이 나온다. 몸짓 중에 조준하면 움직일 때처럼 빠지는 중으로 보고, 허리 서기 몫(서기 비중 × (1 − 조준 묶음 비중))이 0 이 되면 끝낸다(제 판단, 기록에 없다)
	// 앉음도 같다(D62 6) — noIdleBreak = 조준 또는 앉음. 허리 서기 몫 = 서기 비중 × (1 − 조준 묶음 비중) × (1 − 앉기 묶음 비중)
	// 몽타주 중도 같다(D71) — CanPlayIdleBreak 의 IsAnyMontagePlaying(엔진 MontageInstances.Num() > 0, 빠지는 중 포함). 근접 기록에서 서버만 근접 직후 몸짓을 틀어 22부위가 최대 70cm 벌어졌다(실측)
	void CAnimState::UpdateIdleBreak(bool still, bool noIdleBreak, float deltaSeconds)
	{
		if (_fidgetTime >= 0.f)
		{
			_fidgetTime += deltaSeconds;
			if (!still || noIdleBreak) _fidgetLeaving = true;
			if (_fidgetTime >= _fidgetLength || (_fidgetLeaving && _weights[kClipIdle] * ((1.f - _adsBlend) * (1.f - _crouchBlend)) <= kZeroWeight))
			{
				_fidgetTime = -1.f;
				_fidgetLeaving = false;
				_standTime = 0.f;
			}
			return;
		}
		if (noIdleBreak)
		{
			_standTime = still ? _stopClipSeconds : 0.f;
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
		layer.settled = false;
	}

	// 서기 상태에서 앉음이 바뀜 → 앉기 · 일어서기 전환 칸을 얹는다(D62 4). 바로 얹으면(실제 경로, D63) 아래 칸은 다 덮여 이번 갱신 끝에 버려지므로 먼저 비운다 —
	// 칸이 차서 밀려나는 수(_stackOverflows)를 안 늘리려고. 그 위에 이번 갱신의 출발 · 점프가 얹힐 수 있다(Lyra 서기 상태가 섞여 나가는 동안 같은 갱신에 StanceTransition 으로 간다 —
	// 앉은 채 점프 기록에서 일어서기 전환이 점프 섞기 첫 프레임 몫 0.93 으로 섰다, 실측)
	void CAnimState::PushStance(bool crouched)
	{
		if (_stanceInSeconds <= 0.f) _count = 0;
		PushJump(crouched ? kAnimStanceEntry : kAnimStanceExit, _stanceInSeconds);
	}

	// 서기 안 전환이 남은 0.25초에 닿으면 서기로 돌아간다(D62 4) — 이 갱신이 시각을 넘기기 전, 앞 갱신이 남긴 시각으로 본다(엔진은 전환 규칙을 앞 프레임 시각으로 판정한다).
	// 서기 클립은 처음부터(기록에서 돌아간 프레임의 서기 시각 0.017 = 한 프레임). 실제 경로(D63) = 그 칸이 맨 위면 서기 자리를 0.25초에 걸쳐 덮는다(엔진 자동 전환 섞기 —
	// 문턱인 남은 시간이 곧 섞기 시간). 위에 출발 · 점프 칸이 있으면 그 칸을 그 자리에서 서기 자리로 바꾼다(제 판단 — 덮인 칸이라 자세 몫이 작고, 칸을 하나 더 쌓지 않는다)
	void CAnimState::SettleStance()
	{
		for (int i = 0; i < _count; ++i)
		{
			Layer& layer = _stack[i];
			if (layer.clip < kAnimStanceEntry || layer.settled) continue;
			if (_stackLengths[layer.clip] - layer.time > kStanceExitRemaining + kStanceExitSlack) continue;
			_idleTime = 0.f;
			if (_stanceOutSeconds > 0.f && i == _count - 1)
			{
				layer.settled = true;
				PushJump(kJumpNone, _stanceOutSeconds);
				return;
			}
			layer.clip = kJumpNone;
		}
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

	// 전환 클립 시각 — 클립 종류(출발 · 멈춤 · 꺾기)의 갱신 함수대로. 맨 위가 아니어도(걸음 상태로 넘어가 섞여 빠지는 중에도) 같은 함수다 —
	// Lyra 는 섞여 빠지는 상태의 갱신 함수(UpdateStartAnim · UpdateStopAnim · UpdatePivotAnim)도 계속 부른다(원본 기준). 맨 위 층의 종류는 늘 지금 상태와 같아 맨 위 결과는 전과 같다.
	// 없으면 권총 MM_Pistol_Walk_Left_Start 가 빠지는 동안 배속 1 로 돌아 걸음 위상이 0.16초 밀렸다(content-add-progress.md 미결 (나), 실측)
	float CAnimState::TransitionClipTime(const Layer& layer, float deltaSeconds) const
	{
		const int transition = layer.clip - kAnimJumpClips;
		const auto advanceByTravel = [&]()
		{
			const float target = TimeFromDistance(transition, DistanceAtTime(transition, layer.time) + _frameTravel);
			const float rate = deltaSeconds > 0.f ? Clamp((target - layer.time) / deltaSeconds, kDistanceRateMin, kDistanceRateMax) : 1.f;
			return layer.time + rate * deltaSeconds;
		};
		switch ((transition % kAnimTransitions) / 4)
		{
		case kTransitionStart:
			return advanceByTravel();
		case kTransitionStop:
			// ShouldDistanceMatchStop = HasVelocity && !HasAcceleration(ABP_ItemAnimLayersBase, 원본 기준) — 아니면 배속 1. 맨 위 멈춤은 가속하면 바로 출발로 넘어가 늘 가속이 없다
			return !_accelerating && _stopDistance > 0.f ? std::max(layer.time, TimeFromDistance(transition, -_stopDistance)) : layer.time + deltaSeconds;
		case kTransitionPivot:
			return _beforePivot ? std::max(layer.time, TimeFromDistance(transition, -_pivotDistance)) : advanceByTravel();
		default:
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
		_moveAds = _ads;		// 전환 클립은 들어갈 때의 조준 · 앉음으로 고른다(D60 3 · D62 3) — 앉음이 조준보다 먼저
		_moveCrouch = _crouch;
		const int set = _moveCrouch ? 2 : _moveAds ? 1 : 0;
		PushJump(kAnimJumpClips + set * kAnimTransitions + kind * 4 + direction, fadeSeconds);
	}

	void CAnimState::EnterLocomotion(EMoveState state, float fadeSeconds)
	{
		_followClip = -1;
		if (state == EMoveState::Cycle && _count > 0 && _stack[_count - 1].clip >= kAnimJumpClips && _stack[_count - 1].clip < kAnimStanceEntry)
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
		// 출발 · 꺾기 클립이 쌓기 칸에 남아 있는 동안은 비중과 상관없이 이끈다 — Lyra 걸음 동기 묶음에서 거리 맞추기 평가기는 「늘 이끎」(점수 2.0 > 섞기 비중,
		// UE_5.8 AnimationAsset.cpp:90-93 TestTickRecordForLeadership, 원본 기준). 옛 판은 비중 0.5 아래면 놓아 앉은 출발 뒤 0.14초 앞섰다(미결 (사), 실측).
		// 따르는 시각 = 이끄는 클립이 이번 갱신에 갈 시각(엔진은 이끄는 쪽을 먼저 넘긴다) — UpdateJump 와 같은 TransitionClipTime
		for (int i = 0; i < _count; ++i)
		{
			if (_stack[i].clip != _followClip) continue;
			const int transition = _followClip - kAnimJumpClips;
			const float time = TransitionClipTime(_stack[i], deltaSeconds);
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

	// 엔진 FVector2D::GetSafeNormal
	static void SafeNormal2D(const double* v, double out[2])
	{
		out[0] = out[1] = 0.0;
		const double squareSum = v[0] * v[0] + v[1] * v[1];
		if (squareSum <= (double)kSmallNumber) return;
		const double scale = 1.0 / std::sqrt(squareSum);
		out[0] = v[0] * scale;
		out[1] = v[1] * scale;
	}

	// 전환 상태 판정 앞에서 — 이끄는 전환 클립의 이번 시각(FollowStepsOfTransition)이 이 값을 쓴다. 땅에 있을 때만 갱신한다(공중에선 전 값 그대로 — 옛 판과 같다)
	void CAnimState::UpdateMotion(const AnimMoveInput& input, float deltaSeconds)
	{
		if (input.falling) return;
		const double* velocity = input.velocity;
		const float speed = (float)std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
		double accelDirection[2];
		SafeNormal2D(input.acceleration, accelDirection);
		_accelerating = input.accelerating;
		_frameTravel = speed * deltaSeconds;
		_beforePivot = _accelerating && velocity[0] * accelDirection[0] + velocity[1] * accelDirection[1] < 0.0;
		_stopDistance = PredictStopDistance(speed);
		_pivotDistance = _beforePivot ? PredictPivotDistance(input.acceleration, velocity) : 0.f;
	}

	void CAnimState::UpdateTransitions(const AnimMoveInput& input, float deltaSeconds)
	{
		(void)deltaSeconds;
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
		double accelDirection[2];
		SafeNormal2D(acceleration, accelDirection);

		const bool pivotNeeded = _beforePivot && speed > kPivotMinSpeed;
		const float topTime = _count > 0 ? _stack[_count - 1].time : 0.f;
		const auto enterPivot = [&]()
		{
			_pivotAccelDirection[0] = accelDirection[0];
			_pivotAccelDirection[1] = accelDirection[1];
			EnterTransition(EMoveState::Pivot, CardinalFromAngle(AngleOf(velocity)), kToPivotSeconds);
		};
		const auto enterStop = [&]() { EnterTransition(EMoveState::Stop, CardinalFromAngle(AngleOf(velocity)), kToStopSeconds); };
		// 조준이 바뀌면 전환 클립을 버리고 반복 · 서기로(D60 3). 우선순위는 Lyra 전환 표 차례 — 출발 · 돌아서기에선 빠지는 시각 다음, 멈춤에선 출발 다음 · 빠지는 시각 앞(원본 기준).
		// 앉음이 바뀌어도 같다 — 같은 전환 규칙이 CrouchStateChange 를 ADSStateChanged 와 같은 자리에 둔다(D62 3)
		const float(&exitTimes)[3][4] = _exitTimes[_moveCrouch ? 2 : _moveAds ? 1 : 0];
		const bool setChanged = _ads != _moveAds || _crouch != _moveCrouch;		// 전환 클립 묶음을 고른 조준 · 앉음이 바뀌었나
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
			else if (topTime >= exitTimes[kTransitionStart][_moveDirection] - kExitTimeSlack) EnterLocomotion(EMoveState::Cycle, kStartToCycleSeconds);
			else if (setChanged) EnterLocomotion(EMoveState::Cycle, kAdsChangeSeconds);
			break;
		case EMoveState::Cycle:
			if (!accelerating) enterStop();
			else if (pivotNeeded) enterPivot();
			break;
		case EMoveState::Stop:
			if (accelerating) EnterTransition(EMoveState::Start, CardinalFromAngle(AngleOf(acceleration)), kStopToStartSeconds);
			else if (setChanged) EnterLocomotion(EMoveState::Idle, kAdsChangeSeconds);
			// 아직 움직이는 중이어도 빠진다 — Lyra 전환 조건은 TransitionToLocomotion 알림 구간뿐이다(원본 기준, 권총 앉기 기록은 225.2cm/s 에서 빠짐 · 실측, 미결 (바))
			else if (topTime >= exitTimes[kTransitionStop][_moveDirection] - kExitTimeSlack) EnterLocomotion(EMoveState::Idle, kStopToIdleSeconds);
			break;
		case EMoveState::Pivot:
			if (!accelerating) enterStop();
			else if (pivotNeeded && accelDirection[0] * _pivotAccelDirection[0] + accelDirection[1] * _pivotAccelDirection[1] < 0.0) enterPivot();
			else if (topTime >= exitTimes[kTransitionPivot][_moveDirection] - kExitTimeSlack) EnterLocomotion(EMoveState::Cycle, kPivotToCycleSeconds);
			else if (setChanged) EnterLocomotion(EMoveState::Cycle, kAdsChangeSeconds);
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
			if (layer.clip >= kAnimJumpClips && layer.clip < kAnimStanceEntry) layer.time = TransitionClipTime(layer, deltaSeconds);
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

	void CAnimState::Update(const AnimMoveInput& input, CAnimPoseEvaluator& evaluator, BoneTransform* scratch)
	{
		const float deltaSeconds = input.deltaSeconds;
		const double* velocity = input.velocity;
		const float speed = (float)std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
		SettleStance();		// 서기 시계를 넘기기 전에(되돌린 시계가 이번 갱신에 한 프레임 간다)
		// 통째 자리 몽타주(부활)가 몸을 다 덮는 동안 Lyra 는 걸음 쪽(상태 · 걸음 비중 · 걸음 · 서기 시계 · 전환 클립)을 안 넘긴다 — 부활 몽타주 내내 상태가 멈춤으로 148프레임 남았다가
		// 비중이 1 아래로 내려간 첫 프레임에 서기로 갔다(animcmp_ev_rifle_probe.csv 2169~2316 · 4639~4786, 실측. content-add-progress.md 미결 (다)). 그 첫 프레임은 ReinitLocomotion
		const bool frozen = FullBodyCoversSource(deltaSeconds);
		const bool reinit = !frozen && _sourceFrozen;
		_sourceFrozen = frozen;
		if (reinit) ReinitLocomotion();

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
		const float walkFollow = frozen ? 0.f : reinit ? 1.f : follow;		// 다시 초기화한 서기 상태는 섞기 없이 선다
		float sum = 0.f;
		for (int i = 0; i < kAnimWalkClips; ++i)
		{
			_weights[i] += (targets[i] - _weights[i]) * walkFollow;
			sum += _weights[i];
		}
		if (sum > kSmallNumber)
		{
			for (float& weight : _weights) weight /= sum;
		}
		// 조준 묶음 비중(D60 2) — 같은 따라가기. 끝까지 안 닿는 지수 따라가기라 엔진 비중 문턱 안이면 끝값에 붙인다(허리만 쓰는 판이 조준 전과 같은 수를 내도록)
		_ads = input.ads;
		const float adsTarget = _ads ? 1.f : 0.f;
		_adsBlend += (adsTarget - _adsBlend) * follow;
		if (std::fabs(adsTarget - _adsBlend) <= kZeroWeight) _adsBlend = adsTarget;
		// 앉기 묶음 비중(D62 2) — 같은 따라가기 · 같은 붙이기. 앉기가 조준보다 먼저라 서 있는 몫(1 − 앉기)을 허리 · 조준이 나눈다
		const bool crouchChanged = input.crouched != _crouch;
		_crouch = input.crouched;
		const float crouchTarget = _crouch ? 1.f : 0.f;
		_crouchBlend += (crouchTarget - _crouchBlend) * follow;
		if (std::fabs(crouchTarget - _crouchBlend) <= kZeroWeight) _crouchBlend = crouchTarget;
		const float standShare = 1.f - _crouchBlend;
		const float setShares[3] = { (1.f - _adsBlend) * standShare, _adsBlend * standShare, _crouchBlend };

		// 달리기 진행 — 클립의 원래 속도와 실제 속도의 비로 재생하고, 발걸음 단위로 함께 넘어가 섞이는 두 클립의 발이 같은 때 닿게 한다.
		// 조준 · 앉기 묶음이 섞이는 동안은 묶음 클립을 비중대로 같이 센다(앉지 않으면 앉기 몫이 0 이라 조준 전 · 앉기 전과 같은 수)
		float directionWeight = 0.f;
		float rootSpeed = 0.f;
		float stepSeconds = 0.f;
		for (int i = kClipFwd; i <= kClipRight; ++i)
		{
			for (int set = 0; set < 3; ++set)
			{
				const int clip = i + set * kAnimWalkClips;
				const float weight = _weights[i] * setShares[set];
				float start, end;
				StepSpan(clip, _step, start, end);
				directionWeight += weight;
				rootSpeed += weight * _rootSpeeds[clip];
				stepSeconds += weight * (end - start);
			}
		}
		if (!frozen) UpdateMotion(input, deltaSeconds);
		const bool stepsFollow = !frozen && FollowStepsOfTransition(deltaSeconds);
		if (!frozen && !stepsFollow && directionWeight > 0.001f && stepSeconds > kSmallNumber)
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
		// 서기 시계는 하나 — 조준을 바꿔도 이어 간다(Lyra 허리 4.900 → 조준 4.917, 실측). 도는 길이는 지금 고른 쪽(앉음 > 조준 > 허리) 서기 클립이다
		// 더하기 전에 지금 쪽 길이로 자른다 — 엔진 재생기가 매 갱신 시각을 지금 클립 길이로 자른 뒤 넘긴다(AnimNode_SequencePlayer.cpp:102, 원본 기준).
		// 그래서 시계가 허리 길이를 넘은 채 조준을 떼면 허리 클립은 나머지가 아니라 처음부터 돈다(소총 7.367 → 0.017, 11-18 adsl_rifle 기록 · 미결 (마))
		const float idleLength = _lengths[(_crouch ? 2 * kAnimWalkClips : _ads ? kAnimWalkClips : 0) + kClipIdle];
		if (!frozen) _idleTime = idleLength > 0.f ? std::fmod(std::min(_idleTime, idleLength) + deltaSeconds, idleLength) : 0.f;

		_times[kClipIdle] = WrapIdle(_idleTime, _lengths[kClipIdle]);
		_adsTimes[kClipIdle] = WrapIdle(_idleTime, _lengths[kAnimWalkClips + kClipIdle]);
		_crouchTimes[kClipIdle] = WrapIdle(_idleTime, _lengths[2 * kAnimWalkClips + kClipIdle]);
		_timesStep = _step;
		_timesStepFrac = _stepFrac;
		_timesIdle = _idleTime;
		for (int i = kClipFwd; i <= kClipRight; ++i)
		{
			float start, end;
			StepSpan(i, _step, start, end);
			_times[i] = std::fmod(start + _stepFrac * (end - start), _lengths[i]);
			StepSpan(kAnimWalkClips + i, _step, start, end);
			_adsTimes[i] = std::fmod(start + _stepFrac * (end - start), _lengths[kAnimWalkClips + i]);
			StepSpan(2 * kAnimWalkClips + i, _step, start, end);
			_crouchTimes[i] = std::fmod(start + _stepFrac * (end - start), _lengths[2 * kAnimWalkClips + i]);
		}

		_pitch = input.pitch;
		const bool falling = input.falling;
		const bool accelerating = input.accelerating;
		UpdateFiring(accelerating, falling, _ads, _crouch, deltaSeconds);
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
		// 발 붙이기 장치 켜짐(L-6-c-1) — Lyra ABP 컨트롤 릭 노드 알파 = ShouldEnableControlRig(곡선 DisableLegIK <= 0), 0.2초 선형(weapons_l2a.txt:4445-4465, 원본 기준).
		// 곡선은 앞 자세 것(지난 갱신 끝 SnapshotMontages) — 한 프레임 늦게 읽어야 켬 ↔ 끔 기록 차와 맞는다(L-2-a sim_plant.py LAG=1, 실측). 곡선은 앞 대시 몽타주에만 있다.
		// 장치 셈(발 목표 · 스프링)은 이번 자세가 나온 갱신 끝에서 한다(StepPlant)
		const float plantStep = deltaSeconds / kPlantAlphaSeconds;
		_plantAlpha = _disableLegIK <= 0.f ? std::min(1.f, _plantAlpha + plantStep) : std::max(0.f, _plantAlpha - plantStep);

		// 서기 상태에서 앉음이 바뀌면 앉기 · 일어서기 전환(D62 4). 서기였으면 이번 갱신의 출발 · 점프보다 아래에, 멈춤 · 공중에서 이번에 서기로 왔으면 그 위에 얹는다
		// (멈춘 채 일어서기 기록 — 멈춤 클립이 그 프레임에 빠지고 일어서기 전환이 바로 섰다, 실측)
		const bool wasIdle = _moveState == EMoveState::Idle;
		if (!frozen)
		{
			if (crouchChanged && wasIdle) PushStance(_crouch);
			UpdateTransitions(input, deltaSeconds);
			if (crouchChanged && !wasIdle && _moveState == EMoveState::Idle) PushStance(_crouch);
			UpdateJump(falling, accelerating, (float)velocity[2], timeToApex, groundDistance, deltaSeconds);
		}
		// 몽타주 칸은 이번 갱신에서 넘기기 전 것이다 — 엔진은 몽타주를 먼저 넘기고 이 판정을 하므로 끝나는 프레임에서 한 프레임 갈린다(시계 6초 앞에선 16ms, 제 판단)
		// 사격 태그(GameplayTag_IsFiring)는 안 넣었다 — 사격 몽타주가 그 시간을 덮는다고 본다(짐작)
		// 덮인 동안 Lyra 몸짓 시계는 멈췄다가(13.93 그대로) 다시 갱신될 때 15 로 돌아갔다(melee_pistol probe 1428~1577, 실측) — 몽타주가 있으면 아래 줄이 매 프레임 되돌리므로 따로 멈추지 않는다
		UpdateIdleBreak(!falling && !accelerating && speed < 1.f && !HasJumpLayer(), _ads || _crouch || _montageCount > 0, deltaSeconds);

		// 왼손 비중은 앞 자세의 곡선으로 — 기록에서 HandIK_Left_Alpha 가 곡선보다 한 프레임 늦다(실측). 그다음 몽타주를 넘기고 이번 자세 목록을 뜬다
		_handLeftAlpha = _handFresh ? 1.f : Clamp(1.f - _disableLeftHand, 0.f, 1.f);
		_handFresh = false;
		UpdateMontages(deltaSeconds);
		SnapshotMontages();
		StepPlant(falling, plant, deltaSeconds, evaluator, scratch);
	}

	// 발 붙이기 장치 한 걸음(L-6-c-2) — L-2-a 규칙(Lyra 장치 켬 ↔ 끔 기록 차 2492프레임 최대 0.001cm, 기록 대조):
	//   발마다 쓸기가 땅을 맞히면 목표 = 땅, 못 맞히면 0(RigUnit_WorldCollision.cpp:91 이 맞은 점을 비움) → 골반 목표 = min(왼, 오른) → 스프링 → × 알파
	// 발 높이는 이번 갱신이 끝난 자세(발 디딤 앞)의 것이다. 땅은 평지 하나(-plantGap) — 발 자리마다 쓸기(비탈 · 계단)는 안 옮겼다(대조할 Lyra 비탈 기록이 없다).
	// 뜬 프레임은 장치를 안 돌리고 옛 착지 문턱(plant)을 그대로 둔다 — 점프 · 착지는 L-6-c-3 이 점프 기록으로 대조한다
	void CAnimState::StepPlant(bool falling, bool plant, float deltaSeconds, CAnimPoseEvaluator& evaluator, BoneTransform* scratch)
	{
		_plantStepped = false;
		if (!falling && _plantAlpha > kPlantAlphaRelevant && deltaSeconds > kSmallNumber)
		{
			if (evaluator.Bundle() == &_clips->Bundles()[(size_t)_bundleIndex])
			{
				AnimPoseInput pose;
				ToPoseInput(pose);
				for (float& offset : pose.plantOffset) offset = 0.f;
				evaluator.EvaluateLocal(pose, scratch);
				evaluator.FootHeightsFromLocal(scratch, _plantFeet);
			}
			else
			{
				++_plantBundleMisses;		// 발 높이를 못 읽으면 앞 값으로 — 세어 두고 0 인지 본다
			}
			const float ground = -_plantDrop;
			const float left = (float)_plantFeet[0] - kPlantTraceReach <= ground ? ground : 0.f;
			const float right = (float)_plantFeet[1] - kPlantTraceReach <= ground ? ground : 0.f;
			_plantTarget = std::min(left, right);
			// 판 처음(Reset)은 스프링 값 · 속도 0 에서 시작한다 — 컨트롤 릭 변수 CurrentPelvisOffsetZ 기본값 0 이고 그 변수에 넣는 곳은 스프링 결과뿐이다
			// (CR_Mannequin_FootPlant 그래프 덤프, 원본 기준). Lyra 대조 기록은 첫 프레임 앞 대기 갱신 동안 목표에 닿아 이 시작값을 못 가른다
			// 엔진 GenericSpringInterp(KismetMathLibrary.cpp:427) → FMath::SpringDamper 임계 감쇠 갈래(UnrealMathUtility.h:1741)와 같은 식.
			// 각진동수만 세기 × 2π 로 바로 셈한다 — 엔진은 강성(각진동수²)의 제곱근을 거쳐 float 끝자리가 다를 수 있다
			const float targetRate = _plantPrevValid ? (_plantTarget - _plantPrevTarget) * (kPlantSpringTargetRate / deltaSeconds) : 0.f;
			const float w = kPlantSpringHz * 2.f * kPi;
			const float adjusted = _plantTarget + targetRate * (2.f / w);
			const float c1 = _plantCurrent - adjusted;
			const float c2 = _plantVelocity + c1 * w;
			const float x = w * deltaSeconds;
			const float e = 1.f / (1.f + 1.00746054f * x + 0.45053901f * x * x + 0.25724632f * x * x * x);		// InvExpApprox
			_plantCurrent = adjusted + (c1 + c2 * deltaSeconds) * e;
			_plantVelocity = (c2 - c1 * w - c2 * (w * deltaSeconds)) * e;
			_plantPrevTarget = _plantTarget;
			_plantPrevValid = true;
			_plantStepped = true;
		}
		_plantOffset[0] = 0.f;
		_plantOffset[1] = 0.f;
		_plantOffset[2] = plant && _plantAlpha > 0.f ? _plantCurrent * _plantAlpha : 0.f;
	}

	CAnimState::PlantView CAnimState::Plant() const
	{
		PlantView view;
		view.feet[0] = _plantFeet[0];
		view.feet[1] = _plantFeet[1];
		view.ground = -_plantDrop;
		view.target = _plantTarget;
		view.current = _plantCurrent;
		view.alpha = _plantAlpha;
		view.stepped = _plantStepped;
		return view;
	}

	void CAnimState::ToWire(wire::AnimStateWire& out) const
	{
		for (int i = 0; i < kAnimWalkClips; ++i) out.weights[i] = ToWireByte(_weights[i]);
		out.step = (uint8_t)_timesStep;
		out.stepFrac = (uint16_t)std::lround(Clamp(_timesStepFrac, 0.f, 1.f) * 65535.f);
		out.idleTime = ToWireTime(_timesIdle);
		out.aimBlend = ToWireByte(_aimBlend);
		out.adsBlend = ToWireByte(_adsBlend);
		out.crouchBlend = ToWireByte(_crouchBlend);
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
			// 빼기 진행 — 0 = 없음, 1~255 = 0~1(시작 진행 0 도 0 과 갈리게 1 부터)
			cell.blendOut = slot.blendOut < 0.f ? (uint8_t)0 : (uint8_t)(1 + std::lround(Clamp(slot.blendOut, 0.f, 1.f) * 254.f));
		}
	}

	void CAnimState::FromWire(const wire::AnimStateWire& in, float pitch, bool crouched, AnimPoseInput& out) const
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

		const float idle = FromWireTime(in.idleTime);
		out.times[kClipIdle] = WrapIdle(idle, _lengths[kClipIdle]);
		out.adsTimes[kClipIdle] = WrapIdle(idle, _lengths[kAnimWalkClips + kClipIdle]);
		out.crouchTimes[kClipIdle] = WrapIdle(idle, _lengths[2 * kAnimWalkClips + kClipIdle]);
		const float frac = (float)in.stepFrac / 65535.f;
		for (int i = kClipFwd; i <= kClipRight; ++i)
		{
			float start, end;
			StepSpan(i, in.step, start, end);
			out.times[i] = std::fmod(start + frac * (end - start), _lengths[i]);
			StepSpan(kAnimWalkClips + i, in.step, start, end);
			out.adsTimes[i] = std::fmod(start + frac * (end - start), _lengths[kAnimWalkClips + i]);
			StepSpan(2 * kAnimWalkClips + i, in.step, start, end);
			out.crouchTimes[i] = std::fmod(start + frac * (end - start), _lengths[2 * kAnimWalkClips + i]);
		}
		out.adsBlend = (float)in.adsBlend / 255.f;
		out.crouchBlend = (float)in.crouchBlend / 255.f;
		out.crouched = crouched;

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
			out.montages[i].blendOut = cell.blendOut == 0 ? -1.f : (float)(cell.blendOut - 1) / 254.f;
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
			out.adsTimes[i] = _adsTimes[i];
			out.crouchTimes[i] = _crouchTimes[i];
		}
		out.adsBlend = _adsBlend;
		out.crouchBlend = _crouchBlend;
		out.crouched = _crouch;
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
