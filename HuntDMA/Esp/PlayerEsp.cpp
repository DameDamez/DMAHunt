#include "PlayerEsp.h"
#include "CheatFunction.h"
#include "ConfigInstance.h"
#include "ConfigUtilities.h"
#include "ESPRenderer.h"
#include "Localization/Localization.h"
#include "Pch.h"
#include "WorldEntity.h"
#include "globals.h"
#include <mutex>

std::shared_ptr<CheatFunction> UpdatePlayers =
    std::make_shared<CheatFunction>(1, [] {
      EnvironmentInstance->UpdatePlayerList();
      // Update velocity for all players (DMA-rate limited by time guard in
      // UpdateVelocity itself)
      for (auto &player : EnvironmentInstance->GetPlayerList()) {
        if (player)
          player->UpdateVelocity();
      }
    });
std::shared_ptr<CheatFunction> UpdateBosses = std::make_shared<CheatFunction>(
    5, [] { EnvironmentInstance->UpdateBossesList(); });
std::shared_ptr<CheatFunction> UpdateGrunts = std::make_shared<CheatFunction>(
    5, [] { EnvironmentInstance->UpdateGruntList(); });

// Helper function to get health bar color based on HP percentage
static ImVec4 GetHealthBarColor(unsigned int currentHp, unsigned int maxHp) {
  if (maxHp == 0)
    return ImVec4(0.5f, 0.5f, 0.5f, 1.0f); // Gray for invalid

  float percentage = (float)currentHp / (float)maxHp;

  if (percentage > 0.75f) // 100%-75% = Green
    return ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
  else if (percentage > 0.50f) // 75%-50% = Yellow
    return ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
  else if (percentage > 0.25f) // 50%-25% = Orange
    return ImVec4(1.0f, 0.647f, 0.0f, 1.0f);
  else // 25%-0% = Red
    return ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
}

void DrawBossesEsp() {
  if (EnvironmentInstance == nullptr)
    return;

  if (EnvironmentInstance->GetObjectCount() < 10)
    return;

  // Brief lock to copy render list, then release
  std::vector<std::shared_ptr<WorldEntity>> templist;
  {
    std::lock_guard<std::mutex> lock(EntityMutex);
    templist = EnvironmentInstance->GetRenderBossesList();
  }

  if (Configs.Bosses.Enable) {
    if (!templist.empty()) {
      for (std::shared_ptr<WorldEntity> ent : templist) {
        if (ent == nullptr)
          continue;
        int distance = (int)Vector3::Distance(ent->Render.Position,
                                              CameraInstance->GetRenderPosition());
        if (distance <= 0 || distance > Configs.Bosses.MaxDistance)
          continue;

        if (!ent->Render.Valid)
          continue;
        Vector2 pos = CameraInstance->RenderWorldToScreen(ent->Render.Position);
        if (pos.x <= 0 || pos.y <= 0)
          continue;
        std::string wname =
            Configs.Bosses.Name ? LOC("entity", ent->GetTypeAsString()) : "";
        std::string wdistance =
            Configs.Bosses.Distance
                ? std::vformat(LOC("menu", "esp.Meters"),
                               std::make_format_args(distance))
                : "";
        ESPRenderer::DrawText(ImVec2(pos.x, pos.y), wname + wdistance,
                              Configs.Bosses.TextColor, Configs.Bosses.FontSize,
                              Center);
      }
    }
  }
}

void DrawPlayersEsp() {
  if (EnvironmentInstance == nullptr)
    return;

  if (EnvironmentInstance->GetObjectCount() < 10)
    return;

  // Brief lock to copy render list, then release
  std::vector<std::shared_ptr<WorldEntity>> templist;
  {
    std::lock_guard<std::mutex> lock(EntityMutex);
    templist = EnvironmentInstance->GetRenderPlayerList();
  }

  if (Configs.Player.Enable || Configs.Player.BoxType > 0 ||
      Configs.Player.Snaplines) {
    if (templist.empty())
      return;

    Vector2 center;
    if (Configs.General.CrosshairLowerPosition)
      center = Vector2(ESPRenderer::GetScreenWidth() * 0.5f,
                       ESPRenderer::GetScreenHeight() * 0.6f);
    else
      center = Vector2(ESPRenderer::GetScreenWidth() * 0.5f,
                       ESPRenderer::GetScreenHeight() * 0.5f);

    // 1. Collect positions of confirmed DEAD entities (Hunter_Loot only)
    std::vector<Vector3> deadPositions;
    for (std::shared_ptr<WorldEntity> ent : templist) {
      // Strictly check for Loot Box class name to avoid self-referencing bug
      if (ent != nullptr &&
          strstr(ent->GetEntityClassName().name, "Hunter_Loot")) {
        deadPositions.push_back(ent->Render.Position);
        ent->SetType(
            EntityType::DeadPlayer); // Ensure loot boxes are always Dead
      }
    }

    for (std::shared_ptr<WorldEntity> ent : templist) {
      if (ent == nullptr || ent->Render.Type == EntityType::LocalPlayer)
        continue;

      // Skip Hunter_Loot entities (they are used for detection only, not to be
      // drawn)
      if (strstr(ent->GetEntityClassName().name, "Hunter_Loot"))
        continue;

      auto playerPos = ent->Render.Position;

      if (!Configs.Player.DrawFriendsHP && !Configs.Player.DrawBonesFriend &&
          ent->Render.Type == EntityType::FriendlyPlayer)
        continue;

      if (!ent->Render.Valid || ent->Render.Hidden) // Has extracted
        continue;

      // Check if it is a Player Model
      if (strstr(ent->GetEntityClassName().name, "HunterBasic")) {
        // 1. Determine team (Friend/Enemy) based on silhouettes_param
        if (ent->Render.Node.silhouettes_param == 0x8CD2FF ||
            ent->Render.Node.silhouettes_param == 0x3322eeff) {
          ent->SetType(EntityType::FriendlyPlayer);
        } else {
          ent->SetType(EntityType::EnemyPlayer);
        }

        // 2. Check if dead based on HP (primary method)
        auto health = ent->Render.Health;
        if (health.current_hp == 0 && IsValidHP(health.current_max_hp)) {
          ent->SetType(EntityType::DeadPlayer);
        }
        // 3. Fallback: proximity to loot box (for cases where HP read fails)
        else {
          for (const auto &dPos : deadPositions) {
            if (Vector3::Distance(playerPos, dPos) < 1.0f) // 1 meter tolerance
            {
              ent->SetType(EntityType::DeadPlayer);
              break;
            }
          }
        }
      }

      bool isDead = (ent->Render.Type == EntityType::DeadPlayer);

      // HP-based dead detection now active

      // 3. Set Teams logic (Redundant but keeps existing structure safe)
      // (Already handled in reset step above)

      if (!Configs.Player.ShowDead && isDead)
        continue;

      int distance =
          (int)Vector3::Distance(playerPos, CameraInstance->GetRenderPosition());
      if (distance <= 0 || distance > (isDead ? Configs.Player.DeadMaxDistance
                                              : Configs.Player.MaxDistance))
        continue;

      auto tempPos = playerPos;
      Vector2 feetPos = CameraInstance->RenderWorldToScreen(playerPos, false);
      if (feetPos.IsZero())
        continue;

      tempPos.z = playerPos.z + 1.7f;
      Vector2 headPos;
      if (Configs.Player.Snaplines) {
        headPos = CameraInstance->RenderWorldToScreen(tempPos, false);
        if (headPos.IsZero())
          continue;
      }

      tempPos.z = playerPos.z + 2.0f;
      Vector2 uppderFramePos;
      if ((Configs.Player.BoxType > 0 || Configs.Player.DrawHealthBars)) {
        uppderFramePos = CameraInstance->RenderWorldToScreen(tempPos, false);
        if (uppderFramePos.IsZero())
          continue;
      }

      if (Configs.Player.Snaplines && !headPos.IsZero()) {
        auto colour = ent->Render.Type == EntityType::FriendlyPlayer
                          ? Configs.Player.FriendColor
                          : Configs.Player.FramesColor;
        ESPRenderer::DrawLine(ImVec2(center.x, center.y),
                              ImVec2(headPos.x, headPos.y), colour, 1.0f);
      }

      tempPos.z = playerPos.z + 2.1f;
      Vector2 healthBarPos;
      if (Configs.Player.DrawHealthBars) {
        healthBarPos = CameraInstance->RenderWorldToScreen(tempPos, false);
        if (healthBarPos.IsZero())
          continue;
      }

      Vector2 offset, normal;
      if ((Configs.Player.BoxType > 0 || Configs.Player.DrawHealthBars)) {
        Vector2 v = uppderFramePos - feetPos;
        float segmentLength = Vector2::Length(v);
        normal = Vector2(-v.y, v.x);
        offset = normal / (2.0f * 2);
      }

      if (Configs.Player.BoxType > 0 &&
          ent->Render.Type != EntityType::FriendlyPlayer && !isDead) {
        // Determine top position based on state (Alive = 2.0m, Dead/Loot
        // = 1.85m)
        Vector2 topPos = uppderFramePos;
        if (isDead) {
          auto tempPosTop = playerPos;
          tempPosTop.z = playerPos.z + 1.85f;
          topPos = CameraInstance->RenderWorldToScreen(tempPosTop, false);
        }

        if (!topPos.IsZero()) {
          Vector2 v = topPos - feetPos;
          float segmentLength = Vector2::Length(v);
          Vector2 normal = Vector2(-v.y, v.x);
          Vector2 offset = normal / (2.0f * 2);

          Vector2 A1 = feetPos + offset; // Bottom-left
          Vector2 A2 = feetPos - offset; // Bottom-right
          Vector2 B1 = topPos + offset;  // Top-left
          Vector2 B2 = topPos - offset;  // Top-right

          auto colour = ent->Render.Type == EntityType::FriendlyPlayer
                            ? Configs.Player.FriendColor
                            : Configs.Player.FramesColor;

          if (Configs.Player.BoxType == 2) {
            // Full box — draw all 4 sides
            ESPRenderer::DrawLine(ImVec2(A1.x, A1.y), ImVec2(A2.x, A2.y), colour, 1.5f);
            ESPRenderer::DrawLine(ImVec2(A2.x, A2.y), ImVec2(B2.x, B2.y), colour, 1.5f);
            ESPRenderer::DrawLine(ImVec2(B2.x, B2.y), ImVec2(B1.x, B1.y), colour, 1.5f);
            ESPRenderer::DrawLine(ImVec2(B1.x, B1.y), ImVec2(A1.x, A1.y), colour, 1.5f);
          }
          else {
            // Corner brackets (BoxType == 1)

            // Bottom-left corner (L-shape)
            Vector2 BL_horizontal =
                Vector2(A1.x + (A2.x - A1.x) * 0.2f, A1.y + (A2.y - A1.y) * 0.2f);
            Vector2 BL_vertical =
                Vector2(A1.x + (B1.x - A1.x) * 0.2f, A1.y + (B1.y - A1.y) * 0.2f);
            ESPRenderer::DrawLine(ImVec2(A1.x, A1.y),
                                  ImVec2(BL_horizontal.x, BL_horizontal.y),
                                  colour, 1.5f);
            ESPRenderer::DrawLine(ImVec2(A1.x, A1.y),
                                  ImVec2(BL_vertical.x, BL_vertical.y), colour,
                                  1.5f);

            // Bottom-right corner
            Vector2 BR_horizontal =
                Vector2(A2.x + (A1.x - A2.x) * 0.2f, A2.y + (A1.y - A2.y) * 0.2f);
            Vector2 BR_vertical =
                Vector2(A2.x + (B2.x - A2.x) * 0.2f, A2.y + (B2.y - A2.y) * 0.2f);
            ESPRenderer::DrawLine(ImVec2(A2.x, A2.y),
                                  ImVec2(BR_horizontal.x, BR_horizontal.y),
                                  colour, 1.5f);
            ESPRenderer::DrawLine(ImVec2(A2.x, A2.y),
                                  ImVec2(BR_vertical.x, BR_vertical.y), colour,
                                  1.5f);

            // Top-left corner
            Vector2 TL_horizontal =
                Vector2(B1.x + (B2.x - B1.x) * 0.2f, B1.y + (B2.y - B1.y) * 0.2f);
            Vector2 TL_vertical =
                Vector2(B1.x + (A1.x - B1.x) * 0.2f, B1.y + (A1.y - B1.y) * 0.2f);
            ESPRenderer::DrawLine(ImVec2(B1.x, B1.y),
                                  ImVec2(TL_horizontal.x, TL_horizontal.y),
                                  colour, 1.5f);
            ESPRenderer::DrawLine(ImVec2(B1.x, B1.y),
                                  ImVec2(TL_vertical.x, TL_vertical.y), colour,
                                  1.5f);

            // Top-right corner
            Vector2 TR_horizontal =
                Vector2(B2.x + (B1.x - B2.x) * 0.2f, B2.y + (B1.y - B2.y) * 0.2f);
            Vector2 TR_vertical =
                Vector2(B2.x + (A2.x - B2.x) * 0.2f, B2.y + (A2.y - B2.y) * 0.2f);
            ESPRenderer::DrawLine(ImVec2(B2.x, B2.y),
                                  ImVec2(TR_horizontal.x, TR_horizontal.y),
                                  colour, 1.5f);
            ESPRenderer::DrawLine(ImVec2(B2.x, B2.y),
                                  ImVec2(TR_vertical.x, TR_vertical.y), colour,
                                  1.5f);
          }
        }
      }

      // ── Draw Head Circle (independent, uses bone head position) ──
      if (Configs.Player.DrawHead &&
          ent->Render.Type != EntityType::FriendlyPlayer && !isDead) {
        const Vector3& boneHeadWorld = ent->Render.BonePositions[0]; // bone 0 = head
        if (!boneHeadWorld.IsZero()) {
          Vector2 boneHeadScreen = CameraInstance->RenderWorldToScreen(boneHeadWorld, false);
          if (!boneHeadScreen.IsZero()) {
            // Estimate radius from frame width if box is drawn, else use distance-based
            float headRadius = 5.0f; // fallback
            if (Configs.Player.BoxType > 0 && !uppderFramePos.IsZero()) {
              Vector2 v = uppderFramePos - feetPos;
              Vector2 n = Vector2(-v.y, v.x);
              Vector2 off = n / (2.0f * 2);
              headRadius = Vector2::Distance(boneHeadScreen + off, boneHeadScreen - off) /
                           Configs.Player.HeadCircleSize;
            } else {
              // Distance-based radius when no box
              float dist = Vector3::Distance(playerPos, CameraInstance->GetRenderPosition());
              headRadius = std::max(3.0f, 500.0f / dist / Configs.Player.HeadCircleSize);
            }
            auto colour = Configs.Player.FramesColor;
            ESPRenderer::DrawCircle(
                ImVec2(boneHeadScreen.x, boneHeadScreen.y), headRadius,
                colour, 1.5f, false);
          }
        }
      }

      if (Configs.Player.DrawHealthBars) {
        auto health = ent->Render.Health;
        Vector2 Health1 = healthBarPos - offset;
        Vector2 Health2 = healthBarPos + offset;
        auto lineHeight =
            std::max(2.0f, Vector2::Distance(Health1, Health2) / 10.0f);
        float currentHp = health.current_hp / 150.0f;
        float currentMaxHp = health.current_max_hp / 150.0f;
        float potentialMaxHp = health.regenerable_max_hp / 150.0f;
        Vector2 currentHpPos =
            Vector2(currentHp * Health2.x + (1 - currentHp) * Health1.x,
                    currentHp * Health2.y + (1 - currentHp) * Health1.y);
        Vector2 currentMaxHpPos =
            Vector2(currentMaxHp * Health2.x + (1 - currentMaxHp) * Health1.x,
                    currentMaxHp * Health2.y + (1 - currentMaxHp) * Health1.y);
        Vector2 potentialMaxHpPos = Vector2(
            potentialMaxHp * Health2.x + (1 - potentialMaxHp) * Health1.x,
            potentialMaxHp * Health2.y + (1 - potentialMaxHp) * Health1.y);

        // Determine health bar color based on HP percentage
        ImVec4 healthColor;
        if (ent->Render.Type == EntityType::FriendlyPlayer) {
          // Friendly players: use blue or dynamic color based on config
          healthColor = ImVec4(0.058823f, 0.407843f, 0.909803f, 1.0f); // Blue
        } else {
          // Enemy players: use dynamic color based on HP percentage
          healthColor =
              GetHealthBarColor(health.current_hp, health.regenerable_max_hp);
        }

        ESPRenderer::DrawLine( // current health
            ImVec2(Health1.x, Health1.y),
            ImVec2(currentHpPos.x, currentHpPos.y), healthColor, lineHeight);
        ESPRenderer::DrawLine( // regenerable black health
            ImVec2(currentHpPos.x, currentHpPos.y),
            ImVec2(currentMaxHpPos.x, currentMaxHpPos.y),
            ImVec4(0.039215f, 0.039215f, 0.039215f, 1.0f), lineHeight);
        ESPRenderer::DrawLine( // burning health
            ImVec2(currentMaxHpPos.x, currentMaxHpPos.y),
            ImVec2(potentialMaxHpPos.x, potentialMaxHpPos.y),
            ImVec4(0.784313f, 0.392156f, 0.039215f, 1.0f), lineHeight);
        ESPRenderer::DrawLine( // lost health
            ImVec2(potentialMaxHpPos.x, potentialMaxHpPos.y),
            ImVec2(Health2.x, Health2.y),
            ImVec4(0.784313f, 0.784313f, 0.784313f, 1.0f), lineHeight);
      }

      // ── Skeleton ESP (cached bone projections) ────────────────────────────
      bool isFriend = (ent->Render.Type == EntityType::FriendlyPlayer);
      bool drawBonesNow = (!isFriend && Configs.Player.DrawBones && !isDead)
                       || (isFriend  && Configs.Player.DrawBonesFriend);
      if (drawBonesNow)
      {
        // Cache all 15 bone screen positions in one pass
        constexpr int BONE_COUNT = 15;
        Vector2 screenBones[BONE_COUNT];
        bool    boneValid[BONE_COUNT] = {};

        for (int i = 0; i < BONE_COUNT; i++)
        {
          Vector3 worldPos = ent->Render.BonePositions[i];
          if (!worldPos.IsZero()) {
            screenBones[i] = CameraInstance->RenderWorldToScreen(worldPos, false);
            boneValid[i] = !screenBones[i].IsZero();
          }
        }

        // Draw connections from cached screen positions
        static const int connections[][2] = {
          {0, 1}, {1, 2},           // head-neck-pelvis (spine)
          {1, 3}, {3, 4}, {4, 5},   // R arm
          {1, 9}, {9,10},{10,11},   // L arm
          {2, 6}, {6, 7}, {7, 8},   // R leg
          {2,12},{12,13},{13,14},   // L leg
        };
        auto boneColour = isFriend ? Configs.Player.FriendBonesColor : Configs.Player.BonesColor;
        for (auto& c : connections)
        {
          if (!boneValid[c[0]] || !boneValid[c[1]]) continue;
          ESPRenderer::DrawLine(
            ImVec2(screenBones[c[0]].x, screenBones[c[0]].y),
            ImVec2(screenBones[c[1]].x, screenBones[c[1]].y),
            boneColour, 1.0f);
        }
      }

      if (!Configs.Player.Enable ||
          ent->Render.Type == EntityType::FriendlyPlayer)
        continue;

      std::string wname = (Configs.Player.Name || isDead)
                              ? LOC("entity", ent->GetTypeAsString())
                              : "";
      std::string wdistance =
          Configs.Player.Distance
              ? std::vformat(LOC("menu", "esp.Meters"),
                             std::make_format_args(distance))
              : "";
      std::string whealth =
          Configs.Player.HP
              ? std::to_string(ent->Render.Health.current_hp) + "/" +
                    std::to_string(ent->Render.Health.current_max_hp) + "[" +
                    std::to_string(ent->Render.Health.regenerable_max_hp) + "]"
              : "";
      std::string wweapons;
      if (Configs.Player.ShowWeapons) {
          if (!ent->Render.WeaponName1.empty())
              wweapons = ent->Render.WeaponName1;
          if (!ent->Render.WeaponName2.empty())
              wweapons += " | " + ent->Render.WeaponName2;
      }
      std::string wteam;
      if (Configs.Player.ShowTeam) {
          int tid = ent->Render.TeamId;
          if (tid > 0)
              wteam = "[T" + std::to_string(tid) + "]";
          else if (tid == 0)
              wteam = "[Solo]";
          // tid == -1 means unassigned (game not fully loaded) → show nothing
      }
      std::string espText = wname + wdistance;
      if (!whealth.empty())
          espText += "\n" + whealth;
      if (!wweapons.empty())
          espText += "\n" + wweapons;
      if (!wteam.empty())
          espText += "\n" + wteam;
      ESPRenderer::DrawText(ImVec2(feetPos.x, feetPos.y),
                            espText,
                            ent->Render.Type == EntityType::FriendlyPlayer
                                ? Configs.Player.FriendColor
                                : Configs.Player.TextColor,
                            Configs.Player.FontSize, TopCenter);

    }
  }
}

bool IsValidHP(int hp) { return hp > 0 && hp <= 150; }