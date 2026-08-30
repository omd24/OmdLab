#include "CombatDsl.h"

#include "Foundation/Debug.h"
#include "Foundation/Log.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace
{
    using Foundation::Log::Severity;

    enum class TokenKind
    {
        Ident,
        Number,
        String,
        LBrace,
        RBrace,
        Op,
        End,
    };

    struct Token
    {
        TokenKind kind = TokenKind::End;
        std::string text;  // Ident/String text, or the operator's own spelling for Op.
        double number = 0.0;
        int line = 0;
    };

    bool IsIdentStart(char c)
    {
        return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
    }

    bool IsIdentChar(char c)
    {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    }

    // Tokenizes the whole source up front (small files, no need to interleave with parsing).
    // Returns an empty vector on a lexical error (already logged) - the parser treats that the
    // same as "immediately hit End" and fails cleanly rather than dereferencing nothing.
    std::vector<Token> Tokenize(const std::string& source)
    {
        std::vector<Token> tokens;
        size_t i = 0;
        int line = 1;
        const size_t n = source.size();
        while (i < n)
        {
            const char c = source[i];
            if (c == '\n')
            {
                ++line;
                ++i;
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(c)) != 0)
            {
                ++i;
                continue;
            }
            if (c == '#')
            {
                while (i < n && source[i] != '\n')
                {
                    ++i;
                }
                continue;
            }
            if (c == '{')
            {
                tokens.push_back({ TokenKind::LBrace, "{", 0.0, line });
                ++i;
                continue;
            }
            if (c == '}')
            {
                tokens.push_back({ TokenKind::RBrace, "}", 0.0, line });
                ++i;
                continue;
            }
            if (c == '"')
            {
                ++i;
                const size_t start = i;
                while (i < n && source[i] != '"')
                {
                    ++i;
                }
                if (i >= n)
                {
                    Foundation::Log::Write(Severity::Error, "CombatDsl", "line %d: unterminated string literal", line);
                    return {};
                }
                tokens.push_back({ TokenKind::String, source.substr(start, i - start), 0.0, line });
                ++i; // Closing quote.
                continue;
            }
            if (IsIdentStart(c))
            {
                const size_t start = i;
                while (i < n && IsIdentChar(source[i]))
                {
                    ++i;
                }
                tokens.push_back({ TokenKind::Ident, source.substr(start, i - start), 0.0, line });
                continue;
            }
            const bool looksLikeNumber =
                std::isdigit(static_cast<unsigned char>(c)) != 0 ||
                (c == '-' && i + 1 < n && std::isdigit(static_cast<unsigned char>(source[i + 1])) != 0);
            if (looksLikeNumber)
            {
                const size_t start = i;
                ++i;
                while (i < n && (std::isdigit(static_cast<unsigned char>(source[i])) != 0 || source[i] == '.'))
                {
                    ++i;
                }
                const std::string text = source.substr(start, i - start);
                tokens.push_back({ TokenKind::Number, text, std::atof(text.c_str()), line });
                continue;
            }
            if (c == '>' || c == '<' || c == '=' || c == '!')
            {
                std::string op(1, c);
                ++i;
                if (i < n && source[i] == '=')
                {
                    op += '=';
                    ++i;
                }
                else if (c == '=')
                {
                    // Bare '=' isn't a valid operator on its own - only '=='.
                    Foundation::Log::Write(Severity::Error, "CombatDsl", "line %d: unexpected '='", line);
                    return {};
                }
                tokens.push_back({ TokenKind::Op, op, 0.0, line });
                continue;
            }
            Foundation::Log::Write(Severity::Error, "CombatDsl", "line %d: unexpected character '%c'", line, c);
            return {};
        }
        tokens.push_back({ TokenKind::End, "", 0.0, line });
        return tokens;
    }

    // Recursive-descent parser over the token stream produced above. `ok` latches false on the
    // first error and every subsequent Expect*/Parse* call becomes a no-op returning a harmless
    // default - lets the top-level ParseCombatFile bail out with one check instead of threading
    // a success bool through every recursive call.
    class Parser
    {
    public:
        explicit Parser(std::vector<Token> tokens) : m_tokens(std::move(tokens)) {}

        bool ok = true;

        Game::CombatDsl::CombatFile ParseFile()
        {
            Game::CombatDsl::CombatFile file;
            while (ok && Peek().kind != TokenKind::End)
            {
                if (PeekIsIdent("state"))
                {
                    file.states.push_back(ParseStateDecl());
                }
                else if (PeekIsIdent("move"))
                {
                    file.moves.push_back(ParseMoveDecl());
                }
                else if (PeekIsIdent("character"))
                {
                    if (file.character.has_value())
                    {
                        Fail("duplicate 'character' declaration");
                    }
                    else
                    {
                        file.character = ParseCharacterDecl();
                    }
                }
                else
                {
                    Fail("expected 'state', 'move', or 'character'");
                }
            }
            return file;
        }

    private:
        std::vector<Token> m_tokens;
        size_t m_pos = 0;

        const Token& Peek() const
        {
            return m_tokens[m_pos];
        }

        bool PeekIsIdent(const char* text) const
        {
            return Peek().kind == TokenKind::Ident && Peek().text == text;
        }

        Token Advance()
        {
            const Token token = Peek();
            if (token.kind != TokenKind::End)
            {
                ++m_pos;
            }
            return token;
        }

        void Fail(const char* message)
        {
            if (!ok)
            {
                return;
            }
            ok = false;
            Foundation::Log::Write(Severity::Error, "CombatDsl", "line %d: %s (got '%s')", Peek().line, message, Peek().text.c_str());
        }

        std::string ExpectIdent(const char* keyword)
        {
            if (!ok)
            {
                return {};
            }
            if (!PeekIsIdent(keyword))
            {
                Fail(keyword);
                return {};
            }
            return Advance().text;
        }

        // Any identifier, not a specific keyword - used for names (state names, move ids, field/flag names).
        std::string ExpectAnyIdent()
        {
            if (!ok)
            {
                return {};
            }
            if (Peek().kind != TokenKind::Ident)
            {
                Fail("expected identifier");
                return {};
            }
            return Advance().text;
        }

        void ExpectLBrace()
        {
            if (!ok)
            {
                return;
            }
            if (Peek().kind != TokenKind::LBrace)
            {
                Fail("expected '{'");
                return;
            }
            Advance();
        }

        void ExpectRBrace()
        {
            if (!ok)
            {
                return;
            }
            if (Peek().kind != TokenKind::RBrace)
            {
                Fail("expected '}'");
                return;
            }
            Advance();
        }

        bool AtRBraceOrEnd() const
        {
            return Peek().kind == TokenKind::RBrace || Peek().kind == TokenKind::End;
        }

        Game::CombatDsl::StateDecl ParseStateDecl()
        {
            Game::CombatDsl::StateDecl state;
            ExpectIdent("state");
            state.name = ExpectAnyIdent();
            ExpectLBrace();
            while (ok && !AtRBraceOrEnd())
            {
                state.transitions.push_back(ParseTransitionDecl());
            }
            ExpectRBrace();
            return state;
        }

        Game::CombatDsl::Transition ParseTransitionDecl()
        {
            Game::CombatDsl::Transition transition;
            ExpectIdent("transition");
            transition.toState = ExpectAnyIdent();
            ExpectLBrace();
            transition.condition = ParseConditionItemList();
            ExpectRBrace();
            return transition;
        }

        // Parses zero or more conditionItems and combines them as an implicit "all" - covers
        // transitionDecl/cancelDecl/allBlock's shared "body is implicitly AND'ed" rule in one
        // place. Zero items produces an empty All, which EvaluateCondition treats as vacuously
        // true (an unconditional transition/cancel).
        std::unique_ptr<Game::CombatDsl::Condition> ParseConditionItemList()
        {
            auto all = std::make_unique<Game::CombatDsl::Condition>();
            all->kind = Game::CombatDsl::ConditionKind::All;
            while (ok && !AtRBraceOrEnd())
            {
                all->children.push_back(ParseConditionItem());
            }
            return all;
        }

        std::unique_ptr<Game::CombatDsl::Condition> ParseConditionItem()
        {
            // "require" is pure sugar - consumed and discarded, never becomes an AST node.
            if (PeekIsIdent("require"))
            {
                Advance();
            }
            return ParseConditionTerm();
        }

        std::unique_ptr<Game::CombatDsl::Condition> ParseConditionTerm()
        {
            if (PeekIsIdent("all"))
            {
                return ParseAllOrAnyBlock(Game::CombatDsl::ConditionKind::All);
            }
            if (PeekIsIdent("any"))
            {
                return ParseAllOrAnyBlock(Game::CombatDsl::ConditionKind::Any);
            }
            if (PeekIsIdent("not"))
            {
                Advance();
                auto node = std::make_unique<Game::CombatDsl::Condition>();
                node->kind = Game::CombatDsl::ConditionKind::Not;
                node->child = ParseConditionTerm();
                return node;
            }
            return ParseConditionExpr();
        }

        std::unique_ptr<Game::CombatDsl::Condition> ParseAllOrAnyBlock(Game::CombatDsl::ConditionKind kind)
        {
            Advance(); // "all" or "any"
            ExpectLBrace();
            auto node = std::make_unique<Game::CombatDsl::Condition>();
            node->kind = kind;
            while (ok && !AtRBraceOrEnd())
            {
                node->children.push_back(ParseConditionItem());
            }
            ExpectRBrace();
            return node;
        }

        std::unique_ptr<Game::CombatDsl::Condition> ParseConditionExpr()
        {
            auto node = std::make_unique<Game::CombatDsl::Condition>();
            node->kind = Game::CombatDsl::ConditionKind::Expr;
            node->expr.name = ExpectAnyIdent();
            if (Peek().kind == TokenKind::Op)
            {
                node->expr.op = ParseComparisonOp(Advance().text);
                Game::CombatDsl::Operand rhs;
                if (Peek().kind == TokenKind::Number)
                {
                    rhs.isNumber = true;
                    rhs.numberValue = Advance().number;
                }
                else if (Peek().kind == TokenKind::Ident)
                {
                    rhs.isNumber = false;
                    rhs.identValue = Advance().text;
                }
                else
                {
                    Fail("expected number or identifier after comparison operator");
                }
                node->expr.rhs = rhs;
            }
            return node;
        }

        static Game::CombatDsl::ComparisonOp ParseComparisonOp(const std::string& op)
        {
            if (op == ">=") return Game::CombatDsl::ComparisonOp::GreaterEqual;
            if (op == "<=") return Game::CombatDsl::ComparisonOp::LessEqual;
            if (op == "==") return Game::CombatDsl::ComparisonOp::Equal;
            if (op == "!=") return Game::CombatDsl::ComparisonOp::NotEqual;
            if (op == ">") return Game::CombatDsl::ComparisonOp::Greater;
            if (op == "<") return Game::CombatDsl::ComparisonOp::Less;
            return Game::CombatDsl::ComparisonOp::None;
        }

        Game::CombatDsl::MoveDecl ParseMoveDecl()
        {
            Game::CombatDsl::MoveDecl move;
            ExpectIdent("move");
            move.id = ExpectAnyIdent();
            ExpectLBrace();
            while (ok && !AtRBraceOrEnd())
            {
                if (PeekIsIdent("cancel"))
                {
                    move.cancels.push_back(ParseCancelDecl());
                }
                else if (PeekIsIdent("hitbox"))
                {
                    move.hitboxes.push_back(ParseFieldBlock("hitbox"));
                }
                else if (PeekIsIdent("sfx"))
                {
                    move.sfxCues.push_back(ParseFieldBlock("sfx"));
                }
                else if (PeekIsIdent("vfx"))
                {
                    move.vfxCues.push_back(ParseFieldBlock("vfx"));
                }
                else if (Peek().kind == TokenKind::Ident)
                {
                    move.fields.push_back(ParseFieldAssignment());
                }
                else
                {
                    Fail("expected a field, 'cancel', 'hitbox', 'sfx', or 'vfx'");
                }
            }
            ExpectRBrace();
            return move;
        }

        // characterDecl has no name identifier (unlike state/move) - a file carries at most one,
        // so there is nothing to disambiguate. Its three recognized sub-blocks reuse
        // ParseFieldBlock verbatim, exactly as hitbox/sfx/vfx do inside a moveDecl.
        Game::CombatDsl::CharacterDecl ParseCharacterDecl()
        {
            Game::CombatDsl::CharacterDecl character;
            ExpectIdent("character");
            ExpectLBrace();
            while (ok && !AtRBraceOrEnd())
            {
                if (PeekIsIdent("stats"))
                {
                    character.stats = ParseFieldBlock("stats");
                }
                else if (PeekIsIdent("correction"))
                {
                    character.correction = ParseFieldBlock("correction");
                }
                else if (PeekIsIdent("ko"))
                {
                    character.ko = ParseFieldBlock("ko");
                }
                else if (Peek().kind == TokenKind::Ident)
                {
                    character.fields.push_back(ParseFieldAssignment());
                }
                else
                {
                    Fail("expected a field, 'stats', 'correction', or 'ko'");
                }
            }
            ExpectRBrace();
            return character;
        }

        Game::CombatDsl::Cancel ParseCancelDecl()
        {
            Game::CombatDsl::Cancel cancel;
            ExpectIdent("cancel");
            cancel.toMoveId = ExpectAnyIdent();
            ExpectLBrace();
            cancel.condition = ParseConditionItemList();
            ExpectRBrace();
            return cancel;
        }

        Game::CombatDsl::FieldBlock ParseFieldBlock(const char* keyword)
        {
            Game::CombatDsl::FieldBlock block;
            ExpectIdent(keyword);
            ExpectLBrace();
            while (ok && !AtRBraceOrEnd())
            {
                block.push_back(ParseFieldAssignment());
            }
            ExpectRBrace();
            return block;
        }

        std::pair<std::string, Game::CombatDsl::FieldValue> ParseFieldAssignment()
        {
            const std::string name = ExpectAnyIdent();
            Game::CombatDsl::FieldValue value;
            if (Peek().kind == TokenKind::Number)
            {
                value.isNumber = true;
                value.numberValue = Advance().number;
            }
            else if (Peek().kind == TokenKind::Ident)
            {
                value.isNumber = false;
                value.stringValue = Advance().text;
            }
            else if (Peek().kind == TokenKind::String)
            {
                value.isNumber = false;
                value.stringValue = Advance().text;
            }
            else
            {
                Fail("expected a number, identifier, or string value");
            }
            return { name, value };
        }
    };
}

namespace Game::CombatDsl
{
    bool ParseCombatFile(const std::string& source, CombatFile& outFile)
    {
        std::vector<Token> tokens = Tokenize(source);
        if (tokens.empty())
        {
            return false;
        }
        Parser parser(std::move(tokens));
        CombatFile parsed = parser.ParseFile();
        if (!parser.ok)
        {
            return false;
        }
        outFile = std::move(parsed);
        return true;
    }

    bool LoadCombatFile(const std::string& filePath, CombatFile& outFile)
    {
        std::ifstream file(filePath);
        if (!file)
        {
            Foundation::Log::Write(Severity::Error, "CombatDsl", "failed to open '%s'", filePath.c_str());
            return false;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        return ParseCombatFile(buffer.str(), outFile);
    }

    bool EvaluateCondition(const Condition& condition, const EvaluationContext& context)
    {
        switch (condition.kind)
        {
            case ConditionKind::Expr:
            {
                if (condition.expr.op == ComparisonOp::None)
                {
                    return context.getFlag(condition.expr.name);
                }
                const float lhs = context.getNumber(condition.expr.name);
                const float rhs =
                    condition.expr.rhs.isNumber ? static_cast<float>(condition.expr.rhs.numberValue) : context.getNumber(condition.expr.rhs.identValue);
                switch (condition.expr.op)
                {
                    case ComparisonOp::GreaterEqual: return lhs >= rhs;
                    case ComparisonOp::LessEqual: return lhs <= rhs;
                    case ComparisonOp::Equal: return lhs == rhs;
                    case ComparisonOp::NotEqual: return lhs != rhs;
                    case ComparisonOp::Greater: return lhs > rhs;
                    case ComparisonOp::Less: return lhs < rhs;
                    case ComparisonOp::None: break; // Unreachable (handled above); keeps the switch exhaustive.
                }
                return false;
            }
            case ConditionKind::All:
            {
                for (const std::unique_ptr<Condition>& child : condition.children)
                {
                    if (!EvaluateCondition(*child, context))
                    {
                        return false;
                    }
                }
                return true;
            }
            case ConditionKind::Any:
            {
                for (const std::unique_ptr<Condition>& child : condition.children)
                {
                    if (EvaluateCondition(*child, context))
                    {
                        return true;
                    }
                }
                return false;
            }
            case ConditionKind::Not:
                return !EvaluateCondition(*condition.child, context);
        }
        return false;
    }

    void RunSelfTest()
    {
        // A small hand-written example exercising every grammar construct (state/transition,
        // move/cancel, require-as-sugar, nested all/any/not, comparisons, character block with
        // all three sub-blocks) - not real game content, just enough to catch a lexer/parser/
        // evaluator regression before it reaches real .combat content.
        constexpr const char* kSource = R"(
            state Idle
            {
                transition Walking
                {
                    require axisPressed
                }
            }

            state Walking
            {
                transition Attacking
                {
                    require lightPressed
                    require frame >= 2
                }

                transition Idle
                {
                    require animationFinished
                }
            }

            move LightPunch
            {
                damage 8
                startup 3

                cancel HeavyPunch
                {
                    all
                    {
                        hitConfirmed
                        frame >= 8
                        any
                        {
                            meter >= 20
                            poweredUp
                        }
                        not stunned
                    }
                }

                hitbox
                {
                    offsetX 0.3
                    frameStart 3
                    frameEnd 8
                }
            }

            character
            {
                name "Test Dummy"
                moves "moves.combat"

                stats
                {
                    maxHealth 120
                    walkSpeed 2.5
                }

                correction
                {
                    assetScale 100
                    groundingOffsetY -0.15
                }

                ko
                {
                    dropOffsetY -0.9
                    dropStartFrame 5
                    dropEndFrame 28
                }
            }
        )";

        CombatFile file;
        const bool parsed = ParseCombatFile(kSource, file);
        OMD_ASSERT(parsed, "CombatDsl self-test: failed to parse example source");
        OMD_ASSERT(file.states.size() == 2, "CombatDsl self-test: expected 2 states, got %zu", file.states.size());
        OMD_ASSERT(file.moves.size() == 1, "CombatDsl self-test: expected 1 move, got %zu", file.moves.size());
        OMD_ASSERT(file.states[1].transitions.size() == 2, "CombatDsl self-test: expected 2 transitions on state 1");
        OMD_ASSERT(file.moves[0].cancels.size() == 1, "CombatDsl self-test: expected 1 cancel");
        OMD_ASSERT(file.moves[0].hitboxes.size() == 1, "CombatDsl self-test: expected 1 hitbox sub-block");
        OMD_ASSERT(file.moves[0].fields.size() == 2, "CombatDsl self-test: expected 2 fields (damage, startup)");

        // The character block: present, top-level fields plus all three sub-blocks populated.
        OMD_ASSERT(file.character.has_value(), "CombatDsl self-test: expected a character block");
        OMD_ASSERT(file.character->fields.size() == 2, "CombatDsl self-test: expected 2 character fields (name, moves)");
        OMD_ASSERT(file.character->stats.size() == 2, "CombatDsl self-test: expected 2 stats fields");
        OMD_ASSERT(file.character->correction.size() == 2, "CombatDsl self-test: expected 2 correction fields");
        OMD_ASSERT(file.character->ko.size() == 3, "CombatDsl self-test: expected 3 ko fields");
        OMD_ASSERT(
            file.character->fields[0].first == "name" && !file.character->fields[0].second.isNumber &&
                file.character->fields[0].second.stringValue == "Test Dummy",
            "CombatDsl self-test: character 'name' field did not round-trip");
        OMD_ASSERT(
            file.character->ko[0].first == "dropOffsetY" && file.character->ko[0].second.isNumber &&
                file.character->ko[0].second.numberValue < 0.0 && file.character->ko[0].second.numberValue > -1.0,
            "CombatDsl self-test: character ko 'dropOffsetY' field did not round-trip (expected a small negative)");

        // Evaluate the "Attacking" transition's condition (require lightPressed; require frame >= 2)
        // against a context designed to prove both the flag check and the numeric comparison.
        const Condition& attackCondition = *file.states[1].transitions[0].condition;
        {
            EvaluationContext trueContext;
            trueContext.getFlag = [](const std::string& name) { return name == "lightPressed"; };
            trueContext.getNumber = [](const std::string& name) { return name == "frame" ? 5.0f : 0.0f; };
            OMD_ASSERT(EvaluateCondition(attackCondition, trueContext), "CombatDsl self-test: expected condition to hold");

            EvaluationContext falseContext = trueContext;
            falseContext.getNumber = [](const std::string&) { return 0.0f; }; // frame < 2 now.
            OMD_ASSERT(!EvaluateCondition(attackCondition, falseContext), "CombatDsl self-test: expected condition to fail (frame too low)");
        }

        // Evaluate the nested all/any/not cancel condition against contexts that flip each leaf.
        const Condition& cancelCondition = *file.moves[0].cancels[0].condition;
        {
            EvaluationContext context;
            context.getFlag = [](const std::string& name) { return name == "hitConfirmed" || name == "poweredUp"; };
            context.getNumber = [](const std::string& name) { return name == "frame" ? 10.0f : 0.0f; };
            OMD_ASSERT(EvaluateCondition(cancelCondition, context), "CombatDsl self-test: expected nested condition to hold");

            EvaluationContext stunnedContext = context;
            stunnedContext.getFlag = [](const std::string& name) { return name == "hitConfirmed" || name == "poweredUp" || name == "stunned"; };
            OMD_ASSERT(!EvaluateCondition(cancelCondition, stunnedContext), "CombatDsl self-test: expected 'not stunned' to fail the condition");
        }

        Foundation::Log::Write(Severity::Info, "CombatDsl", "self-test passed");
    }
}
