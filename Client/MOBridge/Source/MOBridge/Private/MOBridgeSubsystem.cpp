#include "MOBridgeSubsystem.h"

#include "GameProtocol.h"
#include "MapCollision.h"
#include "MatchRules.h"
#include "Movement.h"
#include "Prediction.h"
#include "Weapon.h"

#include "AIController.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimNode_StateMachine.h"
#include "Components/SkeletalMeshComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/GameEngine.h"
#include "Engine/World.h"
#include "FrameGrabber.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GenericTeamAgentInterface.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Slate/SceneViewport.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "UnrealClient.h"

#include "Abilities/GameplayAbilityTargetTypes.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/GameplayMessageSubsystem.h"
#include "MOBServerDamageEffect.h"

#include "AbilitySystem/Attributes/LyraHealthSet.h"
#include "AbilitySystem/LyraAbilitySystemComponent.h"
#include "AbilitySystem/Phases/LyraGamePhaseAbility.h"
#include "AbilitySystem/Phases/LyraGamePhaseSubsystem.h"
#include "Character/LyraCharacter.h"
#include "Character/LyraCharacterMovementComponent.h"
#include "Character/LyraHealthComponent.h"
#include "Character/LyraPawnExtensionComponent.h"
#include "Equipment/LyraEquipmentDefinition.h"
#include "Equipment/LyraEquipmentInstance.h"
#include "Equipment/LyraEquipmentManagerComponent.h"
#include "Equipment/LyraQuickBarComponent.h"
#include "GameModes/LyraGameMode.h"
#include "Inventory/LyraInventoryItemDefinition.h"
#include "Inventory/LyraInventoryItemInstance.h"
#include "Inventory/LyraInventoryManagerComponent.h"
#include "LyraGameplayTags.h"
#include "EngineUtils.h"
#include "Weapons/LyraWeaponSpawner.h"
#include "Messages/LyraVerbMessage.h"
#include "Player/LyraPlayerState.h"
#include "Teams/LyraTeamSubsystem.h"
#include "Weapons/LyraWeaponStateComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MOBridgeSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogMOB, Log, All);

namespace
{
	namespace wire = mo::wire;

	constexpr double kInputSeconds = 1.0 / wire::kTickRate;
	constexpr int32 kMaxCatchUpInputs = 8;		// 서버 입력 한도(kMaxInputTokens)와 같다 — 더 밀렸으면 보내도 버려진다
	constexpr double kFocalDistance = 1024.0;	// ULyraGameplayAbility_RangedWeapon::GetTargetingTransform 과 같은 값

	constexpr double kCorrectTolerance = 0.1;	// cm·cm/s — 이 안이면 부동소수 차이로 보고 안 고친다
	constexpr double kVisualSnapDistance = 300.0;	// 이보다 크게 틀어지면 서서히 줄이지 않고 바로 옮긴다
	constexpr float kVisualErrorDecay = 12.f;		// 초당 — 보정 차이가 0.1초면 약 30% 남는다
	constexpr double kRemoteDelayTicks = 6.0;		// 0.1초

	constexpr double kAutoStart = 6.0;
	constexpr double kAutoLeg = 1.2;

	TArray<float> ParseFloats(const FString& S)
	{
		TArray<FString> Parts;
		S.ParseIntoArray(Parts, TEXT(","));
		TArray<float> Out;
		for (const FString& P : Parts) Out.Add(FCString::Atof(*P));
		return Out;
	}

	// Lyra 가 사격 시작점을 정하는 식 그대로 — 카메라 시선 위에서 내 몸에 가장 가까운 점
	// (LyraGameplayAbility_RangedWeapon.cpp GetTargetingTransform, CameraTowardsFocus)
	FVector TargetingStart(APlayerController* PC, const FVector& PawnLocation)
	{
		FVector CamLoc;
		FRotator CamRot;
		PC->GetPlayerViewPoint(CamLoc, CamRot);
		const FVector AimDir = CamRot.Vector().GetSafeNormal();
		const FVector FocalLoc = CamLoc + AimDir * kFocalDistance;
		return FocalLoc + ((PawnLocation - FocalLoc) | AimDir) * AimDir;
	}

	FVector ToFVector(const mo::Vec3& V)
	{
		return FVector(V.x, V.y, V.z);
	}

	// 위치는 서버(·내 예측) 값을 입히므로 언리얼 이동 계산은 쉰다. 입력 소비와 가속 계산은 그대로 돈다 — 애니가 읽는 값
	void NoLyraMove(ULyraCharacterMovementComponent*, float)
	{
	}

	void ApplyMoveToCharacter(ACharacter* Ch, const FVector& Velocity, bool bFalling)
	{
		UCharacterMovementComponent* M = Ch->GetCharacterMovement();
		if (!M) return;
		if (bFalling != M->IsFalling()) M->SetMovementMode(bFalling ? MOVE_Falling : MOVE_Walking);
		M->Velocity = Velocity;
		M->UpdateComponentVelocity();	// 애니가 읽는 GetVelocity 는 이쪽 값이다
	}
}

// 공용 코드 묶음 — 헤더에 Shared 를 드러내지 않으려고 여기에 둔다
struct FMOBCore
{
	mo::CMapCollision Map;
	mo::MoveParams Params = mo::MakeLyraMoveParams();	// CMoveSim 은 참조로 들고 있다 — 임시 값을 넘기면 안 된다
	mo::CMoveSim Sim;
	mo::MatchRules Rules;
	mo::CSelfPrediction Prediction;

	FMOBCore()
		: Sim(Map, Params)
		, Prediction(Sim, Rules, Params.maxWalkSpeed)
	{
	}
};

// ─────────────────────────────────────────────────────────────
//  켜고 끄기
// ─────────────────────────────────────────────────────────────

bool UMOBridgeSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	if (!World || !(World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE)) return false;
	const TCHAR* Cmd = FCommandLine::Get();
	FString Dummy;
	return FParse::Value(Cmd, TEXT("MOBServer="), Dummy) || FParse::Value(Cmd, TEXT("MOBShotAt="), Dummy) || FParse::Value(Cmd, TEXT("MOBRecord="), Dummy);
}

void UMOBridgeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	ParseCommandLine();
	UE_LOG(LogMOB, Log, TEXT("MOB_INIT world=%s server=%s tag=%s blockDamage=%d autoInput=%d"),
		*GetNameSafe(GetWorld()), *ServerAddr, *Tag, bBlockDamage ? 1 : 0, bAutoInput ? 1 : 0);
}

void UMOBridgeSubsystem::Deinitialize()
{
	if (Socket && !bQuitting) FinishAndQuit(TEXT("closed"));	// 사람이 창을 닫은 경우 — 기록은 남긴다
	if (Socket)
	{
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}
	if (Core)
	{
		ULyraCharacterMovementComponent::MOBMoveHook = nullptr;
		delete Core;
		Core = nullptr;
	}
	Super::Deinitialize();
}

TStatId UMOBridgeSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMOBridgeSubsystem, STATGROUP_Tickables);
}

void UMOBridgeSubsystem::ParseCommandLine()
{
	const TCHAR* Cmd = FCommandLine::Get();
	FString S;
	FParse::Value(Cmd, TEXT("MOBServer="), ServerAddr);
	// 목록 값은 쉼표에서 잘리지 않게(FParse::Value 기본값은 쉼표를 구분자로 보고 첫 값만 준다 — 첫 실행에서 캡처가 1장만 찍힌 까닭)
	if (FParse::Value(Cmd, TEXT("MOBShotAt="), S, false)) ShotAt = ParseFloats(S);
	if (FParse::Value(Cmd, TEXT("MOBRecord="), S, false))
	{
		const TArray<float> V = ParseFloats(S);
		if (V.Num() >= 2) { RecStart = V[0]; RecDur = V[1]; }
		if (V.Num() >= 3) RecFps = V[2];
	}
	FParse::Value(Cmd, TEXT("MOBQuitAt="), QuitAt);
	FParse::Value(Cmd, TEXT("MOBTag="), Tag);
	FParse::Value(Cmd, TEXT("MOBBreakPredictAt="), BreakPredictAt);
	bAutoInput = FParse::Param(Cmd, TEXT("MOBAutoInput"));
	bAutoAim = FParse::Param(Cmd, TEXT("MOBAutoAim"));
	bEventShots = FParse::Param(Cmd, TEXT("MOBEventShots"));
	bKeepPickups = FParse::Param(Cmd, TEXT("MOBKeepPickups"));
	bAnimLog = FParse::Param(Cmd, TEXT("MOBAnimLog"));
	bViewLog = FParse::Param(Cmd, TEXT("MOBViewLog"));
	FParse::Value(Cmd, TEXT("MOBBreakAt="), BreakAt);
	// 대기 단계는 경험 로드 직후 시작한다 — 준비(캐릭터 생성)를 기다리면 늦는다
	if (!ServerAddr.IsEmpty()) IConsoleManager::Get().FindConsoleVariable(TEXT("Lyra.Bridge.SkipWarmup"))->Set(1, ECVF_SetByCode);
	int32 Block = 1;
	if (FParse::Value(Cmd, TEXT("MOBBlockDamage="), Block)) bBlockDamage = (Block != 0);
}

// 내 캐릭터가 체력까지 붙은 뒤를 「준비」로 본다 — 경험(Experience) 로드가 끝나야 폰이 생긴다
bool UMOBridgeSubsystem::TryBecomeReady()
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	UAbilitySystemComponent* ASC = FindASC(Pawn);
	if (!ASC || !ASC->HasAttributeSetForAttribute(ULyraHealthSet::GetHealthAttribute())) return false;

	ReadyAt = FPlatformTime::Seconds();
	UE_LOG(LogMOB, Log, TEXT("MOB_READY pawn=%s at=(%.1f,%.1f,%.1f)"), *GetNameSafe(Pawn), Pawn->GetActorLocation().X, Pawn->GetActorLocation().Y, Pawn->GetActorLocation().Z);

	if (!ServerAddr.IsEmpty())
	{
		if (bBlockDamage)
		{
			IConsoleManager::Get().FindConsoleVariable(TEXT("Lyra.Bridge.BlockLyraDamage"))->Set(1, ECVF_SetByCode);
			IConsoleManager::Get().FindConsoleVariable(TEXT("Lyra.Bridge.BlockBotRespawn"))->Set(1, ECVF_SetByCode);
			IConsoleManager::Get().FindConsoleVariable(TEXT("Lyra.Bridge.BlockLyraReset"))->Set(1, ECVF_SetByCode);
		}
		Connect();
	}
	return true;
}

// ─────────────────────────────────────────────────────────────
//  매 틱
// ─────────────────────────────────────────────────────────────

void UMOBridgeSubsystem::Tick(float DeltaTime)
{
	if (bQuitting) return;
	if (ReadyAt < 0 && !TryBecomeReady()) return;

	const double Now = FPlatformTime::Seconds();
	const double T = Now - ReadyAt;
	MaxFrameGap = FMath::Max(MaxFrameGap, (double)DeltaTime);

	PumpSocket();
	if (bQuitting) return;
	if (bAutoInput) TickAutoInput(T);
	TickInput(Now);
	if (Core && BreakPredictAt > 0 && T >= BreakPredictAt)
	{
		BreakPredictAt = -1;
		Core->Prediction.DebugNudge(mo::Vec3(100, 0, 0));
		UE_LOG(LogMOB, Log, TEXT("MOB_BREAK_PREDICT t=%.1f seq=%u"), T, Seq);
	}
	if (BreakAt > 0 && T >= BreakAt)
	{
		BreakAt = -1;
		const APlayerController* BreakPC = GetWorld()->GetFirstPlayerController();
		APawn* BreakPawn = BreakPC ? BreakPC->GetPawn() : nullptr;
		UAbilitySystemComponent* BreakASC = FindASC(BreakPawn);
		const ULyraHealthComponent* BreakHC = ULyraHealthComponent::FindHealthComponent(BreakPawn);
		if (BreakASC && BreakHC) BreakASC->SetNumericAttributeBase(ULyraHealthSet::GetHealthAttribute(), FMath::Max(1.f, BreakHC->GetHealth() - 10.f));
		static const FGameplayTag BreakElim = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.Score.Eliminations"));
		if (ULyraTeamSubsystem* BreakTeams = GetWorld()->GetSubsystem<ULyraTeamSubsystem>()) BreakTeams->AddTeamTagStack(LyraTeamOf(Team), BreakElim, 1);
		const ULyraQuickBarComponent* BreakBar = BreakPC ? BreakPC->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
		if (ULyraInventoryItemInstance* BreakItem = BreakBar ? BreakBar->GetActiveSlotItem() : nullptr)
		{
			BreakItem->AddStatTagStack(FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.MagazineAmmo")), 7);
		}
		UE_LOG(LogMOB, Log, TEXT("MOB_BREAK t=%.1f hp-10 score+1 mag+7"), T);
	}
	TickSelfVisual(Now, DeltaTime);
	TickRemotes(Now);
	TickPickups(Now);
	FlushSend();
	TickCapture(T);

	if (Socket && Now >= NextLogAt)
	{
		NextLogAt = Now + 5.0;
		const APlayerController* LogPC = GetWorld()->GetFirstPlayerController();
		const APawn* LogPawn = LogPC ? LogPC->GetPawn() : nullptr;
		const FVector LyraAt = LogPawn ? LogPawn->GetActorLocation() : FVector::ZeroVector;
		UE_LOG(LogMOB, Log, TEXT("MOB_PROGRESS t=%.1f sent=%lld ack=%u states=%lld hp=%.0f mag=%d spare=%d weapon=%u flags=%u phase=%u score=%u:%u server=(%.0f,%.0f,%.0f) lyra=(%.0f,%.0f,%.0f) pawn=%s compared=%lld corrections=%lld remotes=%d skipped=%lld"),
			T, InputsSent, AckSeq, StatesRecv, ServerHp, ServerMagazine, ServerSpare, ServerWeapon, ServerFlags, MatchPhase, ScoreA, ScoreB, LastServerPosX, LastServerPosY, LastServerPosZ, LyraAt.X, LyraAt.Y, LyraAt.Z, *GetNameSafe(LogPawn), PosDiffCount, Corrections, RemoteSpawned, InputsSkipped);
	}

	if (ResultAt > 0 && bAutoInput && Now - ResultAt > 1.5) FinishAndQuit(TEXT("result"));	// 사람이면 결과 화면을 볼 수 있게 둔다
	else if (QuitAt > 0 && T > QuitAt) FinishAndQuit(TEXT("quit-at"));
}

// ─────────────────────────────────────────────────────────────
//  소켓
// ─────────────────────────────────────────────────────────────

void UMOBridgeSubsystem::Connect()
{
	FString Host = ServerAddr, PortStr = TEXT("7791");
	ServerAddr.Split(TEXT(":"), &Host, &PortStr);

	ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	TSharedRef<FInternetAddr> Addr = SS->CreateInternetAddr();
	bool bValid = false;
	Addr->SetIp(*Host, bValid);
	Addr->SetPort(FCString::Atoi(*PortStr));
	Socket = SS->CreateSocket(NAME_Stream, TEXT("MOBridge"), false);
	if (!bValid || !Socket || !Socket->Connect(*Addr))
	{
		UE_LOG(LogMOB, Error, TEXT("MOB_CONNECT_FAIL addr=%s"), *ServerAddr);
		FinishAndQuit(TEXT("connect-fail"));
		return;
	}
	Socket->SetNonBlocking(true);
	Socket->SetNoDelay(true);

	// 예측은 서버와 같은 맵·같은 코드로 — 맵을 못 읽으면 계속해도 의미가 없다
	const FString MapPath = FPaths::ProjectSavedDir() / TEXT("MOB") / TEXT("map_expanse.mocl");
	TArray<uint8> MapBytes;
	Core = new FMOBCore();
	if (!FFileHelper::LoadFileToArray(MapBytes, *MapPath) || Core->Map.LoadFromMemory(MapBytes.GetData(), (size_t)MapBytes.Num()) != mo::EMapLoad::Ok)
	{
		UE_LOG(LogMOB, Error, TEXT("MOB_MAP_FAIL path=%s bytes=%d"), *MapPath, MapBytes.Num());
		FinishAndQuit(TEXT("map-fail"));
		return;
	}
	ULyraCharacterMovementComponent::MOBMoveHook = &NoLyraMove;

	wire::Hello Hello = wire::Hello();
	Hello.h.size = sizeof(Hello);
	Hello.h.type = (uint16)wire::Msg::C2S_HELLO;
	Hello.protocolVersion = wire::kProtocolVersion;
	QueuePacket(&Hello, sizeof(Hello));
	FlushSend();
	UE_LOG(LogMOB, Log, TEXT("MOB_CONNECTED addr=%s protocol=%u"), *ServerAddr, Hello.protocolVersion);
}

void UMOBridgeSubsystem::QueuePacket(const void* Pkt, int32 Size)
{
	SendBuf.Append(static_cast<const uint8*>(Pkt), Size);
}

void UMOBridgeSubsystem::FlushSend()
{
	if (!Socket || SendBuf.Num() == 0) return;
	int32 Sent = 0;
	Socket->Send(SendBuf.GetData(), SendBuf.Num(), Sent);
	if (Sent > 0) SendBuf.RemoveAt(0, Sent);
	if (SendBuf.Num() > 0) ++SendShort;
}

void UMOBridgeSubsystem::PumpSocket()
{
	if (!Socket) return;
	uint32 PendingBytes = 0;
	while (Socket->HasPendingData(PendingBytes) && PendingBytes > 0)
	{
		const int32 Old = RecvBuf.Num();
		RecvBuf.SetNumUninitialized(Old + (int32)PendingBytes);
		int32 Read = 0;
		Socket->Recv(RecvBuf.GetData() + Old, (int32)PendingBytes, Read);
		RecvBuf.SetNum(Old + FMath::Max(Read, 0));
		if (Read <= 0) break;
	}

	int32 Pos = 0;
	while (RecvBuf.Num() - Pos >= (int32)sizeof(wire::Header))
	{
		wire::Header H;
		FMemory::Memcpy(&H, RecvBuf.GetData() + Pos, sizeof(H));
		if (H.size < sizeof(wire::Header))
		{
			UE_LOG(LogMOB, Error, TEXT("MOB_BAD_HEADER size=%u"), H.size);
			FinishAndQuit(TEXT("bad-header"));
			return;
		}
		if (RecvBuf.Num() - Pos < H.size) break;
		HandlePacket(RecvBuf.GetData() + Pos, H.size);
		Pos += H.size;
	}
	if (Pos > 0) RecvBuf.RemoveAt(0, Pos);

	// 상대가 닫아도 HasPendingData 는 0 바이트라 알려 주지 않고, GetConnectionState 도 Connected 로 남는다 — 들여다보기로 확인한다
	// 받을 것이 없으면 성공(0 바이트), 상대가 닫았거나 끊겼으면 실패다
	uint8 Peek = 0;
	int32 PeekRead = 0;
	const bool bPeerGone = !Socket->Recv(&Peek, 1, PeekRead, ESocketReceiveFlags::Peek);

	// 결과를 받은 뒤의 끊김은 정상이다 — 서버가 방을 닫는 것이고 사람은 결과 화면을 보는 중이다
	if ((bPeerGone || Socket->GetConnectionState() != SCS_Connected) && !bQuitting && ResultAt <= 0)
	{
		FinishAndQuit(TEXT("disconnected"));
	}
}

void UMOBridgeSubsystem::HandlePacket(const uint8* Data, int32 Size)
{
	wire::Header H;
	FMemory::Memcpy(&H, Data, sizeof(H));
	switch ((wire::Msg)H.type)
	{
	case wire::Msg::S2C_WELCOME:
	{
		wire::Welcome M; FMemory::Memcpy(&M, Data, FMath::Min<int32>(Size, sizeof(M)));
		PlayerId = M.playerId;
		Team = M.team;
		bWelcomed = true;
		UE_LOG(LogMOB, Log, TEXT("MOB_WELCOME player=%u team=%u tick=%u debugAllowed=%u"), M.playerId, M.team, M.tick, M.debugAllowed);
		break;
	}
	case wire::Msg::S2C_SPAWN:
	{
		wire::Spawn M; FMemory::Memcpy(&M, Data, FMath::Min<int32>(Size, sizeof(M)));
		++SpawnsRecv;
		if (M.player == PlayerId && bWelcomed)
		{
			bHasSpawn = true;
			Weapon = 0;
			AutoWeapon = 0;
			const FVector At(M.x, M.y, M.z);
			APlayerController* PC = GetWorld()->GetFirstPlayerController();
			APawn* Pawn = PC ? PC->GetPawn() : nullptr;
			const ULyraHealthComponent* HC = ULyraHealthComponent::FindHealthComponent(Pawn);
			if (PC && (!Pawn || (HC && HC->IsDeadOrDying())))
			{
				// 부활은 서버가 정한다 — 쓰러지는 중이어도 지금 새 캐릭터로
				if (ALyraCharacter* Old = Cast<ALyraCharacter>(Pawn)) Old->Reset();
				if (ALyraGameMode* GM = Cast<ALyraGameMode>(GetWorld()->GetAuthGameMode())) GM->RestartPlayer(PC);
				Pawn = PC->GetPawn();
				++SelfRespawns;
				UE_LOG(LogMOB, Log, TEXT("MOB_RESPAWN me pawn=%s"), *GetNameSafe(Pawn));
			}
			if (Pawn)
			{
				const FRotator Rot(0.f, M.yaw, 0.f);
				const bool bMoved = Pawn->TeleportTo(At, Rot);
				bSelfSet = false;
				UE_LOG(LogMOB, Log, TEXT("MOB_TELEPORT moved=%d now=(%.1f,%.1f,%.1f) pawn=%s"), bMoved ? 1 : 0, Pawn->GetActorLocation().X, Pawn->GetActorLocation().Y, Pawn->GetActorLocation().Z, *GetNameSafe(Pawn));
				PC->SetControlRotation(Rot);
			}
			if (Core)
			{
				Core->Prediction.OnSpawn(M.x, M.y, M.z, M.lastSeq);
				SelfCur = ToFVector(Core->Prediction.GetMove().location);
				SelfPrev = SelfCur;
				VisualError = FVector::ZeroVector;
			}
			UE_LOG(LogMOB, Log, TEXT("MOB_SPAWN me tick=%u lastSeq=%u at=(%.1f,%.1f,%.1f) yaw=%.1f start=%u"), M.tick, M.lastSeq, M.x, M.y, M.z, M.yaw, M.startIndex);
		}
		break;
	}
	case wire::Msg::S2C_STATE:
	{
		wire::State M; FMemory::Memcpy(&M, Data, FMath::Min<int32>(Size, sizeof(M)));
		++StatesRecv;
		bool bFoundMe = false;
		const double TickSample = (double)M.tick - FPlatformTime::Seconds() * wire::kTickRate;
		if (!bHasTickClock || FMath::Abs(TickSample - TickClockOffset) > 30.0)
		{
			TickClockOffset = TickSample;
			bHasTickClock = true;
		}
		else
		{
			TickClockOffset += 0.02 * (TickSample - TickClockOffset);
		}
		for (int32 i = 0; i < M.count && i < wire::kMaxPlayersInRoom; ++i)
		{
			const wire::PlayerState& P = M.players[i];
			if (P.id != PlayerId)
			{
				AddRemoteSnapshot(P, M.tick);
				continue;
			}
			bFoundMe = true;
			AckSeq = P.lastSeq;
			ServerHp = P.hp;
			ServerMagazine = P.magazine;
			ServerSpare = P.spare;
			ServerFlags = P.flags;
			ServerWeapon = P.weapon;
			LastServerPosX = P.x; LastServerPosY = P.y; LastServerPosZ = P.z;
			MaxAckLag = FMath::Max(MaxAckLag, (int32)(Seq - P.lastSeq));
			ApplySelfState(P);
			SyncSelfWeapon(P);
			if (!(P.flags & wire::STATE_DEAD)) CompareHp(FindPlayerPawn(PlayerId), P.hp);
		}
		if (!bFoundMe) ++StatesWithoutMe;
		break;
	}
	case wire::Msg::S2C_SHOT:
	{
		wire::Shot M; FMemory::Memcpy(&M, Data, FMath::Min<int32>(Size, sizeof(M)));
		HandleShot(M);
		break;
	}
	case wire::Msg::S2C_DAMAGE:
	{
		wire::Damage M; FMemory::Memcpy(&M, Data, FMath::Min<int32>(Size, sizeof(M)));
		++DamagesRecv;
		ApplyServerHp(M.target, M.attacker, M.hpAfter);
		break;
	}
	case wire::Msg::S2C_MATCH:
	{
		wire::Match M; FMemory::Memcpy(&M, Data, FMath::Min<int32>(Size, sizeof(M)));
		ApplyMatch(M);
		break;
	}
	case wire::Msg::S2C_KILL:
	{
		wire::Kill M; FMemory::Memcpy(&M, Data, FMath::Min<int32>(Size, sizeof(M)));
		HandleKill(M);
		break;
	}
	case wire::Msg::S2C_RESULT:
	{
		wire::Result M; FMemory::Memcpy(&M, Data, FMath::Min<int32>(Size, sizeof(M)));
		ResultAt = FPlatformTime::Seconds();
		UE_LOG(LogMOB, Log, TEXT("MOB_RESULT tick=%u score=%u:%u winner=%u players=%u"), M.tick, M.teamScoreA, M.teamScoreB, M.winningTeam, M.count);
		break;
	}
	default:
		++UnknownPackets;
		UE_LOG(LogMOB, Warning, TEXT("MOB_UNKNOWN type=%u size=%d"), H.type, Size);
	}
}

// ─────────────────────────────────────────────────────────────
//  내 캐릭터 — 입력을 서버와 같은 코드로 미리 돌리고, 서버가 다르다고 하면 그 값에서 다시 돌린다
// ─────────────────────────────────────────────────────────────

void UMOBridgeSubsystem::ApplySelfState(const wire::PlayerState& P)
{
	if (!Core || !bHasSpawn) return;
	// 어긋난 처음 몇 줄 — 어디서부터 다른지 보려고
	static int32 DiffLinesLeft = 40;
	if (const mo::CSelfPrediction::Slot* S = DiffLinesLeft > 0 ? Core->Prediction.FindComparable(P) : nullptr)
	{
		const double DiffNow = FVector::Dist(FVector(P.x, P.y, P.z), FVector((float)S->move.location.x, (float)S->move.location.y, (float)S->move.location.z));
		const double VelNow = FVector::Dist(FVector(P.vx, P.vy, P.vz), FVector((float)S->move.velocity.x, (float)S->move.velocity.y, (float)S->move.velocity.z));
		if (DiffNow > kCorrectTolerance || VelNow > kCorrectTolerance)
		{
			--DiffLinesLeft;
			UE_LOG(LogMOB, Log, TEXT("MOB_DIFF seq=%u cur=%u server=(%.3f,%.3f,%.3f)(%.3f,%.3f,%.3f) f=%u slot=(%.3f,%.3f,%.3f)(%.3f,%.3f,%.3f) m=%d in=(%.3f,%.3f) btn=%u lock=%d"),
				P.lastSeq, Seq, P.x, P.y, P.z, P.vx, P.vy, P.vz, P.flags,
				S->move.location.x, S->move.location.y, S->move.location.z, S->move.velocity.x, S->move.velocity.y, S->move.velocity.z, (int32)S->move.mode,
				S->input.moveX, S->input.moveY, S->input.buttons, S->lockInputs);
		}
	}
	const mo::CSelfPrediction::ReconcileResult R = Core->Prediction.Reconcile(P, kCorrectTolerance);
	if (R.compared)
	{
		++PosDiffCount;
		PosDiffSum += R.posDiff;
		PosDiffMax = FMath::Max(PosDiffMax, R.posDiff);
		VelDiffMax = FMath::Max(VelDiffMax, R.velDiff);
		if (R.posDiff > 0) ++PosDiffNonZero;
	}
	if (!R.corrected) return;

	// 계산은 바로 고치고, 화면은 그 차이만큼 남겨 뒀다가 서서히 없앤다
	const FVector After = ToFVector(Core->Prediction.GetMove().location);
	const FVector Delta = SelfCur - After;
	const double Distance = Delta.Size();
	++Corrections;
	CorrectionSum += Distance;
	CorrectionMax = FMath::Max(CorrectionMax, Distance);
	SelfCur = After;
	SelfPrev -= Delta;
	VisualError += Delta;
	if (VisualError.Size() > kVisualSnapDistance)
	{
		VisualError = FVector::ZeroVector;
		++VisualSnaps;
	}
}

void UMOBridgeSubsystem::TickSelfVisual(double Now, float DeltaTime)
{
	if (!Core || !bHasSpawn) return;
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ACharacter* Ch = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	if (!Ch) return;
	const ULyraHealthComponent* HC = ULyraHealthComponent::FindHealthComponent(Ch);
	if (HC && HC->IsDeadOrDying()) return;	// 쓰러지는 모습은 Lyra 몫 — 자리를 덮지 않는다

	const FName PawnName = Ch->GetFName();
	if (!LastPawnName.IsNone() && PawnName != LastPawnName)
	{
		++PawnChanges;
		bSelfSet = false;
		UE_LOG(LogMOB, Warning, TEXT("MOB_PAWN_CHANGED %s -> %s"), *LastPawnName.ToString(), *PawnName.ToString());
	}
	LastPawnName = PawnName;

	// 내가 지난 프레임에 놓은 자리에서 누가 옮겼나 — 0 이어야 Lyra 이동이 꺼진 것이다
	if (bSelfSet)
	{
		const double Drift = FVector::Dist(Ch->GetActorLocation(), SelfLastSet);
		SelfDriftMax = FMath::Max(SelfDriftMax, Drift);
		if (Drift > 1.0) ++SelfDriftOver1cm;
	}

	const mo::MoveState& Move = Core->Prediction.GetMove();
	const bool bWaiting = Core->Prediction.IsWaitingSpawn();
	const double Alpha = FMath::Clamp(1.0 - (NextInputAt - Now) / kInputSeconds, 0.0, 1.0);
	VisualError *= FMath::Exp(-kVisualErrorDecay * DeltaTime);
	const FVector Shown = FMath::Lerp(SelfPrev, SelfCur, Alpha) + VisualError;

	Ch->SetActorLocation(Shown, false, nullptr, ETeleportType::TeleportPhysics);
	SelfLastSet = Ch->GetActorLocation();
	bSelfSet = true;
	ApplyMoveToCharacter(Ch, bWaiting ? FVector::ZeroVector : ToFVector(Move.velocity), !bWaiting && Move.mode == mo::EMoveMode::Falling);
}

// ─────────────────────────────────────────────────────────────
//  남의 캐릭터 — 서버 상태를 0.1초 뒤 시점으로 입힌다. 위치·속도는 서버 값, 애니는 Lyra 그대로
// ─────────────────────────────────────────────────────────────

void UMOBridgeSubsystem::AddRemoteSnapshot(const wire::PlayerState& P, uint32 ServerTick)
{
	FMOBRemote& R = Remotes.FindOrAdd(P.id);
	R.Id = P.id;
	R.ServerTeam = P.team;
	FMOBSnapshot& S = R.Snaps[R.SnapCount % FMOBRemote::kSnapshots];
	S.Tick = ServerTick;
	S.Pos = FVector(P.x, P.y, P.z);
	S.Vel = FVector(P.vx, P.vy, P.vz);
	S.Yaw = P.yaw;
	S.Pitch = P.pitch;
	S.MoveX = P.moveX;
	S.MoveY = P.moveY;
	S.bFalling = (P.flags & wire::STATE_FALLING) != 0;
	++R.SnapCount;
	R.Weapon = P.weapon;

	if (P.flags & wire::STATE_DEAD)
	{
		R.bKilled = true;
		R.bRespawnPending = false;
	}
	else if (R.bKilled && !R.bRespawnPending)
	{
		R.bRespawnPending = true;
		R.RespawnTick = ServerTick;
	}
	if (!R.bKilled) CompareHp(R.Pawn.Get(), P.hp);
}

// Lyra 봇 생성 순서를 그대로 밟는다(LyraBotCreationComponent.cpp:100-125). C++ 컨트롤러라 두뇌(행동 트리) 없이 뜬다
void UMOBridgeSubsystem::SpawnRemote(FMOBRemote& R)
{
	UWorld* World = GetWorld();
	UClass* BotClass = LoadClass<AAIController>(nullptr, TEXT("/Script/LyraGame.LyraPlayerBotController"));
	ALyraGameMode* GM = Cast<ALyraGameMode>(World->GetAuthGameMode());
	if (!BotClass || !GM)
	{
		R.bSpawnFailed = true;
		++RemoteSpawnFail;
		UE_LOG(LogMOB, Error, TEXT("MOB_REMOTE_FAIL id=%u botClass=%d gm=%d"), R.Id, BotClass ? 1 : 0, GM ? 1 : 0);
		return;
	}

	if (AAIController* Existing = R.Ctrl.Get())
	{
		// 부활 — 컨트롤러·이름·팀은 그대로, 캐릭터만 새로
		// 죽을 때 Lyra 가 이동 입력을 막는다(LyraCharacter.cpp:354) — Lyra 봇은 ServerRestartController 에서 풀지만 여기는 그 길을 안 탄다.
		// 안 풀면 부활한 캐릭터의 AddMovementInput 이 버려져 가속이 0 이고, 애니가 Idle 인 채 미끄러진다
		Existing->ResetIgnoreInputFlags();
		GM->RestartPlayer(Existing);
		ACharacter* Fresh = Cast<ACharacter>(Existing->GetPawn());
		if (!Fresh)
		{
			R.bSpawnFailed = true;
			++RemoteSpawnFail;
			UE_LOG(LogMOB, Error, TEXT("MOB_REMOTE_FAIL id=%u respawn"), R.Id);
			return;
		}
		if (ULyraPawnExtensionComponent* Ext = ULyraPawnExtensionComponent::FindPawnExtensionComponent(Fresh)) Ext->CheckDefaultInitialization();
		R.Pawn = Fresh;
		R.ShownWeapon = 0xFF;
		R.Equipped = nullptr;
		++RemoteRespawns;
		return;
	}

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SP.ObjectFlags |= RF_Transient;
	AAIController* C = World->SpawnActor<AAIController>(BotClass, FVector::ZeroVector, FRotator::ZeroRotator, SP);
	if (!C)
	{
		R.bSpawnFailed = true;
		++RemoteSpawnFail;
		UE_LOG(LogMOB, Error, TEXT("MOB_REMOTE_FAIL id=%u controller"), R.Id);
		return;
	}
	C->bSetControlRotationFromPawnOrientation = false;	// 시선은 서버 값 — 안 끄면 AI 컨트롤러가 몸 방향으로 덮는다
	if (C->PlayerState) C->PlayerState->SetPlayerName(FString::Printf(TEXT("Player%u"), R.Id));

	GM->GenericPlayerInitialization(C);
	GM->RestartPlayer(C);
	ACharacter* Ch = Cast<ACharacter>(C->GetPawn());
	if (!Ch)
	{
		R.bSpawnFailed = true;
		++RemoteSpawnFail;
		UE_LOG(LogMOB, Error, TEXT("MOB_REMOTE_FAIL id=%u pawn"), R.Id);
		return;
	}
	if (ULyraPawnExtensionComponent* Ext = ULyraPawnExtensionComponent::FindPawnExtensionComponent(Ch)) Ext->CheckDefaultInitialization();

	// 색만 맞춘다 — 피해 판정은 서버 몫이라 Lyra 팀은 화면에만 쓰인다
	APlayerController* PC = World->GetFirstPlayerController();
	IGenericTeamAgentInterface* LocalTA = PC ? Cast<IGenericTeamAgentInterface>(PC->PlayerState) : nullptr;
	IGenericTeamAgentInterface* RemoteTA = Cast<IGenericTeamAgentInterface>(C->PlayerState);
	const int32 LocalLyraTeam = LocalTA ? LocalTA->GetGenericTeamId().GetId() : 1;
	const int32 WantLyraTeam = R.ServerTeam == Team ? LocalLyraTeam : (LocalLyraTeam == 1 ? 2 : 1);
	if (RemoteTA) RemoteTA->SetGenericTeamId(FGenericTeamId((uint8)WantLyraTeam));

	R.Ctrl = C;
	R.Pawn = Ch;
	++RemoteSpawned;
	UE_LOG(LogMOB, Log, TEXT("MOB_REMOTE id=%u pawn=%s serverTeam=%u lyraTeam=%d localLyraTeam=%d"), R.Id, *GetNameSafe(Ch), R.ServerTeam, WantLyraTeam, LocalLyraTeam);
}

void UMOBridgeSubsystem::TickRemotes(double Now)
{
	if (!Core || !bHasTickClock || ResultAt > 0) return;	// 경기가 끝나면 상태가 안 온다 — 그 자리에 둔다
	const double RenderTick = Now * wire::kTickRate + TickClockOffset - kRemoteDelayTicks;
	ShownRenderTick = RenderTick;
	bool bSpawnedThisFrame = false;

	for (TPair<uint32, FMOBRemote>& Pair : Remotes)
	{
		FMOBRemote& R = Pair.Value;
		if (R.bKilled)
		{
			// 쓰러지는 모습은 Lyra 몫이다. 살아난 틱이 보여 줄 시점에 닿으면 새 캐릭터로
			if (!R.bRespawnPending || RenderTick < (double)R.RespawnTick) continue;
			if (ACharacter* Old = R.Pawn.Get())
			{
				if (ALyraCharacter* LyraOld = Cast<ALyraCharacter>(Old)) LyraOld->Reset();
			}
			R.Pawn = nullptr;
			R.bKilled = false;
			R.bRespawnPending = false;
			R.bHasSet = false;
		}
		if (!R.Pawn.IsValid())
		{
			// 한 프레임에 하나만 — 캐릭터 만들기가 무거워 입력이 밀리지 않게
			if (!R.bSpawnFailed && !bSpawnedThisFrame)
			{
				bSpawnedThisFrame = true;
				SpawnRemote(R);
			}
			if (!R.Pawn.IsValid()) continue;
		}
		if (R.SnapCount == 0) continue;
		ACharacter* Ch = R.Pawn.Get();
		SyncRemoteWeapon(R, Ch);

		// 내가 지난 프레임에 놓은 자리·방향에서 누가 옮겼나
		if (R.bHasSet)
		{
			const double Drift = FVector::Dist(Ch->GetActorLocation(), R.LastSet);
			RemoteDriftMax = FMath::Max(RemoteDriftMax, Drift);
			if (Drift > 1.0) ++RemoteDriftOver1cm;
			RemoteYawDiffMax = FMath::Max(RemoteYawDiffMax, (double)FMath::Abs(FRotator::NormalizeAxis(Ch->GetActorRotation().Yaw - R.LastSetYaw)));
		}

		// 보여 줄 시점을 사이에 둔 두 상태를 찾는다(틱마다 오므로 뒤에서 몇 칸이면 닿는다)
		const FMOBSnapshot& Newest = R.Snaps[(R.SnapCount - 1) % FMOBRemote::kSnapshots];
		const FMOBSnapshot* From = nullptr;
		const FMOBSnapshot* To = nullptr;
		const uint32 Available = FMath::Min<uint32>(R.SnapCount, FMOBRemote::kSnapshots);
		for (uint32 Back = 0; Back < Available; ++Back)
		{
			const FMOBSnapshot& S = R.Snaps[(R.SnapCount - 1 - Back) % FMOBRemote::kSnapshots];
			if ((double)S.Tick <= RenderTick)
			{
				From = &S;
				break;
			}
			To = &S;
		}

		FMOBSnapshot Shown;
		if (From && To)
		{
			const double Span = FMath::Max<double>((double)To->Tick - (double)From->Tick, 1.0);
			const double Alpha = FMath::Clamp((RenderTick - (double)From->Tick) / Span, 0.0, 1.0);
			Shown = *From;
			if (FVector::Dist(From->Pos, To->Pos) > wire::kRemoteTeleportDistance)
			{
				++RemoteTeleports;
			}
			else
			{
				Shown.Pos = FMath::Lerp(From->Pos, To->Pos, Alpha);
				Shown.Vel = FMath::Lerp(From->Vel, To->Vel, Alpha);
				Shown.Yaw = From->Yaw + (float)Alpha * FRotator::NormalizeAxis(To->Yaw - From->Yaw);
				Shown.Pitch = FMath::Lerp(From->Pitch, To->Pitch, (float)Alpha);
			}
		}
		else if (From)
		{
			Shown = *From;		// 새 상태가 아직 안 왔다 — 그 자리에 둔다
			++RemoteUnderrun;
		}
		else
		{
			Shown = *To;		// 쌓인 상태 전부가 보여 줄 시점보다 뒤다(막 나타남)
		}
		++RemoteFrames;
		RemoteLagSum += FVector::Dist(Shown.Pos, Newest.Pos);
		RemoteBehindSum += (double)Newest.Tick - RenderTick;

		const FRotator Rot(0.f, Shown.Yaw, 0.f);
		Ch->SetActorLocationAndRotation(Shown.Pos, Rot, false, nullptr, ETeleportType::TeleportPhysics);
		R.LastSet = Ch->GetActorLocation();
		R.LastSetYaw = Shown.Yaw;
		R.bHasSet = true;
		ApplyMoveToCharacter(Ch, Shown.Vel, Shown.bFalling);
		if (bViewLog)
		{
			RemotePitchWantMax = FMath::Max(RemotePitchWantMax, (double)FMath::Abs(Shown.Pitch));
			RemotePitchSeenMax = FMath::Max(RemotePitchSeenMax, (double)FMath::Abs(FRotator::NormalizeAxis(Ch->GetBaseAimRotation().Pitch)));
		}
		if (R.Ctrl.IsValid()) R.Ctrl->SetControlRotation(FRotator(Shown.Pitch, Shown.Yaw, 0.f));
		// 위치는 위에서 정했고, 입력은 애니가 읽는 가속을 만들 뿐이다
		if (Shown.MoveX != 0 || Shown.MoveY != 0) Ch->AddMovementInput(FVector(Shown.MoveX / 127.f, Shown.MoveY / 127.f, 0.f), 1.f);
		if (bAnimLog) MeasureRemoteAnim(Ch, R.Id, Shown.Vel, Shown.MoveX, Shown.MoveY, Now);
	}
}

// 진단 — 안 그려지는 캐릭터는 애니가 안 돌 수 있으므로 그려진 프레임만 상태를 센다. 내 캐릭터와 나란히 찍어 대조한다
void UMOBridgeSubsystem::MeasureRemoteAnim(ACharacter* Ch, uint32 Id, const FVector& Vel, int8 MoveX, int8 MoveY, double Now)
{
	USkeletalMeshComponent* Mesh = Ch->GetMesh();
	UAnimInstance* Anim = Mesh ? Mesh->GetAnimInstance() : nullptr;
	const UCharacterMovementComponent* Move = Ch->GetCharacterMovement();
	if (!Anim || !Move) return;

	const auto StateOf = [](UAnimInstance* AI)
	{
		const FAnimNode_StateMachine* SM = AI ? AI->GetStateMachineInstanceFromName(TEXT("LocomotionSM")) : nullptr;
		return SM ? SM->GetCurrentStateName() : FName(TEXT("NoMachine"));
	};
	const FName State = StateOf(Anim);
	const bool bRendered = Mesh->WasRecentlyRendered(0.2f);
	if (Vel.Size2D() > 100.0)
	{
		++AnimMoving;
		if (MoveX == 0 && MoveY == 0) ++AnimMovingNoInput;
		if (Move->GetCurrentAcceleration().IsNearlyZero()) ++AnimMovingNoAccel;
		if (bRendered)
		{
			++AnimMovingRendered;
			AnimStates.FindOrAdd(State)++;
		}
	}

	if (!bRendered || Now < NextAnimLogAt) return;
	NextAnimLogAt = Now + 0.5;
	FString Linked;
	Mesh->ForEachAnimInstance([&Linked, Anim](UAnimInstance* L) { if (L != Anim) Linked += GetNameSafe(L->GetClass()) + TEXT(" "); });
	FString SelfText = TEXT("none");
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		if (ACharacter* Me = Cast<ACharacter>(PC->GetPawn()))
		{
			UAnimInstance* MyAnim = Me->GetMesh() ? Me->GetMesh()->GetAnimInstance() : nullptr;
			FString MyLinked;
			if (MyAnim) Me->GetMesh()->ForEachAnimInstance([&MyLinked, MyAnim](UAnimInstance* L) { if (L != MyAnim) MyLinked += GetNameSafe(L->GetClass()) + TEXT(" "); });
			SelfText = FString::Printf(TEXT("state=%s speed=%.0f accel=%.0f mode=%d linked=[%s]"), *StateOf(MyAnim).ToString(), Me->GetVelocity().Size2D(),
				Me->GetCharacterMovement()->GetCurrentAcceleration().Size(), (int32)Me->GetCharacterMovement()->MovementMode.GetValue(), *MyLinked);
		}
	}
	UE_LOG(LogMOB, Log, TEXT("MOB_ANIM id=%u state=%s speed=%.0f compVel=%.0f accel=%.0f move=%d,%d mode=%d tick=%d anim=%s linked=[%s] | self %s"), Id, *State.ToString(), Vel.Size2D(), Ch->GetVelocity().Size2D(),
		Move->GetCurrentAcceleration().Size(), MoveX, MoveY, (int32)Move->MovementMode.GetValue(), (int32)Mesh->VisibilityBasedAnimTickOption, *GetNameSafe(Anim->GetClass()), *Linked, *SelfText);
}

// ─────────────────────────────────────────────────────────────
//  입력 — 프레임과 상관없이 초당 60개. 서버는 틱마다 하나씩 꺼낸다
// ─────────────────────────────────────────────────────────────

void UMOBridgeSubsystem::TickInput(double Now)
{
	if (!Socket || !Core || !bHasSpawn || ResultAt > 0) return;
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ACharacter* Ch = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	if (!Ch)
	{
		NextInputAt = 0;	// 캐릭터가 없는 동안 쌓인 몫을 몰아 보내지 않는다
		return;
	}

	if (NextInputAt <= 0) NextInputAt = Now;
	int32 Due = (int32)((Now - NextInputAt) / kInputSeconds) + (Now >= NextInputAt ? 1 : 0);
	if (Due <= 0) return;
	if (Due > kMaxCatchUpInputs)
	{
		InputsSkipped += Due - kMaxCatchUpInputs;
		Due = kMaxCatchUpInputs;
		NextInputAt = Now;
	}
	else
	{
		NextInputAt += Due * kInputSeconds;
	}

	// Lyra 입력은 AddMovementInput 을 앞·옆 따로 불러 대각선이면 길이가 1 을 넘는다. 서버 입력은 길이 1 이하가 약속이다
	FVector Move = Ch->GetLastMovementInputVector();
	Move.Z = 0;
	InputRawMax = FMath::Max(InputRawMax, Move.Size());
	if (FMath::Abs(Move.X) > 0.01 && FMath::Abs(Move.Y) > 0.01) ++InputsDiagonal;
	if (Move.SizeSquared() > 1.0) Move.Normalize();
	InputSentMax = FMath::Max(InputSentMax, (double)FVector2f((float)Move.X, (float)Move.Y).Size());

	const FRotator Control = PC->GetControlRotation();
	const FVector Location = Ch->GetActorLocation();
	const FVector Aim = TargetingStart(PC, Location);

	const bool bFire = bAutoInput ? bAutoFire : PC->IsInputKeyDown(EKeys::LeftMouseButton);
	if (bFire)
	{
		const FVector AimDelta = Aim - Location;
		const double AimDist = AimDelta.Size();
		const double AimHoriz = AimDelta.Size2D();
		AimDistMax = FMath::Max(AimDistMax, AimDist);
		AimHorizMax = FMath::Max(AimHorizMax, AimHoriz);
		if (AimDist > 120.0 || AimHoriz > 90.0)
		{
			++AimFarLocal;
			if (AimFarLinesLeft > 0)
			{
				--AimFarLinesLeft;
				FVector ViewLoc;
				FRotator ViewRot;
				PC->GetPlayerViewPoint(ViewLoc, ViewRot);
				UE_LOG(LogMOB, Warning, TEXT("MOB_AIM_FAR dist=%.1f horiz=%.1f pitch=%.1f ctrlPitch=%.1f camFromPawn=%.1f seq=%u"), AimDist, AimHoriz, ViewRot.Pitch, Control.Pitch, FVector::Dist(ViewLoc, Location), Seq);
			}
		}
	}
	const bool bJump = bAutoInput ? bAutoJump : PC->IsInputKeyDown(EKeys::SpaceBar);
	const bool bReloadDown = bAutoInput ? bAutoReload : PC->IsInputKeyDown(EKeys::R);

	uint8 Wanted = Weapon;
	if (bAutoInput) Wanted = AutoWeapon;
	else if (PC->IsInputKeyDown(EKeys::One)) Wanted = (uint8)mo::EWeapon::Pistol;
	else if (PC->IsInputKeyDown(EKeys::Two)) Wanted = (uint8)mo::EWeapon::Rifle;
	else if (PC->IsInputKeyDown(EKeys::Three)) Wanted = (uint8)mo::EWeapon::Shotgun;
	if (Wanted != Weapon) ++InputsWeaponChange;
	Weapon = Wanted;

	// 되감기 시점 = 사람이 보고 쏜 화면. 이 함수는 TickRemotes 앞에 돌므로 ShownRenderTick 은 지난 프레임에 상대를 놓은 시점이다
	// 시계가 아직 없으면 0 = 시점 없음(서버가 현재 자리로 판정)
	uint32 ViewTick = 0;
	float ViewAlpha = 0.f;
	if (ShownRenderTick >= 1.0)
	{
		const double ViewWhole = FMath::FloorToDouble(ShownRenderTick);
		ViewTick = (uint32)ViewWhole;
		ViewAlpha = (float)(ShownRenderTick - ViewWhole);
	}

	for (int32 i = 0; i < Due; ++i)
	{
		wire::Input In = wire::Input();
		In.h.size = sizeof(In);
		In.h.type = (uint16)wire::Msg::C2S_INPUT;
		In.moveX = (float)Move.X;
		In.moveY = (float)Move.Y;
		In.yaw = FRotator::NormalizeAxis(Control.Yaw);
		In.pitch = FRotator::NormalizeAxis(Control.Pitch);
		In.aimX = (float)Aim.X;
		In.aimY = (float)Aim.Y;
		In.aimZ = (float)Aim.Z;
		In.weapon = Weapon;
		In.viewTick = ViewTick;
		In.viewAlpha = ViewAlpha;
		if (bJump) In.buttons |= wire::BUTTON_JUMP;
		if (bFire) In.buttons |= wire::BUTTON_FIRE;
		if (bReloadDown && !bReloadWasDown) { In.buttons |= wire::BUTTON_RELOAD; ++InputsReload; }
		bReloadWasDown = bReloadDown;

		// 번호를 붙이고 여기서도 돌린다 — 서버 답을 기다리지 않고 바로 움직이려고
		Core->Prediction.AddInput(In);
		Seq = In.seq;
		if (bViewLog && bFire)
		{
			// 서버 REWIND 줄과 (seq, 상대) 로 잇는다 — 서버가 되돌린 자리 = 그때 화면 자리인가
			for (const TPair<uint32, FMOBRemote>& Pair : Remotes)
			{
				const FMOBRemote& R = Pair.Value;
				const ACharacter* Shown = R.Pawn.Get();
				if (!Shown || R.bKilled) continue;
				const FVector At = Shown->GetActorLocation();
				UE_LOG(LogMOB, Log, TEXT("MOB_VIEW seq=%u target=%u viewTick=%u alpha=%.4f x=%.3f y=%.3f z=%.3f yaw=%.3f"), In.seq, R.Id, ViewTick, ViewAlpha, At.X, At.Y, At.Z, Shown->GetActorRotation().Yaw);
			}
		}
		if (!Core->Prediction.IsWaitingSpawn())
		{
			SelfPrev = SelfCur;
			SelfCur = ToFVector(Core->Prediction.GetMove().location);
		}
		QueuePacket(&In, sizeof(In));
		++InputsSent;
		if (bFire) ++InputsFire;
		if (bJump) ++InputsJump;
	}
}

// 사람 없이 시험하는 입력 — 시선을 돌리며 앞·왼·뒤·오른으로 걷고, 점프·사격·재장전·무기 바꾸기를 번갈아 낸다
void UMOBridgeSubsystem::TickAutoInput(double T)
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ACharacter* Ch = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	if (!Ch || !bHasSpawn || T < kAutoStart) return;

	const double S = T - kAutoStart;
	static const FVector Dirs[8] = { FVector(1, 0, 0), FVector(1, -1, 0), FVector(0, -1, 0), FVector(-1, -1, 0), FVector(-1, 0, 0), FVector(-1, 1, 0), FVector(0, 1, 0), FVector(1, 1, 0) };
	FVector AutoMove = Dirs[(int32)(S / kAutoLeg) % 8];
	FRotator AutoRot(-5.f, (float)FMath::Fmod(S * 20.0, 360.0), 0.f);
	if (bAutoAim)
	{
		const FVector From = Ch->GetActorLocation();
		const ACharacter* Best = nullptr;
		const ACharacter* Nearest = nullptr;
		double BestDist = 0, NearestDist = 0;
		for (const TPair<uint32, FMOBRemote>& Pair : Remotes)
		{
			const FMOBRemote& R = Pair.Value;
			if (R.ServerTeam == Team || R.bKilled || !R.Pawn.IsValid()) continue;
			const double D = FVector::DistSquared(From, R.Pawn->GetActorLocation());
			if (!Nearest || D < NearestDist)
			{
				Nearest = R.Pawn.Get();
				NearestDist = D;
			}
			if (PC->LineOfSightTo(R.Pawn.Get()) && (!Best || D < BestDist))
			{
				Best = R.Pawn.Get();
				BestDist = D;
			}
		}
		bAutoSeesEnemy = Best != nullptr;
		// 안 보이면 다가간다 — 벽을 돌아가지는 않으므로 네 방향 걷기와 번갈아 끼임을 푼다
		if (Nearest && FMath::Fmod(S, 6.0) >= 2.0 && !(Best && BestDist < 1000000.0))
		{
			AutoMove = (Nearest->GetActorLocation() - From).GetSafeNormal2D();
		}
		if (Best)
		{
			AutoRot = (Best->GetActorLocation() + FVector(0, 0, 30) - TargetingStart(PC, From)).Rotation();
			if (T >= NextSeeShotAt)
			{
				NextSeeShotAt = T + 4.0;
				RequestEventShot(TEXT("see"), 0.3, 4);
			}
		}
	}
	PC->SetControlRotation(AutoRot);
	// 사람 입력처럼 앞·옆을 따로 넣는다 — 대각선이면 합친 길이가 1 을 넘는 경로를 그대로 탄다
	Ch->AddMovementInput(FVector::ForwardVector, (float)AutoMove.X);
	Ch->AddMovementInput(FVector::RightVector, (float)AutoMove.Y);

	const bool bJumpNow = FMath::Fmod(S, 5.0) < 0.1;
	if (bJumpNow && !bAutoJump) Ch->Jump();
	if (!bJumpNow && bAutoJump) Ch->StopJumping();
	bAutoJump = bJumpNow;

	const bool bImmuneNow = MatchPhase == (uint8)wire::EMatchPhase::Playing && (ServerFlags & wire::STATE_IMMUNE) && !(ServerFlags & wire::STATE_DEAD);
	const bool bFireNow = bImmuneNow || (bAutoAim ? bAutoSeesEnemy : FMath::Fmod(S, 5.0) >= 2.0 && FMath::Fmod(S, 5.0) < 4.0);
	if (bFireNow != bAutoFire)
	{
		// 사람이 누른 것과 같은 길 — 내 총의 불꽃·궤적은 Lyra 능력이 그린다. 피해는 막혀 있다
		static const FGameplayTag FireTag = FGameplayTag::RequestGameplayTag(TEXT("InputTag.Weapon.Fire"));
		if (ULyraAbilitySystemComponent* LASC = Cast<ULyraAbilitySystemComponent>(FindASC(Ch)))
		{
			if (bFireNow) LASC->AbilityInputTagPressed(FireTag);
			else LASC->AbilityInputTagReleased(FireTag);
		}
	}
	bAutoFire = bFireNow;
	bAutoReload = FMath::Fmod(S, 20.0) >= 19.0 || (bAutoAim && ServerMagazine == 0);
	if (bAutoReload != bAutoReloadPressed)
	{
		static const FGameplayTag ReloadTag = FGameplayTag::RequestGameplayTag(TEXT("InputTag.Weapon.Reload"), false);
		ULyraAbilitySystemComponent* LASC = Cast<ULyraAbilitySystemComponent>(FindASC(Ch));
		if (LASC && ReloadTag.IsValid())
		{
			if (bAutoReload) LASC->AbilityInputTagPressed(ReloadTag);
			else LASC->AbilityInputTagReleased(ReloadTag);
		}
		bAutoReloadPressed = bAutoReload;
	}
	AutoWeapon = (uint8)((int32)(S / 10.0) % (int32)mo::EWeapon::Count);
}

// ─────────────────────────────────────────────────────────────
//  캡처
// ─────────────────────────────────────────────────────────────

void UMOBridgeSubsystem::RequestEventShot(const TCHAR* Name, double Delay, int32 MaxCount)
{
	if (!bEventShots) return;
	int32& Count = EventShotCounts.FindOrAdd(Name);
	if (Count >= MaxCount) return;
	++Count;
	PendingEventShots.Emplace(FPlatformTime::Seconds() + Delay, FString::Printf(TEXT("%s_%s_%d.png"), *Tag, Name, Count));
}

void UMOBridgeSubsystem::TickCapture(double T)
{
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("MOB");
	// 한 프레임에 한 장 — 요청이 겹치면 앞의 것이 덮인다
	for (int32 i = 0; i < PendingEventShots.Num(); ++i)
	{
		if (FPlatformTime::Seconds() < PendingEventShots[i].Key) continue;
		const FString Path = Dir / PendingEventShots[i].Value;
		FScreenshotRequest::RequestScreenshot(Path, true, false);
		UE_LOG(LogMOB, Log, TEXT("MOB_SHOT t=%.2f path=%s"), T, *Path);
		PendingEventShots.RemoveAt(i);
		return;
	}
	if (ShotIndex < ShotAt.Num() && T >= ShotAt[ShotIndex])
	{
		const FString Path = Dir / FString::Printf(TEXT("%s_%02d.png"), *Tag, FMath::RoundToInt(ShotAt[ShotIndex]));
		FScreenshotRequest::RequestScreenshot(Path, true, false);
		UE_LOG(LogMOB, Log, TEXT("MOB_SHOT t=%.2f path=%s"), T, *Path);
		++ShotIndex;
	}
	TickRecord(T);
}

// 영상 — 원시 BGRA 프레임을 한 파일(Saved/MOB/rec_<tag>.bgra)에 이어 쓴다. mp4 는 바깥에서 ffmpeg 로 만든다(MOB_REC 줄에 크기·프레임 수·초)
void UMOBridgeSubsystem::TickRecord(double T)
{
	if (RecStart < 0 || bGrabDone) return;
	if (!Grabber && T >= RecStart && T <= RecStart + RecDur)
	{
		UGameEngine* GE = Cast<UGameEngine>(GEngine);
		TSharedPtr<FSceneViewport> SV = GE ? GE->SceneViewport : nullptr;
		if (!SV.IsValid()) { UE_LOG(LogMOB, Error, TEXT("MOB_REC_FAIL no scene viewport")); bGrabDone = true; return; }
		GrabSize = SV->GetSizeXY();
		Grabber = new FFrameGrabber(SV.ToSharedRef(), GrabSize);
		Grabber->StartCapturingFrames();
		RawWriter = IFileManager::Get().CreateFileWriter(*(FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("rec_%s.bgra"), *Tag)));
		UE_LOG(LogMOB, Log, TEXT("MOB_REC_START size=%dx%d"), GrabSize.X, GrabSize.Y);
	}
	if (!Grabber) return;

	if (T <= RecStart + RecDur)
	{
		Grabber->CaptureThisFrame(FFramePayloadPtr());
		DrainGrabber();
		return;
	}

	if (Grabber->IsCapturingFrames()) Grabber->StopCapturingFrames();
	DrainGrabber();
	if (!Grabber->HasOutstandingFrames())
	{
		Grabber->Shutdown();
		delete Grabber;
		Grabber = nullptr;
		if (RawWriter) { RawWriter->Close(); delete RawWriter; RawWriter = nullptr; }
		bGrabDone = true;
		const double Span = GrabLastAt - GrabFirstAt;
		UE_LOG(LogMOB, Log, TEXT("MOB_REC size=%dx%d frames=%d seconds=%.3f fps=%.2f"),
			GrabSize.X, GrabSize.Y, GrabbedFrames, Span, Span > 0 ? (GrabbedFrames - 1) / Span : 0.0);
	}
}

void UMOBridgeSubsystem::DrainGrabber()
{
	for (FCapturedFrameData& F : Grabber->GetCapturedFrames())
	{
		if (F.BufferSize != GrabSize || !RawWriter) continue;
		RawWriter->Serialize(F.ColorBuffer.GetData(), F.ColorBuffer.Num() * sizeof(FColor));
		const double Now = FPlatformTime::Seconds();
		if (GrabbedFrames == 0) GrabFirstAt = Now;
		GrabLastAt = Now;
		++GrabbedFrames;
	}
}

// ─────────────────────────────────────────────────────────────
//  끝 — 요약을 찍고 나간다
// ─────────────────────────────────────────────────────────────

void UMOBridgeSubsystem::FinishAndQuit(const TCHAR* Why)
{
	if (bViewLog) UE_LOG(LogMOB, Log, TEXT("MOB_VIEWPITCH want=%.3f seen=%.3f"), RemotePitchWantMax, RemotePitchSeenMax);
	if (bQuitting) return;
	bQuitting = true;

	UE_LOG(LogMOB, Log, TEXT("MOB_SUMMARY why=%s player=%u team=%u spawns=%lld sent=%lld skipped=%lld fire=%lld jump=%lld reload=%lld weaponChange=%lld ack=%u maxAckLag=%d sendShort=%lld"),
		Why, PlayerId, Team, SpawnsRecv, InputsSent, InputsSkipped, InputsFire, InputsJump, InputsReload, InputsWeaponChange, AckSeq, MaxAckLag, SendShort);
	UE_LOG(LogMOB, Log, TEXT("MOB_SUMMARY2 states=%lld statesWithoutMe=%lld shots=%lld damages=%lld matches=%lld kills=%lld unknown=%lld hp=%.0f mag=%d spare=%d phase=%u score=%u:%u posDiff n=%lld avg=%.1f max=%.1f"),
		StatesRecv, StatesWithoutMe, ShotsRecv, DamagesRecv, MatchesRecv, KillsRecv, UnknownPackets, ServerHp, ServerMagazine, ServerSpare, MatchPhase, ScoreA, ScoreB,
		PosDiffCount, PosDiffCount ? PosDiffSum / PosDiffCount : 0.0, PosDiffMax);
	UE_LOG(LogMOB, Log, TEXT("MOB_SUMMARY3 posNonZero=%lld velMax=%.3f corrections=%lld correctionAvg=%.2f correctionMax=%.2f visualSnaps=%lld pawnChanges=%lld selfDriftMax=%.3f selfDriftOver1cm=%lld maxFrameGap=%.3f"),
		PosDiffNonZero, VelDiffMax, Corrections, Corrections ? CorrectionSum / Corrections : 0.0, CorrectionMax, VisualSnaps, PawnChanges, SelfDriftMax, SelfDriftOver1cm, MaxFrameGap);
	UE_LOG(LogMOB, Log, TEXT("MOB_SUMMARY4 remotes=%d remoteFail=%d frames=%lld underrun=%lld teleports=%lld driftMax=%.3f driftOver1cm=%lld yawDiffMax=%.3f lagAvg=%.1f behindTicksAvg=%.2f"),
		RemoteSpawned, RemoteSpawnFail, RemoteFrames, RemoteUnderrun, RemoteTeleports, RemoteDriftMax, RemoteDriftOver1cm, RemoteYawDiffMax, RemoteFrames ? RemoteLagSum / RemoteFrames : 0.0, RemoteFrames ? RemoteBehindSum / RemoteFrames : 0.0);

	UE_LOG(LogMOB, Log, TEXT("MOB_PICKUPS hidden=%d hpFixes=%lld"), PickupsHidden, HpFixes);
	if (bAnimLog)
	{
		FString States;
		for (const TPair<FName, int64>& P : AnimStates) States += FString::Printf(TEXT("%s=%lld "), *P.Key.ToString(), P.Value);
		UE_LOG(LogMOB, Log, TEXT("MOB_ANIM_SUM moving=%lld rendered=%lld noInput=%lld noAccel=%lld states=[%s]"), AnimMoving, AnimMovingRendered, AnimMovingNoInput, AnimMovingNoAccel, *States);
	}
	UE_LOG(LogMOB, Log, TEXT("MOB_SUMMARY6 inputRawMax=%.3f inputSentMax=%.6f diagonal=%lld bulletsLyra=%lld bulletsServer=%lld bulletsLyraImmune=%lld bulletsServerImmune=%lld bulletsLyraOther=%lld bulletsServerOther=%lld magDiffMax=%d myShots=%lld,%lld,%lld reloadStart=%lld,%lld,%lld reloadDone=%lld,%lld,%lld aimFarLocal=%lld aimDistMax=%.1f aimHorizMax=%.1f"),
		InputRawMax, InputSentMax, InputsDiagonal, BulletsLyra, BulletsServer, BulletsLyraImmune, BulletsServerImmune, BulletsLyraOther, BulletsServerOther, MagDiffMax, MyShots[0], MyShots[1], MyShots[2], ReloadStarts[0], ReloadStarts[1], ReloadStarts[2], ReloadDones[0], ReloadDones[1], ReloadDones[2], AimFarLocal, AimDistMax, AimHorizMax);
	UE_LOG(LogMOB, Log, TEXT("MOB_SUMMARY5 damageApplied=%lld damageMismatch=%lld damageNoTarget=%lld hpCompared=%lld hpDiffCount=%lld hpDiffMax=%.3f selfRespawns=%lld remoteRespawns=%lld scoreFixes=%lld scoreServer=%u:%u scoreLyra=%d:%d ammoFixes=%lld weaponSwaps=%lld remoteEquips=%lld phaseChanges=%lld shotCues=%lld hitMarkers=%lld countDownSets=%lld countDownLast=%d"),
		DamageApplied, DamageMismatch, DamageNoTarget, HpCompared, HpDiffCount, HpDiffMax, SelfRespawns, RemoteRespawns, ScoreFixes, ScoreA, ScoreB,
		LyraScoreA, LyraScoreB,
		AmmoFixes, WeaponSwaps, RemoteEquips, PhaseChanges, ShotCues, HitMarkers, CountDownSets, LastCountDownShown);

	FPlatformMisc::RequestExit(false, TEXT("MOBridge"));
}

// ─────────────────────────────────────────────────────────────
//  도우미
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────
//  서버 결과를 Lyra 화면에 — 값은 전부 서버 것이고, Lyra 는 체력 바·죽는 모습·알림·점수판을 그대로 보여 준다
// ─────────────────────────────────────────────

APawn* UMOBridgeSubsystem::FindPlayerPawn(uint32 Id) const
{
	if (Id == 0) return nullptr;
	if (Id == PlayerId)
	{
		const APlayerController* PC = GetWorld()->GetFirstPlayerController();
		return PC ? PC->GetPawn() : nullptr;
	}
	const FMOBRemote* R = Remotes.Find(Id);
	return R ? R->Pawn.Get() : nullptr;
}

APlayerState* UMOBridgeSubsystem::FindPlayerState(uint32 Id) const
{
	if (Id == 0) return nullptr;
	if (Id == PlayerId)
	{
		const APlayerController* PC = GetWorld()->GetFirstPlayerController();
		return PC ? PC->PlayerState : nullptr;
	}
	const FMOBRemote* R = Remotes.Find(Id);
	return R && R->Ctrl.IsValid() ? R->Ctrl->PlayerState : nullptr;
}

int32 UMOBridgeSubsystem::LyraTeamOf(uint8 ServerTeam) const
{
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const IGenericTeamAgentInterface* LocalTA = PC ? Cast<IGenericTeamAgentInterface>(PC->PlayerState) : nullptr;
	const int32 LocalLyraTeam = LocalTA ? LocalTA->GetGenericTeamId().GetId() : 1;
	return ServerTeam == Team ? LocalLyraTeam : (LocalLyraTeam == 1 ? 2 : 1);
}

// 받은 양이 아니라 서버 체력에 맞춘다 — 한 번 어긋나도 다음 피해에서 닫히게
void UMOBridgeSubsystem::ApplyServerHp(uint32 TargetId, uint32 AttackerId, float HpAfter)
{
	APawn* Target = FindPlayerPawn(TargetId);
	UAbilitySystemComponent* TargetASC = FindASC(Target);
	ULyraHealthComponent* HC = ULyraHealthComponent::FindHealthComponent(Target);
	if (!TargetASC || !HC)
	{
		++DamageNoTarget;
		return;
	}
	if (HC->IsDeadOrDying()) return;
	const float Amount = HC->GetHealth() - HpAfter;
	if (Amount <= 0.f) return;

	// 무적은 서버 판정이다 — 서버가 피해를 줬으면 Lyra 쪽 무적(시작 효과 등)이 막으면 안 된다
	if (TargetASC->HasMatchingGameplayTag(TAG_Gameplay_DamageImmunity))
	{
		TargetASC->RemoveActiveEffectsWithGrantedTags(FGameplayTagContainer(TAG_Gameplay_DamageImmunity));
	}

	APawn* Attacker = FindPlayerPawn(AttackerId);
	UAbilitySystemComponent* SourceASC = FindASC(Attacker);
	if (!SourceASC) SourceASC = TargetASC;

	FGameplayCueParameters Cue;
	Cue.RawMagnitude = Amount;
	Cue.NormalizedMagnitude = Amount;
	Cue.Location = Target->GetActorLocation();
	Cue.Normal = FVector::UpVector;
	Cue.Instigator = Attacker ? Attacker : Target;
	Cue.EffectCauser = Attacker ? Attacker : Target;
	Cue.EffectContext = SourceASC->MakeEffectContext();

	// 처치 알림·점수 규칙은 공격자 쪽 맥락(플레이어 상태 + 캐릭터)을 읽는다 — Lyra 무기 효과와 같은 모양
	FGameplayEffectSpecHandle Spec = SourceASC->MakeOutgoingSpec(UMOBServerDamageEffect::StaticClass(), 1.f, SourceASC->MakeEffectContext());
	Spec.Data->SetSetByCallerMagnitude(LyraGameplayTags::SetByCaller_Damage, Amount);
	SourceASC->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), TargetASC);
	++DamageApplied;
	if (!FMath::IsNearlyEqual(HC->GetHealth(), HpAfter, 0.01f))
	{
		++DamageMismatch;
		UE_LOG(LogMOB, Warning, TEXT("MOB_DAMAGE_MISMATCH target=%u lyra=%.2f server=%.2f immune=%d"), TargetId, HC->GetHealth(), HpAfter,
			TargetASC->HasMatchingGameplayTag(TAG_Gameplay_DamageImmunity) ? 1 : 0);
	}

	static const FGameplayTag DamageCue = FGameplayTag::RequestGameplayTag(TEXT("GameplayCue.Character.DamageTaken"), false);
	if (DamageCue.IsValid()) TargetASC->ExecuteGameplayCue(DamageCue, Cue);
}

void UMOBridgeSubsystem::CompareHp(const AActor* Actor, float ServerValue)
{
	const ULyraHealthComponent* HC = ULyraHealthComponent::FindHealthComponent(Actor);
	if (!HC || HC->IsDeadOrDying() || HC->GetMaxHealth() <= 0.f) return;
	const double Diff = FMath::Abs(HC->GetHealth() - ServerValue);
	++HpCompared;
	HpDiffMax = FMath::Max(HpDiffMax, Diff);
	if (Diff > 0.01)
	{
		++HpDiffCount;
		if (HpDiffLinesLeft > 0)
		{
			--HpDiffLinesLeft;
			double NearestSpawner = -1;
			for (TActorIterator<ALyraWeaponSpawner> It(GetWorld()); It; ++It)
			{
				const double D = FVector::Dist(It->GetActorLocation(), Actor->GetActorLocation());
				if (NearestSpawner < 0 || D < NearestSpawner) NearestSpawner = D;
			}
			UE_LOG(LogMOB, Warning, TEXT("MOB_HP_DIFF pawn=%s lyra=%.2f server=%.2f nearestPickup=%.0f"), *GetNameSafe(Actor), HC->GetHealth(), ServerValue, NearestSpawner);
		}
		// 체력은 서버 값이다 — 까닭이 무엇이든 다음 상태에서 닫는다. 0 은 죽음 흐름(처치 패킷)이 맡는다
		if (ServerValue > 0.f)
		{
			if (UAbilitySystemComponent* ASC = FindASC(const_cast<AActor*>(Actor)))
			{
				ASC->SetNumericAttributeBase(ULyraHealthSet::GetHealthAttribute(), ServerValue);
				++HpFixes;
			}
		}
	}
}

// 맵의 줍기 받침 — Lyra 혼자 체력·무기를 주면 서버와 어긋난다. 서버에 줍기가 생기기 전까지 숨긴다(월드 파티션이 나중에 올리는 것도 있어 계속 본다)
void UMOBridgeSubsystem::TickPickups(double Now)
{
	if (!bNamedSelf && PlayerId != 0)
	{
		// 기본 이름은 PC 이름이다 — 처치 알림에 그대로 보인다
		const APlayerController* PC = GetWorld()->GetFirstPlayerController();
		if (PC && PC->PlayerState)
		{
			PC->PlayerState->SetPlayerName(FString::Printf(TEXT("Player%u"), PlayerId));
			bNamedSelf = true;
		}
	}
	if (bKeepPickups || Now < NextPickupCheckAt) return;
	NextPickupCheckAt = Now + 1.0;
	for (TActorIterator<ALyraWeaponSpawner> It(GetWorld()); It; ++It)
	{
		if (It->IsHidden()) continue;
		It->SetActorEnableCollision(false);
		It->SetActorHiddenInGame(true);
		It->SetActorTickEnabled(false);
		++PickupsHidden;
	}
}

void UMOBridgeSubsystem::HandleKill(const wire::Kill& M)
{
	++KillsRecv;
	if (M.target == PlayerId && Core) Core->Prediction.BeginWaitSpawn();
	if (M.target == PlayerId)
	{
		RequestEventShot(TEXT("dead"), 0.6, 2);
		RequestEventShot(TEXT("deadlate"), 3.0, 2);
	}
	if (M.attacker == PlayerId)
	{
		RequestEventShot(TEXT("kill"), 0.15, 3);
		RequestEventShot(TEXT("killlate"), 1.0, 3);
	}
	if (FMOBRemote* R = Remotes.Find(M.target))
	{
		R->bKilled = true;
		R->bRespawnPending = false;
	}
	// 떨어져 죽으면 피해 패킷 없이 처치만 온다 — 체력 0 이 Lyra 죽음 흐름(모습·처치 알림)을 부른다
	ApplyServerHp(M.target, M.attacker, 0.f);

	ScoreA = M.teamScoreA;
	ScoreB = M.teamScoreB;
	SyncTeamScores();

	// 개인 기록 — Lyra 규칙이 이미 올렸어도 서버 값에 맞춘다
	static const FGameplayTag ElimTag = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.Score.Eliminations"));
	static const FGameplayTag DeathTag = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.Score.Deaths"));
	const auto SetStat = [this](APlayerState* PS, const FGameplayTag& Stat, int32 Want)
	{
		ALyraPlayerState* LPS = Cast<ALyraPlayerState>(PS);
		if (!LPS) return;
		const int32 Have = LPS->GetStatTagStackCount(Stat);
		if (Have == Want) return;
		++ScoreFixes;
		if (Have < Want) LPS->AddStatTagStack(Stat, Want - Have);
		else LPS->RemoveStatTagStack(Stat, Have - Want);
	};
	if (M.attacker != 0) SetStat(FindPlayerState(M.attacker), ElimTag, M.attackerKills);
	SetStat(FindPlayerState(M.target), DeathTag, M.targetDeaths);

	UE_LOG(LogMOB, Log, TEXT("MOB_KILL tick=%u attacker=%u target=%u weapon=%u weak=%u score=%u:%u kills=%u deaths=%u"),
		M.tick, M.attacker, M.target, M.weapon, M.weakSpot, M.teamScoreA, M.teamScoreB, M.attackerKills, M.targetDeaths);
}

void UMOBridgeSubsystem::SyncTeamScores()
{
	ULyraTeamSubsystem* Teams = GetWorld()->GetSubsystem<ULyraTeamSubsystem>();
	if (!Teams) return;
	static const FGameplayTag ElimTag = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.Score.Eliminations"));
	const auto Sync = [&](uint8 ServerTeam, int32 Want)
	{
		const int32 LyraTeam = LyraTeamOf(ServerTeam);
		const int32 Have = Teams->GetTeamTagStackCount(LyraTeam, ElimTag);
		if (Have == Want) return;
		++ScoreFixes;
		if (Have < Want) Teams->AddTeamTagStack(LyraTeam, ElimTag, Want - Have);
		else Teams->RemoveTeamTagStack(LyraTeam, ElimTag, Have - Want);
	};
	Sync(wire::kTeamA, ScoreA);
	Sync(wire::kTeamB, ScoreB);
}

// 타이머·목표 점수는 블루프린트(B_TeamDeathMatchScoring) 변수라 이름으로 찾아 넣는다
void UMOBridgeSubsystem::ForceCountDown(int32 Seconds, int32 TargetScore)
{
	AGameStateBase* GS = GetWorld()->GetGameState();
	UActorComponent* Scoring = nullptr;
	if (GS)
	{
		for (UActorComponent* C : GS->GetComponents())
		{
			if (C && C->GetClass()->GetName().StartsWith(TEXT("B_TeamDeathMatchScoring")))
			{
				Scoring = C;
				break;
			}
		}
	}
	const auto Missing = [this](const TCHAR* What)
	{
		if (bLoggedScoringMissing) return;
		bLoggedScoringMissing = true;
		UE_LOG(LogMOB, Warning, TEXT("MOB_SCORING_MISSING %s"), What);
	};
	if (!Scoring)
	{
		Missing(TEXT("component"));
		return;
	}
	UClass* Cls = Scoring->GetClass();

	// Lyra 자기 타이머를 끈다 — 안 끄면 0 에서 저 혼자 경기를 끝낸다(핸들은 블루프린트 지역 값이라 이름으로 못 찾는다)
	GetWorld()->GetTimerManager().ClearAllTimersForObject(Scoring);

	const auto SetAndNotify = [&](const TCHAR* Prop, int32 Value, const TCHAR* Rep)
	{
		FNumericProperty* N = FindFProperty<FNumericProperty>(Cls, Prop);
		if (!N)
		{
			Missing(Prop);
			return;
		}
		void* Ptr = N->ContainerPtrToValuePtr<void>(Scoring);
		if (N->IsFloatingPoint()) N->SetFloatingPointPropertyValue(Ptr, (double)Value);
		else N->SetIntPropertyValue(Ptr, (int64)Value);
		if (UFunction* F = Cls->FindFunctionByName(Rep)) Scoring->ProcessEvent(F, nullptr);	// 화면이 이 알림으로 갱신된다
		else Missing(Rep);
	};
	SetAndNotify(TEXT("TargetScore"), TargetScore, TEXT("OnRep_TargetScore"));
	SetAndNotify(TEXT("CountDownTime"), Seconds, TEXT("OnRep_CountDownTime"));
	++CountDownSets;
	LastCountDownShown = Seconds;
}

void UMOBridgeSubsystem::ApplyMatch(const wire::Match& M)
{
	++MatchesRecv;
	if (M.phase != MatchPhase)
	{
		++PhaseChanges;
		UE_LOG(LogMOB, Log, TEXT("MOB_MATCH tick=%u phase %u->%u left=%.1f target=%u score=%u:%u lyraBefore=%d:%d"), M.tick, MatchPhase, M.phase, M.secondsLeft, M.targetScore, M.teamScoreA, M.teamScoreB, LyraScoreA, LyraScoreB);
	}
	MatchPhase = M.phase;
	ScoreA = M.teamScoreA;
	ScoreB = M.teamScoreB;
	SyncTeamScores();
	if (const ULyraTeamSubsystem* Teams = GetWorld()->GetSubsystem<ULyraTeamSubsystem>())
	{
		static const FGameplayTag ElimTag = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.Score.Eliminations"));
		LyraScoreA = Teams->GetTeamTagStackCount(LyraTeamOf(wire::kTeamA), ElimTag);
		LyraScoreB = Teams->GetTeamTagStackCount(LyraTeamOf(wire::kTeamB), ElimTag);
	}

	if (!bPostGamePrepared)
	{
		// 끝 단계가 끝나면 Lyra 는 맵을 다시 연다 — 경기 끝은 서버 몫이고 결과 화면을 봐야 하니 그 시간을 길게 둔다
		bPostGamePrepared = true;
		UClass* PostPhase = LoadClass<ULyraGamePhaseAbility>(nullptr, TEXT("/ShooterCore/Experiences/Phases/Phase_PostGame.Phase_PostGame_C"));
		FNumericProperty* N = PostPhase ? FindFProperty<FNumericProperty>(PostPhase, TEXT("NewGameTime")) : nullptr;
		if (N)
		{
			void* Ptr = N->ContainerPtrToValuePtr<void>(PostPhase->GetDefaultObject());
			if (N->IsFloatingPoint()) N->SetFloatingPointPropertyValue(Ptr, 86400.0);
			else N->SetIntPropertyValue(Ptr, (int64)86400);
		}
		else
		{
			UE_LOG(LogMOB, Warning, TEXT("MOB_SCORING_MISSING NewGameTime"));
		}
	}

	const int32 Seconds = FMath::CeilToInt(M.secondsLeft);
	ForceCountDown(Seconds, M.targetScore);

	if ((wire::EMatchPhase)M.phase == wire::EMatchPhase::Warmup)
	{
		// 시작 전 숫자 — Lyra 대기 단계가 보내던 알림을 서버 남은 초로 보낸다
		static const FGameplayTag CountdownTag = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.GamePhase.MatchBeginCountdown"), false);
		if (CountdownTag.IsValid())
		{
			FLyraVerbMessage Msg;
			Msg.Verb = CountdownTag;
			Msg.Magnitude = (double)Seconds;
			UGameplayMessageSubsystem::Get(this).BroadcastMessage(CountdownTag, Msg);
		}
	}
	else if ((wire::EMatchPhase)M.phase >= wire::EMatchPhase::PostGame)
	{
		ULyraGamePhaseSubsystem* Phases = GetWorld()->GetSubsystem<ULyraGamePhaseSubsystem>();
		static const FGameplayTag PostTag = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.GamePhase.PostGame"));
		if (Phases && !Phases->IsPhaseActive(PostTag))
		{
			if (UClass* PostPhase = LoadClass<ULyraGamePhaseAbility>(nullptr, TEXT("/ShooterCore/Experiences/Phases/Phase_PostGame.Phase_PostGame_C")))
			{
				Phases->StartPhase(PostPhase);
			}
		}
	}
}

void UMOBridgeSubsystem::LoadWeaponClasses()
{
	if (WeaponItemDefs.Num() != 0) return;
	static const TCHAR* const Names[(int32)mo::EWeapon::Count] = { TEXT("Pistol"), TEXT("Rifle"), TEXT("Shotgun") };
	for (const TCHAR* Name : Names)
	{
		const FString Item = FString::Printf(TEXT("/ShooterCore/Weapons/%s/ID_%s.ID_%s_C"), Name, Name, Name);
		const FString Equip = FString::Printf(TEXT("/ShooterCore/Weapons/%s/WID_%s.WID_%s_C"), Name, Name, Name);
		WeaponItemDefs.Add(LoadClass<ULyraInventoryItemDefinition>(nullptr, *Item));
		WeaponEquipDefs.Add(LoadClass<ULyraEquipmentDefinition>(nullptr, *Equip));
		UE_LOG(LogMOB, Log, TEXT("MOB_WEAPON_CLASS %s item=%d equip=%d"), Name, WeaponItemDefs.Last() ? 1 : 0, WeaponEquipDefs.Last() ? 1 : 0);
	}
}

// 내 무기 — 첫 판 범위 셋을 빠른 칸에 넣어 두고, 고른 칸·탄은 서버 값에 맞춘다
void UMOBridgeSubsystem::SyncSelfWeapon(const wire::PlayerState& P)
{
	if (P.flags & wire::STATE_DEAD)
	{
		bWasReloading = false;
		return;
	}
	const bool bReloading = (P.flags & wire::STATE_RELOADING) != 0;
	if (bReloading && !bWasReloading && P.weapon < 3)
	{
		ReloadWeapon = P.weapon;
		ReloadMagBefore = P.magazine;
		++ReloadStarts[P.weapon];
	}
	if (!bReloading && bWasReloading && P.weapon == ReloadWeapon && P.magazine > ReloadMagBefore) ++ReloadDones[ReloadWeapon];
	bJustReloaded = !bReloading && bWasReloading;
	bWasReloading = bReloading;

	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ULyraQuickBarComponent* Bar = PC ? PC->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
	ULyraInventoryManagerComponent* Inv = PC ? PC->FindComponentByClass<ULyraInventoryManagerComponent>() : nullptr;
	if (!Bar || !Inv || !PC->GetPawn() || P.weapon >= (uint8)mo::EWeapon::Count) return;
	LoadWeaponClasses();

	int32 WantSlot = INDEX_NONE;
	for (int32 W = 0; W < WeaponItemDefs.Num(); ++W)
	{
		UClass* Def = WeaponItemDefs[W];
		if (!Def) continue;
		int32 Found = INDEX_NONE;
		const TArray<ULyraInventoryItemInstance*> Slots = Bar->GetSlots();
		for (int32 S = 0; S < Slots.Num(); ++S)
		{
			if (Slots[S] && Slots[S]->GetItemDef() == Def) Found = S;
		}
		if (Found == INDEX_NONE)
		{
			Found = Bar->GetNextFreeItemSlot();
			if (Found == INDEX_NONE) continue;
			if (ULyraInventoryItemInstance* Item = Inv->AddItemDefinition(Def, 1)) Bar->AddItemToSlot(Found, Item);
			UE_LOG(LogMOB, Log, TEXT("MOB_QUICKBAR_ADD weapon=%d slot=%d"), W, Found);
		}
		if (W == (int32)P.weapon) WantSlot = Found;
	}
	if (WantSlot == INDEX_NONE) return;
	if (Bar->GetActiveSlotIndex() != WantSlot)
	{
		++WeaponSwaps;
		Bar->SetActiveSlotIndex(WantSlot);
	}

	ULyraInventoryItemInstance* Active = Bar->GetActiveSlotItem();
	if (!Active) return;
	static const FGameplayTag MagTag = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.MagazineAmmo"));
	static const FGameplayTag SpareTag = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.SpareAmmo"));
	const auto SetStack = [&](const FGameplayTag& Stack, int32 Want)
	{
		const int32 Have = Active->GetStatTagStackCount(Stack);
		if (Have == Want) return;
		++AmmoFixes;
		if (Have < Want) Active->AddStatTagStack(Stack, Want - Have);
		else Active->RemoveStatTagStack(Stack, Have - Want);
	};
	// 지난 상태에서 Lyra 탄을 서버 값으로 맞춰 뒀다 — 지금까지 Lyra 가 쏜 수 ↔ 서버가 쏜 수. 재장전 중·끝난 직후·무기 바꾼 직후는 뺀다
	const int32 MagHave = Active->GetStatTagStackCount(MagTag);
	if (!bReloading && !bJustReloaded && LastSyncWeapon == P.weapon && LastSyncMag >= 0)
	{
		const int32 LyraShot = FMath::Max(0, LastSyncMag - MagHave);
		const int32 ServerShot = FMath::Max(0, LastSyncMag - (int32)P.magazine);
		// 무적은 시작 전·끝 단계에도 켜진다 — 부활 무적만 따로 보려고 경기 중과 나머지를 가른다
		const bool bPlaying = MatchPhase == (uint8)wire::EMatchPhase::Playing;
		const bool bImmune = (P.flags & wire::STATE_IMMUNE) != 0;
		if (!bPlaying)
		{
			BulletsLyraOther += LyraShot;
			BulletsServerOther += ServerShot;
		}
		else if (bImmune)
		{
			BulletsLyraImmune += LyraShot;
			BulletsServerImmune += ServerShot;
		}
		else
		{
			BulletsLyra += LyraShot;
			BulletsServer += ServerShot;
		}
		MagDiffMax = FMath::Max(MagDiffMax, FMath::Abs(MagHave - (int32)P.magazine));
	}
	LastSyncWeapon = P.weapon;
	LastSyncMag = P.magazine;
	SetStack(MagTag, P.magazine);
	SetStack(SpareTag, P.spare);
}

void UMOBridgeSubsystem::SyncRemoteWeapon(FMOBRemote& R, ACharacter* Ch)
{
	if (R.ShownWeapon == R.Weapon || R.Weapon >= (uint8)mo::EWeapon::Count) return;
	ULyraEquipmentManagerComponent* EM = Ch->FindComponentByClass<ULyraEquipmentManagerComponent>();
	if (!EM) return;
	LoadWeaponClasses();
	UClass* Def = WeaponEquipDefs.IsValidIndex(R.Weapon) ? WeaponEquipDefs[R.Weapon].Get() : nullptr;
	if (!Def) return;
	if (ULyraEquipmentInstance* Old = R.Equipped.Get()) EM->UnequipItem(Old);
	R.Equipped = EM->EquipItem(Def);
	R.ShownWeapon = R.Weapon;
	++RemoteEquips;
}

void UMOBridgeSubsystem::HandleShot(const wire::Shot& M)
{
	++ShotsRecv;
	if (M.shooter == PlayerId && M.weapon < 3) ++MyShots[M.weapon];
	const int32 Count = FMath::Min<int32>(M.count, wire::kMaxBulletsInShot);
	if (Count <= 0) return;

	if (M.shooter == PlayerId)
	{
		// 맞았는지는 서버 답으로 표시한다 — 내 총알의 불꽃·궤적은 Lyra 자기 능력이 그린다
		APlayerController* PC = GetWorld()->GetFirstPlayerController();
		ULyraWeaponStateComponent* WS = PC ? PC->FindComponentByClass<ULyraWeaponStateComponent>() : nullptr;
		if (!WS) return;
		TArray<FHitResult> Hits;
		for (int32 i = 0; i < Count; ++i)
		{
			const wire::ShotBullet& B = M.bullets[i];
			APawn* Target = B.hit == (uint8)wire::EShotHit::Body && B.damage > 0.f ? FindPlayerPawn(B.target) : nullptr;
			if (!Target) continue;
			FHitResult& Hit = Hits.AddDefaulted_GetRef();
			Hit.Location = Hit.ImpactPoint = FVector(B.x, B.y, B.z);
			Hit.HitObjectHandle = FActorInstanceHandle(Target);
		}
		if (Hits.Num() == 0) return;
		FGameplayAbilityTargetDataHandle Handle;
		Handle.UniqueId = ++HitMarkerId;
		WS->AddUnconfirmedServerSideHitMarkers(Handle, Hits);
		WS->ClientConfirmTargetData(Handle.UniqueId, true, TArray<uint8>());
		++HitMarkers;
		RequestEventShot(TEXT("hit"), 0.05, 3);
		return;
	}

	// 남의 총 — Lyra 불꽃·궤적·탄착 효과를 서버가 정한 점으로 부른다
	APawn* Shooter = FindPlayerPawn(M.shooter);
	UAbilitySystemComponent* ASC = FindASC(Shooter);
	if (!ASC) return;
	static const TCHAR* const Names[(int32)mo::EWeapon::Count] = { TEXT("Pistol"), TEXT("Rifle"), TEXT("Shotgun") };
	if (M.weapon >= (uint8)mo::EWeapon::Count) return;
	const FGameplayTag FireTag = FGameplayTag::RequestGameplayTag(*FString::Printf(TEXT("GameplayCue.Weapon.%s.Fire"), Names[M.weapon]), false);
	if (!FireTag.IsValid()) return;
	const FVector Start(M.startX, M.startY, M.startZ);
	for (int32 i = 0; i < Count; ++i)
	{
		const FVector End(M.bullets[i].x, M.bullets[i].y, M.bullets[i].z);
		FGameplayCueParameters Cue;
		Cue.Location = End;
		Cue.Normal = (Start - End).GetSafeNormal();
		Cue.Instigator = Shooter;
		Cue.EffectCauser = Shooter;
		Cue.EffectContext = ASC->MakeEffectContext();
		ASC->ExecuteGameplayCue(FireTag, Cue);
		++ShotCues;
	}
}

UAbilitySystemComponent* UMOBridgeSubsystem::FindASC(AActor* Actor) const
{
	return Actor ? UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Actor) : nullptr;
}
