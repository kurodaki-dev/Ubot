#pragma once

// Ubot - Bot Showcase
// Le bot utilise le moteur physique du jeu lui-même comme simulateur :
// gravité, vitesse, portails, orbes, pads et triggers sont donc pris en compte
// exactement comme quand tu joues, sans réécrire la physique de GD.

#include <Geode/Geode.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace ubot {

// ---------------------------------------------------------------------------
// Analyse du niveau (portails, triggers, etc.)
// ---------------------------------------------------------------------------
struct LevelInfo {
    int objects = 0;        // nombre total d'objets
    int triggers = 0;       // triggers (move, toggle, spawn, ...)
    int speedPortals = 0;
    int gravityPortals = 0;
    int modePortals = 0;    // ship, ball, ufo, wave, robot, spider, swing, cube
    int sizePortals = 0;
    int mirrorPortals = 0;
    int dualPortals = 0;
    int teleportPortals = 0;
    int orbsAndPads = 0;
    bool fineControl = false; // ship / ufo / wave / swing présents -> pas de recherche plus court

    std::string summary() const;
};

LevelInfo analyzeLevel(PlayLayer* pl);

// ---------------------------------------------------------------------------
// Macro sauvegardée (un fichier par niveau, dans le dossier de sauvegarde du mod)
// ---------------------------------------------------------------------------
struct Macro {
    int64_t levelID = 0;
    uint64_t levelHash = 0;
    std::string levelName;
    uint32_t totalTicks = 0;
    // Ticks physiques (240/s) où l'état "clic maintenu" s'inverse.
    // Au tick 0 le bouton est relâché. Strictement croissant.
    std::vector<uint32_t> toggles;
    LevelInfo info;
};

bool holdingAt(std::vector<uint32_t> const& toggles, uint32_t tick);
uint64_t fnv1a(std::string_view data, uint64_t seed = 0xcbf29ce484222325ull);
uint64_t levelHash(GJGameLevel* level);
std::string macroFileName(GJGameLevel* level);
std::optional<Macro> loadMacro(GJGameLevel* level);
bool saveMacro(Macro const& macro, GJGameLevel* level);
bool hasMacro(GJGameLevel* level);
// Supprime le fichier de la macro de ce niveau (le prochain showcase recalcule tout)
bool deleteMacro(GJGameLevel* level);

// Si un showcase est en cours : supprime sa macro et relance le calcul complet
void resolveCurrentLevel();
bool isShowcaseRunning();

// ---------------------------------------------------------------------------
// Etat global partagé entre les hooks
// ---------------------------------------------------------------------------
enum class Mode { Off, Solving, Playback };

struct Runtime {
    bool pendingShowcase = false;   // le bouton Bot Showcase vient d'être cliqué
    Mode mode = Mode::Off;
    PlayLayer* layer = nullptr;

    uint32_t tick = 0;              // ticks physiques depuis le dernier reset
    bool appliedHold = false;       // ce qu'on a réellement envoyé au jeu
    std::vector<uint32_t> const* path = nullptr; // inputs à rejouer

    bool died = false;              // la mort a été interceptée
    bool completed = false;         // la fin du niveau a été interceptée
    bool interceptEnd = false;      // pendant le calcul on bloque mort/fin
    bool injecting = false;         // l'input vient du bot (pas du joueur)
    bool safeMode = false;          // aucun progrès sauvegardé
    bool allowReset = false;        // pendant le calcul, seuls les resets du solveur passent

    void clearFlags() { died = false; completed = false; }
};

Runtime& rt();

// Applique au jeu l'input voulu pour le tick courant (appelé à chaque tick physique).
void applyInputForTick(GJBaseGameLayer* layer);
void setHold(GJBaseGameLayer* layer, bool hold);

// ---------------------------------------------------------------------------
// Solveur : recherche en profondeur avec retour arrière sur les instants de clic
// ---------------------------------------------------------------------------
struct Env {
    PlayLayer* pl = nullptr;
    std::function<void()> stepRaw;   // un appel à l'update d'origine avec dt = 1/240
    std::function<void()> reset;     // resetLevel d'origine
};

class Solver {
public:
    enum class Status { Running, Solved, Failed };

    void start(Env env, LevelInfo info);
    // Travaille pendant `budgetMs` millisecondes puis rend la main au jeu.
    Status work(double budgetMs);
    void cancel();

    std::string statusText() const;
    std::vector<uint32_t> const& solution() const { return m_solution; }
    uint32_t solutionTicks() const { return m_solutionTicks; }
    std::string const& failReason() const { return m_failReason; }
    float bestPercent() const { return m_bestPercent; }

private:
    struct Keyframe {
        cocos2d::CCObject* cp = nullptr; // CheckpointObject retenu
        uint32_t tick = 0;
        size_t toggleCount = 0;          // nombre de toggles avec tick < this->tick
        uint64_t sig = 0;                // hash de ces toggles
    };

    struct Candidate {
        uint32_t delay;
        bool toggle;
    };

    struct Node {
        uint32_t tick = 0;
        size_t toggleCount = 0;   // taille du chemin à ce noeud
        bool viaToggle = false;
        bool hold = false;
        uint64_t key = 0;
        bool expanded = false;
        std::vector<Candidate> cands;
        size_t next = 0;
    };

    enum class Phase {
        Idle,
        Restore,        // revenir à un tick donné (keyframe + rejeu)
        Survive,        // simuler sans changer l'input jusqu'à la mort / limite
        NextCandidate,  // essayer le prochain instant de clic/relâche
        CheckCandidate, // l'état du candidat est-il déjà connu comme perdant ?
        StartRoot,      // repartir d'un état reconstruit
        Verify,         // rejouer toute la solution depuis le début, sans keyframe
        Rebuild,        // reconstruire des keyframes sûres après une désynchro
        Done,
        Failed,
    };

    // outils
    bool stepTick();                 // false si mort ou fin (flags dans rt())
    void beginRestore(uint32_t tick, Phase after, bool forceReset = false);
    void maybeKeyframe();
    void clearKeyframes();
    uint64_t prefixSig(size_t count) const;
    size_t countBefore(uint32_t tick) const;
    uint64_t stateKey(uint32_t tick, bool hold) const;
    void pushToggle(uint32_t tick);
    void popToggles(size_t newSize);
    void startRoot(uint32_t tick);
    void fail(std::string reason);
    void finishSearch();

    Env m_env;
    LevelInfo m_info;
    Phase m_phase = Phase::Idle;
    Phase m_afterRestore = Phase::Idle;
    uint32_t m_restoreTarget = 0;

    std::vector<uint32_t> m_path;          // chemin courant
    std::vector<uint64_t> m_prefixHash;    // m_prefixHash[i] = hash des i premiers toggles
    std::vector<Node> m_stack;
    std::unordered_set<uint64_t> m_dead;   // états déjà prouvés perdants
    std::map<uint32_t, Keyframe> m_keyframes;
    uint32_t m_lastKeyframeTick = 0;

    uint32_t m_surviveCount = 0;
    uint32_t m_cap = 240;
    uint32_t m_keyframeInterval = 60;

    uint32_t m_bestTick = 0;
    uint64_t m_nodes = 0;
    uint64_t m_ticksSimulated = 0;
    int m_rebuildAttempts = 0;
    uint32_t m_anchorTick = 0;
    uint64_t m_deaths = 0;
    uint32_t m_desyncTick = 0;
    uint32_t m_lastDesyncTick = 0;
    bool m_stalled = false;
    float m_bestPercent = 0.f;
    Candidate m_pending{0, false};
    std::chrono::steady_clock::time_point m_startTime{};

    std::vector<uint32_t> m_solution;
    uint32_t m_solutionTicks = 0;
    std::string m_failReason;
};

} // namespace ubot
