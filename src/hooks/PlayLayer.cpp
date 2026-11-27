#include "../Ubot.hpp"

#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>

using namespace geode::prelude;
using namespace ubot;

// Défini plus bas (a besoin de UbotPlayLayer)
static bool ubotUpdate(GJBaseGameLayer* layer, float dt);

static Solver& solver() {
    static Solver s_solver;
    return s_solver;
}

// ---------------------------------------------------------------------------
// Compteur de ticks + injection des inputs du bot + blocage des clics du joueur
// ---------------------------------------------------------------------------
class $modify(UbotBaseLayer, GJBaseGameLayer) {
    bool isUbotLayer() {
        auto& r = rt();
        return r.mode != Mode::Off && r.layer && static_cast<GJBaseGameLayer*>(r.layer) == this;
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!this->isUbotLayer()) {
            return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        }
        applyInputForTick(this);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        rt().tick++;
    }

    // En 2.2091 PlayLayer n'a pas son propre update : c'est celui de
    // GJBaseGameLayer qui est appelé à chaque image.
    void update(float dt) {
        auto& r = rt();
        if (r.layer && static_cast<GJBaseGameLayer*>(r.layer) == this && ubotUpdate(this, dt)) return;
        GJBaseGameLayer::update(dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        // Pendant le showcase, seul le bot joue
        if (this->isUbotLayer() && !rt().injecting) return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

// ---------------------------------------------------------------------------
// Safe mode : aucun pourcentage / record sauvegardé pendant un showcase
// ---------------------------------------------------------------------------
class $modify(UbotLevel, GJGameLevel) {
    void savePercentage(int percent, bool isPracticeMode, int clicks, int attempts, bool isChkValid) {
        if (rt().safeMode) return;
        GJGameLevel::savePercentage(percent, isPracticeMode, clicks, attempts, isChkValid);
    }
};

// ---------------------------------------------------------------------------
// PlayLayer
// ---------------------------------------------------------------------------
class $modify(UbotPlayLayer, PlayLayer) {
    struct Fields {
        bool showcase = false;
        bool solving = false;
        bool solveStarted = false;
        bool desyncWarned = false;
        bool endShown = false;      // un seul "Level Complete" par essai
        Macro macro;
        LevelInfo info;
        float accumulator = 0.f;
        float oldMusic = -1.f;
        float oldSfx = -1.f;
        CCLabelBMFont* label = nullptr;
        CCLayerColor* overlay = nullptr;     // écran de chargement (niveau masqué)
        CCLayerColor* barFill = nullptr;
        CCLabelBMFont* barText = nullptr;

        ~Fields() {
            if (!showcase) return;
            solver().cancel();
            auto& r = rt();
            r.mode = Mode::Off;
            r.layer = nullptr;
            r.path = nullptr;
            r.interceptEnd = false;
            r.safeMode = false;
            r.appliedHold = false;
            r.injecting = false;
            if (oldMusic >= 0.f) {
                auto fm = FMODAudioEngine::get();
                fm->setBackgroundMusicVolume(oldMusic);
                fm->setEffectsVolume(oldSfx);
            }
        }
    };

    bool init(GJGameLevel* level, bool dontCreateObjects) {
        auto& r = rt();
        bool showcase = r.pendingShowcase;
        r.pendingShowcase = false;

        if (!PlayLayer::init(level, dontCreateObjects)) return false;
        if (!showcase) return true;

        if (m_isPlatformer) {
            Notification::create("Ubot: les niveaux plateforme ne sont pas supportes", NotificationIcon::Warning)->show();
            return true;
        }

        auto f = m_fields.self();
        f->showcase = true;
        f->info = analyzeLevel(this);
        log::info("Ubot: analyse du niveau: {}", f->info.summary());

        // Safe mode : le niveau n'est jamais réellement validé
        m_isTestMode = true;
        r.safeMode = true;
        r.layer = this;
        r.tick = 0;
        r.appliedHold = false;

        auto win = CCDirector::get()->getWinSize();
        // Écran de chargement : le niveau est caché pendant le calcul
        f->overlay = CCLayerColor::create({8, 8, 20, 255});
        f->overlay->setVisible(false);
        this->addChild(f->overlay, 9999);

        auto title = CCLabelBMFont::create("Ubot calcule le niveau...", "bigFont.fnt");
        title->setScale(0.6f);
        title->setPosition({win.width / 2.f, win.height / 2.f + 40.f});
        f->overlay->addChild(title);

        constexpr float barW = 300.f, barH = 16.f;
        auto barBg = CCLayerColor::create({50, 50, 70, 255}, barW + 4.f, barH + 4.f);
        barBg->setPosition({win.width / 2.f - barW / 2.f - 2.f, win.height / 2.f - barH / 2.f - 2.f});
        f->overlay->addChild(barBg);

        f->barFill = CCLayerColor::create({80, 230, 90, 255}, 0.f, barH);
        f->barFill->setPosition({win.width / 2.f - barW / 2.f, win.height / 2.f - barH / 2.f});
        f->overlay->addChild(f->barFill);

        f->barText = CCLabelBMFont::create("0%", "bigFont.fnt");
        f->barText->setScale(0.45f);
        f->barText->setPosition({win.width / 2.f, win.height / 2.f - 28.f});
        f->overlay->addChild(f->barText);

        f->label = CCLabelBMFont::create("Ubot", "bigFont.fnt");
        f->label->setAnchorPoint({0.f, 1.f});
        f->label->setScale(0.3f);
        f->label->setPosition({8.f, win.height - 8.f});
        this->addChild(f->label, 10000);

        if (auto macro = loadMacro(level)) {
            f->macro = std::move(*macro);
            this->beginPlayback(false);
        } else {
            f->solving = true;
            r.mode = Mode::Solving;
            r.interceptEnd = true;
            r.path = nullptr; // le solveur fournit son chemin au démarrage
            f->overlay->setVisible(Mod::get()->getSettingValue<bool>("solve-overlay"));
            f->label->setString("Ubot - preparation...");
        }
        return true;
    }

    void beginPlayback(bool restart) {
        auto f = m_fields.self();
        auto& r = rt();
        f->solving = false;
        r.mode = Mode::Playback;
        r.interceptEnd = false;
        r.path = &f->macro.toggles;
        f->accumulator = 0.f;
        if (f->overlay) f->overlay->setVisible(false);
        if (f->label) {
            f->label->setString(fmt::format(
                "Ubot Showcase (Safe Mode)\n{} inputs - {:.1f}s",
                f->macro.toggles.size(), f->macro.totalTicks / 240.0
            ).c_str());
        }
        if (restart) this->resetLevel();
    }

    // Supprime la macro et relance le calcul complet sur place
    void restartSolve() {
        auto f = m_fields.self();
        if (!f->showcase) return;
        auto& r = rt();
        solver().cancel();
        deleteMacro(m_level);
        f->macro = Macro{};
        f->solving = true;
        f->solveStarted = false;
        f->desyncWarned = false;
        r.mode = Mode::Solving;
        r.interceptEnd = true;
        r.path = nullptr;
        if (f->overlay) f->overlay->setVisible(Mod::get()->getSettingValue<bool>("solve-overlay"));
        if (f->label) f->label->setString("Ubot - nouveau calcul...");
        r.allowReset = true;
        this->resetLevel();
        r.allowReset = false;
    }

    void unmute() {
        auto f = m_fields.self();
        if (f->oldMusic < 0.f) return;
        auto fm = FMODAudioEngine::get();
        fm->setBackgroundMusicVolume(f->oldMusic);
        fm->setEffectsVolume(f->oldSfx);
        f->oldMusic = f->oldSfx = -1.f;
    }

    void startSolver() {
        auto f = m_fields.self();
        f->solveStarted = true;

        // coupe le son : le niveau va être rejoué des milliers de fois
        auto fm = FMODAudioEngine::get();
        f->oldMusic = fm->m_musicVolume;
        f->oldSfx = fm->m_sfxVolume;
        fm->setBackgroundMusicVolume(0.f);
        fm->setEffectsVolume(0.f);

        Env env;
        env.pl = this;
        env.stepRaw = [this] { this->GJBaseGameLayer::update(1.f / 240.f); };
        env.reset = [this] {
            rt().allowReset = true;
            this->resetLevel();
            rt().allowReset = false;
        };
        solver().start(std::move(env), f->info);
    }

    void updateLoadingBar() {
        auto f = m_fields.self();
        float pct = std::clamp(solver().bestPercent(), 0.f, 100.f);
        if (f->barFill) f->barFill->setContentSize({300.f * pct / 100.f, 16.f});
        if (f->barText) f->barText->setString(fmt::format("{:.1f}%", pct).c_str());
    }

    // Retourne true si Ubot a géré l'image (l'update normal ne doit pas tourner)
    bool ubotTick(float dt) {
        auto f = m_fields.self();
        auto& r = rt();
        if (!f->showcase || r.mode == Mode::Off || !m_started) return false;

        if (r.mode == Mode::Solving) {
            if (!f->solveStarted) this->startSolver();

            double budget = static_cast<double>(Mod::get()->getSettingValue<int64_t>("solve-budget-ms"));
            auto status = solver().work(budget);
            if (f->label) f->label->setString(solver().statusText().c_str());
            this->updateLoadingBar();

            if (status == Solver::Status::Solved) {
                f->macro.levelID = m_level->m_levelID.value();
                f->macro.levelHash = levelHash(m_level);
                f->macro.levelName = m_level->m_levelName;
                f->macro.toggles = solver().solution();
                f->macro.totalTicks = solver().solutionTicks();
                f->macro.info = f->info;
                saveMacro(f->macro, m_level);
                solver().cancel();
                this->unmute();
                Notification::create("Ubot: niveau resolu et sauvegarde !", NotificationIcon::Success)->show();
                this->beginPlayback(true);
            } else if (status == Solver::Status::Failed) {
                auto reason = solver().failReason();
                solver().cancel();
                this->unmute();
                f->solving = false;
                r.mode = Mode::Off;
                r.interceptEnd = false;
                r.path = nullptr;
                if (f->overlay) f->overlay->setVisible(false);
                if (f->label) f->label->setString(fmt::format("Ubot: echec - {}\n(Safe Mode actif)", reason).c_str());
                Notification::create("Ubot: " + reason, NotificationIcon::Error)->show();
                this->resetLevel();
            }
            return true;
        }

        // Lecture : même découpage en pas de 1/240 s que pendant le calcul,
        // pour que la simulation soit identique à celle qui a été vérifiée.
        f->accumulator += dt * 240.f;
        int steps = static_cast<int>(f->accumulator);
        f->accumulator -= static_cast<float>(steps);
        steps = std::min(steps, 16);
        for (int i = 0; i < steps; ++i) {
            this->GJBaseGameLayer::update(1.f / 240.f);
        }
        return true;
    }

    void resetLevel() {
        auto f = m_fields.self();
        auto& r = rt();
        // Pendant le calcul, on bloque les resets programmés par le jeu
        // (animation de mort, fin du niveau, etc.) : seul le solveur décide.
        if (f->showcase && r.mode == Mode::Solving && f->solveStarted && !r.allowReset) return;
        PlayLayer::resetLevel();
        if (!f->showcase) return;
        f->endShown = false;
        r.tick = 0;
        r.appliedHold = false;
        if (m_player1) m_player1->releaseAllButtons();
        if (m_player2) m_player2->releaseAllButtons();
        f->accumulator = 0.f;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto f = m_fields.self();
        auto& r = rt();
        bool anticheat = m_anticheatSpike && object == m_anticheatSpike;
        if (f->showcase && !anticheat) {
            if (r.interceptEnd) {
                r.died = true; // le solveur gère la mort lui-même
                return;
            }
            if (r.mode == Mode::Playback && !f->desyncWarned) {
                f->desyncWarned = true;
                log::warn("Ubot: mort pendant la lecture au tick {} (desynchronisation)", r.tick);
                Notification::create(
                    "Ubot: desynchro ! Supprime la macro du niveau pour recalculer",
                    NotificationIcon::Warning
                )->show();
            }
        }
        PlayLayer::destroyPlayer(player, object);
    }

    // Pendant le calcul, l'animation de fin ne doit jamais démarrer : sinon elle
    // programme un "Level Complete" qui s'affiche plus tard pendant la lecture.
    void playEndAnimationToPos(CCPoint pos) {
        auto f = m_fields.self();
        if (f->showcase && rt().interceptEnd) {
            rt().completed = true;
            return;
        }
        PlayLayer::playEndAnimationToPos(pos);
    }

    void playPlatformerEndAnimationToPos(CCPoint pos, bool instant) {
        auto f = m_fields.self();
        if (f->showcase && rt().interceptEnd) {
            rt().completed = true;
            return;
        }
        PlayLayer::playPlatformerEndAnimationToPos(pos, instant);
    }

    void levelComplete() {
        auto f = m_fields.self();
        auto& r = rt();
        if (f->showcase) {
            if (r.interceptEnd) {
                r.completed = true;
                return;
            }
            if (f->endShown) return; // jamais deux écrans de fin
            f->endShown = true;
            m_isTestMode = true; // safe mode : rien n'est enregistré
        }
        PlayLayer::levelComplete();
    }
};

namespace ubot {
    bool isShowcaseRunning() {
        auto pl = PlayLayer::get();
        return pl && static_cast<UbotPlayLayer*>(pl)->m_fields->showcase;
    }

    void resolveCurrentLevel() {
        if (auto pl = PlayLayer::get()) {
            static_cast<UbotPlayLayer*>(pl)->restartSolve();
        }
    }
}

static bool ubotUpdate(GJBaseGameLayer* layer, float dt) {
    return static_cast<UbotPlayLayer*>(static_cast<PlayLayer*>(layer))->ubotTick(dt);
}
