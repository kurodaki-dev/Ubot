#include "../Ubot.hpp"

#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>

using namespace geode::prelude;
using namespace ubot;

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
        Macro macro;
        LevelInfo info;
        float accumulator = 0.f;
        float oldMusic = -1.f;
        float oldSfx = -1.f;
        CCLabelBMFont* label = nullptr;
        CCLayerColor* overlay = nullptr;

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
        f->overlay = CCLayerColor::create({0, 0, 0, 190});
        f->overlay->setVisible(false);
        this->addChild(f->overlay, 9999);

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
            f->label->setString("Ubot - en attente du debut du niveau...");
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
        this->resetLevel();
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
        env.stepRaw = [this] { this->PlayLayer::update(1.f / 240.f); };
        env.reset = [this] { this->resetLevel(); };
        solver().start(std::move(env), f->info);
    }

    void update(float dt) {
        auto f = m_fields.self();
        auto& r = rt();
        if (!f->showcase || r.mode == Mode::Off || !m_started) {
            return PlayLayer::update(dt);
        }

        if (r.mode == Mode::Solving) {
            if (!f->solveStarted) this->startSolver();

            double budget = static_cast<double>(Mod::get()->getSettingValue<int64_t>("solve-budget-ms"));
            auto status = solver().work(budget);
            if (f->label) f->label->setString(solver().statusText().c_str());

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
            return;
        }

        // Lecture : même découpage en pas de 1/240 s que pendant le calcul,
        // pour que la simulation soit identique à celle qui a été vérifiée.
        f->accumulator += dt * 240.f;
        int steps = static_cast<int>(f->accumulator);
        f->accumulator -= static_cast<float>(steps);
        steps = std::min(steps, 16);
        for (int i = 0; i < steps; ++i) {
            PlayLayer::update(1.f / 240.f);
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto f = m_fields.self();
        if (!f->showcase) return;
        auto& r = rt();
        r.tick = 0;
        r.appliedHold = false;
        if (m_player1) m_player1->releaseAllButtons();
        if (m_player2) m_player2->releaseAllButtons();
        f->accumulator = 0.f;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto f = m_fields.self();
        auto& r = rt();
        if (f->showcase && object != m_anticheatSpike) {
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

    void levelComplete() {
        auto f = m_fields.self();
        auto& r = rt();
        if (f->showcase) {
            if (r.interceptEnd) {
                r.completed = true;
                return;
            }
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
