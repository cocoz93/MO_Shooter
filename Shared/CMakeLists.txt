# 공용 계산 — 서버와 Lyra 플러그인이 같은 코드를 쓴다
# 언리얼이 예외·RTTI 를 끄고 빌드하므로 여기서도 끄고 빌드해 어기면 바로 걸리게 한다
add_library(MoShared STATIC MapCollision.cpp MapCollision.h Movement.cpp Movement.h Weapon.cpp Weapon.h BodyPose.inl GameProtocol.h MatchRules.cpp MatchRules.h KillRecord.h Prediction.cpp Prediction.h AnimClips.cpp AnimClips.h AnimPose.cpp AnimPose.h AnimState.cpp AnimState.h Pickups.cpp Pickups.h Loadout.cpp Loadout.h Dash.cpp Dash.h Projectile.cpp Projectile.h Grenade.cpp Grenade.h)
target_include_directories(MoShared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(MoShared PRIVATE /W4 /GR- /EHs-c-)
    target_compile_definitions(MoShared PRIVATE _HAS_EXCEPTIONS=0 NOMINMAX)
endif()
