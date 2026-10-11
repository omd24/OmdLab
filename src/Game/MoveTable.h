#pragma once

#include "CombatDsl.h"
#include "Engine/BlendSpace.h"
#include "Engine/Collision.h"

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>

namespace Asset
{
    struct Model;
    struct Clip;
}

namespace Game
{
    enum class GuardHeight : uint8_t
    {
        Mid,
        High,
        Low,
        Overhead,
        Throw,
        Unblockable,
    };

    // Where a move can be performed from. A Ground move is selected from a grounded state and an
    // Air move only while the fighter is mid-jump - the same button resolves to a different move
    // in each, so an aerial variant can reuse a ground move's clip and hitbox without ever being
    // reachable from the floor (or the reverse).
    enum class MoveStance : uint8_t
    {
        Ground,
        Air,
    };

    // Absolute (from move start) frame window a hitbox is active for - defaults to
    // [startupFrames, startupFrames + activeFrames) for the common single-hitbox move, but
    // authored per-hitbox so a future multi-hit move can override it.
    struct MoveHitboxDef
    {
        Engine::CollisionBox box;
        uint32_t frameStart = 0;
        uint32_t frameEnd = 0;
        // Optional bone attachment (authored as a "bone <name>" field inside hitbox { } - see
        // MoveTable.cpp's BuildHitboxShape). Empty means today's behavior unchanged: box.offset
        // is relative to the entity's own root Transform. Non-empty names an Asset::Node by its
        // real name (e.g. "RightHand_014") - resolved once at startup (ResolveHitboxJoints)
        // into resolvedJointIndex rather than searched by string every tick; FighterState.cpp
        // then reads that joint's own current animated position each tick box.offset becomes a
        // small refinement relative to, instead of the sole source of the box's position.
        std::string boneName;
        int32_t resolvedJointIndex = -1;
    };

    // One move's full data, built from a parsed CombatDsl::MoveDecl (see BuildMoveTable) -
    // everything a real fighting game move needs to be simulated: which clip plays, its three
    // sequential frame phases, its outcome on hit/block, and its cancel-into list (still holding
    // the parsed condition trees directly, evaluated the same way state transitions are).
    struct MoveDefinition
    {
        std::string id;
        std::string displayName;
        std::string animationClip; // References an Asset::Clip by name.
        std::string inputButton;   // Matches a Game::FighterButton's name (see InputBindings.h).
        MoveStance stance = MoveStance::Ground; // Authored as "stance ground|air".
        uint32_t startupFrames = 0;
        uint32_t activeFrames = 0;
        uint32_t recoveryFrames = 0;
        uint32_t onHitStunFrames = 0;
        uint32_t onBlockStunFrames = 0;
        int32_t damage = 0;
        GuardHeight guardHeight = GuardHeight::Mid;
        std::vector<MoveHitboxDef> hitboxes;
        std::vector<CombatDsl::Cancel> cancels;

        // --- Game-feel: ONE authored knob, everything else derived -------------------------
        // A raw retargeted clip plays at one even speed for its whole length - sluggish and
        // weightless next to any real fighting game. `impact` turns on a reshaped playback
        // (eased wind-up -> snap to the strike pose -> brief frozen hold -> fast recovery) plus
        // a contact freeze-frame and a forward step, ALL scaled by this one number.
        //   authored in .combat as: impact none|light|medium|heavy   (or a raw 0..1)
        //   -1 = not authored -> the clip plays flat, exactly as before any of this existed.
        // This is the only game-feel field a move normally sets.
        float impact = -1.0f;

        // Everything below is DERIVED after load - from `impact` (Game::FinalizeMoveTiming) or
        // auto-detected from the clip itself (Game::AnalyzeMoveTiming). Each stays at its
        // sentinel unless a .combat file pins it explicitly, which is an escape hatch, not part
        // of the normal authoring surface.
        static constexpr uint32_t kUnsetFrames = 0xFFFFFFFFu;
        float timingContactFraction = -1.0f;  // clip fraction of the strike pose (auto-detected from the hitbox bone)
        float timingWindupFraction = -1.0f;   // clip fraction of the coiled pose (auto-detected)
        uint32_t timingSnapFrames = kUnsetFrames;   // coil -> strike, in runtime ticks (default 3)
        uint32_t hitStopFrames = kUnsetFrames;      // both fighters freeze on contact, runtime ticks
        float lungeDistance = -1.0f;                // forward world-units, decelerating to a plant at the strike

        // --- Clip trim: the five-point shape WarpedClipSeconds (FighterState.cpp) actually
        // plays is start -> windup -> contact -> [settle, timingOut ticks long] -> end, not just
        // windup -> contact. These three extend it past the two above, and all default to
        // exactly today's shape (start 0, settle held flat at contact until the state hands off,
        // end - irrelevant while settle never moves) - every move omitting all three is
        // numerically identical to before this existed, same "escape hatch, not the normal
        // surface" status as timingContact/timingWindup. Authored/tuned via the Animation
        // Editor's "Clip timing" panel (main.cpp), which previews live through the SAME
        // ApplyStateEffects path real gameplay uses (via PreviewMoveHitbox), not a separate
        // approximation.
        float timingStartFraction = -1.0f;   // clip fraction the windup segment glides FROM (was hardcoded 0)
        // Runtime ticks (kUnsetFrames = unset -> 0) the settle segment gets, past contact, before
        // the pose hands off to the recovery-blend-out crossfade (FighterState.cpp's own comment
        // on ApplyStateEffects' `motionFrames`) - 0 means "hand off immediately at contact", the
        // pre-existing hardcoded behavior (the settle segment below was, until this field
        // existed, dead code: every real move handed off on its very first settle tick, so the
        // clip's own post-contact frames - a big follow-through's whole retract, say - were
        // never actually sampled in real play, whatever the clip's own length). Rescaled by
        // 1/kCombatSpeedMultiplier at load like timingSnapFrames/hitStopFrames - the same
        // "game-feel tick count" convention, not a hitbox active-window offset.
        uint32_t timingOutFrames = kUnsetFrames;
        float timingEndFraction = -1.0f;     // clip fraction the settle segment glides TOWARD over timingOutFrames (was hardcoded 1.0)

        // Set by FinalizeMoveTiming once the reshaped playback is on and every field above is
        // resolved. Gates reshaped vs. flat playback in FighterState.cpp.
        bool timingCurveActive = false;
        bool HasTimingCurve() const { return timingCurveActive; }
    };

    // One character's full moveset. Move-only (CombatDsl::Cancel holds a unique_ptr<Condition>).
    struct MoveTable
    {
        std::vector<MoveDefinition> moves;

        const MoveDefinition* FindById(const std::string& id) const;

        // Stable position-in-table index, usable directly as Engine::Hitbox::moveId (which is
        // deliberately an opaque uint32_t as far as Engine is concerned) - -1 if id isn't found.
        int32_t IndexOf(const std::string& id) const;
    };

    struct CharacterStats
    {
        int32_t maxHealth = 100;
        float walkSpeed = 2.0f;
        float runSpeed = 4.0f;
        float jumpSpeed = 7.5f; // Initial vertical launch speed for Jump - paired with FighterState.cpp's
                                 // kGravity, which a jump's total airtime (2*jumpSpeed/kGravity) is derived from.
                                 // ~0.69s airtime / ~1.30 peak with today's kGravity; the jump clip is
                                 // phase-mapped onto that arc so airtime need not match any clip's length.
    };

    // A fighter entity is instantiated FROM this, never hand-assembled per-character in code.
    // Populated by LoadCharacterDefinition from a character's own <name>/character.combat file
    // (a CombatDsl "character" block); only the AI-preset "punish" move is still stitched in by
    // hand at the main.cpp call site.
    struct CharacterDefinition
    {
        std::string modelDirectory; // Set to character.combat's own containing directory by LoadCharacterDefinition.
        std::string modelFile;      // Skinned-mesh filename within modelDirectory (character block "model" field).
        std::string displayName;    // Human-readable name (character block "name" field) - UI/debug only.
        CharacterStats stats;
        MoveTable moveTable;
        // Corrective transform this character's specific source asset's MESH VERTEX data needs
        // applied to land in true game-world units/position - see Engine::
        // CreateSkinnedMeshDrawItems's own rootTransform parameter for the scale quirk this
        // exists to correct (a Sketchfab FBX->glTF export baking a 0.01 unit-conversion scale
        // this asset's skin data doesn't otherwise account for). Lives here (not just a
        // main.cpp-local, which is where it used to live) as a single source of truth for
        // whichever future code needs it a second place, even though FighterState.cpp's
        // bone-attached hitbox lookup turned out NOT to need it - verified empirically that this
        // asset's joint/skeleton hierarchy is already true-scale on its own, unlike its mesh
        // vertex data (see ResolveHitboxOffset's own comment for the full story). Identity by
        // default - only a real character asset with a known quirk needs to set this to anything
        // else. Stored as XMFLOAT4X4, not XMMATRIX, matching this project's convention of never
        // storing the SIMD-register type as a class member (XMLoadFloat4x4/XMStoreFloat4x4 at
        // the point of use, same as every other persisted matrix in this codebase).
        DirectX::XMFLOAT4X4 assetCorrection = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
        // A second, DELIBERATELY SEPARATE corrective rotation (radians, around world +Y) - not
        // folded into assetCorrection above, because it means something different: assetCorrection
        // fixes an accidental *import artifact* affecting only mesh vertex data (empirically not
        // the skeleton); this fixes which way the character actually faces, a real property of
        // the whole rigid character (mesh AND skeleton alike, since they must stay visually
        // consistent with each other) that this project cares about for gameplay reasons - the
        // ground plane/movement/camera all already assume a 2D side view (camera looking down
        // +Z, X is the screen-horizontal gameplay axis), but this character's own source asset
        // was authored facing along Z, showing its front/back to that camera instead of a
        // profile. Applied everywhere assetCorrection's scale is NOT (both mesh render AND
        // bone-attached hitbox lookups - see ResolveHitboxOffset), since unlike the scale quirk
        // this must move the skeleton too, or a hitbox would visibly detach from the mesh it's
        // meant to track. 0 (no correction) by default. Authored in character.combat as
        // "facingDegrees" (degrees, converted on load) since degrees read better by hand.
        float facingCorrectionRadians = 0.0f;

        // Constant world-space Y translate seating this asset's feet on the Y=0 ground plane -
        // the model's origin sits above the soles, so every pose renders lifted by the same gap.
        // Applied wherever facingCorrectionRadians is (render world matrix + bone-attached hitbox
        // lookup), NOT to Transform.position - "position.y == 0 means grounded" stays true as a
        // gameplay contract; this is a render/collision-space fix-up for one asset's authoring.
        // character.combat "correction" block, 0 (no offset) by default.
        float groundingOffsetY = 0.0f;

        // KO settle-to-ground ramp. The retargeted "ko" clip animates the limbs through a fall
        // while its root joint stays at standing hip height (the generic per-axis root-motion
        // strip in the retarget pipeline removed the downward translation the knockdown needed),
        // so the body ends the clip lying in mid-air. While in the KO state, Transform.position.y
        // eases from 0 to koDropOffsetY across clip frames [koDropStartFrame, koDropEndFrame],
        // then holds there for the rest of KO (the clip itself already freezes its final pose).
        // koDropOffsetY == 0 (default) leaves KO untouched. Frame counts are pre-scaled by
        // 1/kCombatSpeedMultiplier on load, same as MoveDefinition's own frame fields, since the
        // "ko" clip plays back at that same sped-up rate. character.combat "ko" block.
        float koDropOffsetY = 0.0f;
        uint32_t koDropStartFrame = 0;
        uint32_t koDropEndFrame = 0;

        // Locomotion blend space (idle/walk_forward/walk_backward/running keyed on signed
        // movement speed), built once at load by Game::BuildLocomotionBlendSpace from this
        // character's clips + walk/run stats. The animation source for the Idle/Walk/Run states.
        // Empty (no samples) if none of the expected clips are present - then those states fall
        // back to the bind pose.
        Engine::BlendSpace1D locomotionBlendSpace;

        // Per-node participation weights [0,1] for the in-place jump's sway overlay, built once
        // at load by Game::BuildJumpFloatMask. The in-place jump holds a single frame of its
        // clip and blends toward a later frame of the same clip by a sine hump over the airtime
        // (Animator::overlayWeight) - one smooth rise-and-settle rather than the clip playing
        // back, which loops the limbs rapidly at this playback rate. This mask selects which
        // nodes take part (uniform 1.0 today; a per-node vector so it can be shaped per
        // body-part later). Indexed like Asset::Model::nodes; empty -> no overlay.
        std::vector<float> jumpFloatMask;
    };

    // Maps a parsed CombatFile's stringly-typed MoveDecls onto real MoveDefinition fields.
    // Unknown field names are logged and skipped, not a hard error - keeps authoring additive
    // (a typo'd or not-yet-consumed field doesn't block everything else from loading). Takes the
    // parsed file by rvalue reference since it consumes (moves out of) each MoveDecl's cancel
    // list rather than copying it - CombatDsl::Condition is move-only.
    bool BuildMoveTable(CombatDsl::CombatFile&& parsedFile, MoveTable& outTable);

    // Maps a parsed CombatDsl::CharacterDecl's stringly-typed fields onto a CharacterDefinition's
    // typed ones: "name" -> displayName, "model" -> modelFile, the "stats" sub-block -> stats,
    // "correction" -> assetCorrection (from "assetScale")/facingCorrectionRadians (from
    // "facingDegrees")/groundingOffsetY, "ko" -> koDropOffsetY/koDropStartFrame/koDropEndFrame
    // (the two frame fields scaled by 1/kCombatSpeedMultiplier here, matching BuildMoveTable's
    // own frame-field scaling). Unknown fields are logged and skipped, same additive-authoring
    // rule as BuildMoveTable. Does NOT touch modelDirectory or moveTable - those are the
    // orchestrating LoadCharacterDefinition's job. The "moves" field is recognized but ignored
    // here (LoadCharacterDefinition consumes it), so it doesn't log as unknown.
    void BuildCharacterDefinition(const CombatDsl::CharacterDecl& decl, CharacterDefinition& outDefinition);

    // The one entry point for loading a character: parses characterComatPath (a file with a
    // "character" block), runs BuildCharacterDefinition, sets modelDirectory to that file's own
    // containing directory, then loads + builds the moveset named by the block's "moves" field
    // (resolved relative to the same directory). All-or-nothing, matching this project's other
    // loaders: logs the reason and returns false on a missing/unparseable file, a missing
    // "character" or "moves" field, or a MoveTable build failure - outDefinition is then left
    // partially written and must not be used. Bone-attachment resolution (ResolveHitboxJoints)
    // still happens separately at the call site, since it needs the imported model.
    bool LoadCharacterDefinition(const std::string& characterCombatPath, CharacterDefinition& outDefinition);

    // One-time resolution of every hitbox's optional bone attachment (MoveHitboxDef::boneName)
    // from a name to a stable Asset::Model node index - called once at startup after both the
    // character's model and move table are loaded, not searched by string every tick. Logs (but
    // doesn't fail loudly on) any boneName that doesn't match a real node name, leaving
    // resolvedJointIndex at -1 - FighterState.cpp treats that identically to "no bone was ever
    // authored," so a typo degrades to the old root-relative behavior rather than crashing.
    void ResolveHitboxJoints(const Asset::Model& model, MoveTable& moveTable);

    // Auto-detects each move's strike/coil clip fractions (MoveDefinition::timingContactFraction
    // / timingWindupFraction) by scanning its animation clip: at ~one sample per frame, evaluate
    // the move's hitbox bone position and take the frame where it is furthest from the body's
    // mean node position (the strike) and, before that, nearest (the coil). Only fills fields
    // still at their sentinel; a move with no bone-attached hitbox, or whose clip can't be
    // found, is left for FinalizeMoveTiming's fallback. Call once at startup, after
    // ResolveHitboxJoints (it needs the resolved joint index) and before FinalizeMoveTiming.
    void AnalyzeMoveTiming(const Asset::Model& model, const std::vector<Asset::Clip>& clips, MoveTable& moveTable);

    // Resolves the game-feel layer: turns each move's `impact` (0..1) into concrete
    // hold/hitstop/lunge values, fills every derived timing field still unset (curve shape
    // constants, and the clip-fraction fallbacks AnalyzeMoveTiming couldn't supply), and sets
    // timingCurveActive. A move with impact <= 0 and no explicit timingContact is left flat.
    // Call once, after AnalyzeMoveTiming, before the move table is used by the sim.
    void FinalizeMoveTiming(MoveTable& moveTable);

    // Targeted text patch for the hitbox-tuning debug tool's "Save" button - rewrites the six
    // numeric offsetX/offsetY/offsetZ/halfX/halfY/halfZ field lines of one hitbox block
    // (identified by moveId + its position among that move's own hitbox{} blocks), leaving every
    // other line - comments, other fields, formatting - byte-for-byte untouched. Deliberately NOT
    // a general CombatDsl serializer: regenerating the whole file from the parsed AST would lose
    // every hand-authored comment doing so (moves.combat's own header is exactly the kind of
    // content this must never destroy - see the project's standing "no gitignoring/no destroying
    // authored content" convention). Uses simple line-based brace-depth scanning, not a real
    // parse - relies on this project's own authored style (each "move"/"hitbox" keyword's body
    // opening "{" can be on the same or the next line; a field's value is whatever follows its
    // name on that same line, comment-stripped). Returns false (logs why, never touches the file)
    // if the move, the Nth hitbox, or any of the six offset/half-extent field lines can't be
    // found - a save must never silently do nothing or partially apply.
    //
    // frameStart/frameEnd are handled differently from the six spatial fields above: since
    // MoveHitboxDef::frameStart/frameEnd default to [startup, startup+active) rather than always
    // being explicitly authored (see BuildHitboxShape), a hitbox block frequently has no existing
    // frameStart/frameEnd LINE to rewrite at all. Rather than failing in that (common, expected)
    // case, this function INSERTS a new line for whichever of the two is missing, right before
    // the hitbox block's own closing "}", indented to match whatever indentation the existing
    // offset/half-extent lines in that same block already use. An existing frameStart/frameEnd
    // line (a move that's already had its timing tuned once before) is rewritten in place, same
    // as the six spatial fields.
    bool SaveHitboxToFile(
        const std::string& filePath, const std::string& moveId, int32_t hitboxIndex, const Engine::CollisionBox& box,
        uint32_t frameStart, uint32_t frameEnd);

    // The dev-tools "Character" panel's "Save" counterpart to SaveHitboxToFile: rewrites the
    // "groundingOffsetY" line (correction block) and the "dropOffsetY"/"dropStartFrame"/
    // "dropEndFrame" lines (ko block) of filePath's one "character" block in place, every other
    // line - comments, stats, formatting - byte-for-byte untouched. No insert path (unlike
    // SaveHitboxToFile's frameStart/frameEnd): character.combat always authors all four, so a
    // missing target line is a hard failure that touches nothing. koDropStartFrameRaw/
    // koDropEndFrameRaw are RAW clip frames - the caller multiplies the speed-scaled runtime
    // values back up by kCombatSpeedMultiplier first, so the file keeps the numbers as authored.
    bool SaveCharacterToFile(
        const std::string& filePath, float groundingOffsetY, float koDropOffsetY, uint32_t koDropStartFrameRaw,
        uint32_t koDropEndFrameRaw);

    // The dev-tools "Clip timing" panel's "Save" counterpart to SaveHitboxToFile - same targeted
    // line-rewrite/insert approach, but for timingStart/timingOut/timingEnd (MoveDefinition's own
    // comment), which live at the MOVE's own top level (same depth as the existing
    // timingWindup/timingContact/timingSnapFrames fields), not inside a hitbox{} sub-block. A
    // missing line for any of the three is expected (most moves have none of them authored,
    // defaulting in code instead) and gets a new line inserted right before the move's own
    // closing "}", never treated as failure - only "move <moveId> not found at all" is.
    // timingOutRaw is RAW authored ticks (the caller multiplies the speed-scaled runtime value
    // back up by kCombatSpeedMultiplier first, same convention SaveCharacterToFile's KO frame
    // params already use).
    bool SaveMoveTimingToFile(
        const std::string& filePath, const std::string& moveId, float timingStart, uint32_t timingOutRaw, float timingEnd);
}
