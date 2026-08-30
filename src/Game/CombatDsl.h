#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Game::CombatDsl
{
    // A small hand-rolled text format for authoring a fighter's state-transition graph and a
    // move's cancel graph, both gated by boolean conditions over named flags/variables whose
    // meaning this file has no knowledge of (a genuinely generic DSL doesn't know what
    // "jumpPressed" means, only that it's a named flag - Game supplies meaning via
    // EvaluationContext below). Chosen over a flat CSV/table (can't express conditional logic
    // like "require jumpPressed && isGrounded") and over embedding a scripting language like Lua
    // (the actual need - boolean composition over named flags, nothing stateful or looping - is
    // small enough to hand-roll a lexer/parser/evaluator for, matching this project's existing
    // from-scratch parsers).
    //
    // Grammar (EBNF-ish):
    //   file            := declaration*
    //   declaration     := stateDecl | moveDecl | characterDecl
    //   stateDecl       := "state" IDENT "{" transitionDecl* "}"
    //   transitionDecl  := "transition" IDENT "{" conditionItem* "}"
    //   moveDecl        := "move" IDENT "{" moveBodyItem* "}"
    //   moveBodyItem    := fieldAssignment | cancelDecl | hitboxDecl | sfxDecl | vfxDecl
    //   cancelDecl      := "cancel" IDENT "{" conditionItem* "}"
    //   hitboxDecl      := "hitbox" "{" fieldAssignment* "}"
    //   sfxDecl         := "sfx" "{" fieldAssignment* "}"
    //   vfxDecl         := "vfx" "{" fieldAssignment* "}"
    //   characterDecl   := "character" "{" characterBodyItem* "}"
    //   characterBodyItem := fieldAssignment | statsDecl | correctionDecl | koDecl
    //   statsDecl       := "stats" "{" fieldAssignment* "}"
    //   correctionDecl  := "correction" "{" fieldAssignment* "}"
    //   koDecl          := "ko" "{" fieldAssignment* "}"
    //   fieldAssignment := IDENT (NUMBER | IDENT | STRING)
    //   conditionItem   := ["require"] conditionTerm
    //   conditionTerm   := conditionExpr | allBlock | anyBlock | notTerm
    //   allBlock        := "all" "{" conditionItem* "}"
    //   anyBlock        := "any" "{" conditionItem* "}"
    //   notTerm         := "not" conditionTerm
    //   conditionExpr   := IDENT [ comparisonOp (NUMBER | IDENT) ]
    //   comparisonOp    := ">=" | "<=" | "==" | "!=" | ">" | "<"
    //
    // Lexical: IDENT = [A-Za-z_][A-Za-z0-9_]*, NUMBER an int/decimal literal (optional leading
    // '-'), STRING double-quoted text, punctuation '{'/'}'. '#' starts a line comment.
    //
    // "require" is pure sugar, not a distinct AST node - "require X" and a bare "X" parse
    // identically. A transitionDecl/cancelDecl/allBlock body is implicitly AND'ed; an anyBlock
    // body is implicitly OR'ed. "all"/"any"/"not" are therefore reserved at the start of a
    // conditionTerm - a flag/variable literally named one of those three would be unreachable,
    // an accepted limitation for a grammar this small (same spirit as "require" itself).
    // Likewise "stats"/"correction"/"ko" are reserved at the start of a characterBodyItem - a
    // top-level character field literally named one of those would be read as a sub-block open.
    // A file carries at most one characterDecl; a second one is a parse error. characterDecl is
    // a valid declaration in any .combat file, but only a character's own file authors one -
    // states.combat / moves.combat simply never contain one.

    enum class ComparisonOp
    {
        None,
        GreaterEqual,
        LessEqual,
        Equal,
        NotEqual,
        Greater,
        Less,
    };

    // The right-hand side of a comparison - either a literal number or another named variable
    // (resolved through the same EvaluationContext::getNumber at evaluation time).
    struct Operand
    {
        bool isNumber = true;
        double numberValue = 0.0;
        std::string identValue;
    };

    // A single named flag/variable test, optionally compared against an Operand. No comparison
    // (ComparisonOp::None) tests the name as a boolean flag.
    struct ConditionExpr
    {
        std::string name;
        ComparisonOp op = ComparisonOp::None;
        Operand rhs;
    };

    enum class ConditionKind
    {
        Expr,
        All,
        Any,
        Not,
    };

    // Recursive condition tree - exactly one of expr/children/child is meaningful, selected by
    // kind. A vector of unique_ptr (not by-value Condition, which would be an incomplete type)
    // since Condition contains itself recursively.
    struct Condition
    {
        ConditionKind kind = ConditionKind::Expr;
        ConditionExpr expr;                            // kind == Expr
        std::vector<std::unique_ptr<Condition>> children; // kind == All | Any
        std::unique_ptr<Condition> child;                 // kind == Not
    };

    struct Transition
    {
        std::string toState;
        std::unique_ptr<Condition> condition; // Always non-null, even for an empty body (All with no children -> unconditional).
    };

    struct StateDecl
    {
        std::string name;
        std::vector<Transition> transitions;
    };

    // One fieldAssignment's value - unifies NUMBER/IDENT/STRING into one type so callers don't
    // need to know which literal form was used to write a given field (e.g. an enum-like field
    // such as guardHeight is authored as a bare IDENT - "mid" - and read via stringValue).
    struct FieldValue
    {
        bool isNumber = true;
        double numberValue = 0.0;
        std::string stringValue;
    };
    using FieldBlock = std::vector<std::pair<std::string, FieldValue>>;

    struct Cancel
    {
        std::string toMoveId;
        std::unique_ptr<Condition> condition;
    };

    // hitbox/sfx/vfx sub-blocks are anonymous (never referenced by id from elsewhere) and
    // structurally identical - a repeatable bag of fieldAssignments - so one FieldBlock type
    // covers all three; only which vector a given block lands in carries meaning.
    struct MoveDecl
    {
        std::string id;
        FieldBlock fields;
        std::vector<Cancel> cancels;
        std::vector<FieldBlock> hitboxes;
        std::vector<FieldBlock> sfxCues;
        std::vector<FieldBlock> vfxCues;
    };

    // A character's own identity/stat/correction data, authored in that character's .combat file
    // alongside (or referencing) its moveset. Like hitbox/sfx/vfx, the three sub-blocks are
    // anonymous, structurally identical bags of fieldAssignments (one FieldBlock each) - only
    // which member a block lands in carries meaning. Kept stringly-typed here for the same
    // reason MoveDecl is: this layer only parses shape, Game::MoveTable.cpp maps names onto real
    // typed fields (and logs+skips unknown ones, keeping authoring additive).
    //   fields     - top-level: name, model, moves (the moveset file to also load), ...
    //   stats      - maxHealth, walkSpeed, runSpeed, jumpSpeed
    //   correction - assetScale, facingDegrees, groundingOffsetY (per-asset render fix-ups)
    //   ko         - dropOffsetY, dropStartFrame, dropEndFrame (KO settle-to-ground ramp)
    struct CharacterDecl
    {
        FieldBlock fields;
        FieldBlock stats;
        FieldBlock correction;
        FieldBlock ko;
    };

    struct CombatFile
    {
        std::vector<StateDecl> states;
        std::vector<MoveDecl> moves;
        // Present only when the parsed file authored a "character" block (a character's own
        // file); nullopt for states.combat / moves.combat.
        std::optional<CharacterDecl> character;
    };

    // Strict all-or-nothing parse (matches Asset::ImportGltf's own "fail loudly" convention) -
    // logs the offending line via Foundation::Log and returns false on any syntax error, rather
    // than guessing or partially populating outFile.
    bool ParseCombatFile(const std::string& source, CombatFile& outFile);

    // Reads filePath's full contents and parses it - the only disk-reading entry point, since no
    // other .combat consumer exists. A plain std::ifstream read directly here, matching this
    // project's minimal-dependency style (no Foundation/Engine file-I/O surface exists to reuse).
    bool LoadCombatFile(const std::string& filePath, CombatFile& outFile);

    // Caller-supplied lookup for every named flag/variable EvaluateCondition encounters - the
    // evaluator itself has no built-in vocabulary (see the file-level comment above).
    struct EvaluationContext
    {
        std::function<bool(const std::string&)> getFlag;
        std::function<float(const std::string&)> getNumber;
    };
    bool EvaluateCondition(const Condition& condition, const EvaluationContext& context);

    // Parses a small hand-written example inline (not from a file) and checks its structure/
    // evaluation against known-correct expectations - this project's standing lightweight
    // verification style (see main.cpp's own startup OMD_ASSERT sanity checks) in place of a
    // unit test framework. Safe to call once at every startup; asserts on failure.
    void RunSelfTest();
}
