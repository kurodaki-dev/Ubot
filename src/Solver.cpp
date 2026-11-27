#include "Ubot.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace geode::prelude;

// ============================================================================
//  Fonctionnement du solveur
// ----------------------------------------------------------------------------
//  Un "chemin" est la liste des ticks physiques (240 par seconde) où l'on
//  appuie ou relâche le clic. Le solveur fait une recherche en profondeur :
//
//   1. Depuis un noeud (tick T, état du clic H), il simule sans rien changer
//      jusqu'à la mort (au tick D) ou jusqu'à une limite.
//   2. Tous les instants T+d (d <= D-T) où inverser le clic pourrait éviter
//      cette mort deviennent des candidats. On essaie d'abord le plus tardif.
//   3. Chaque candidat devient un nouveau noeud. Si tous ses descendants
//      meurent, l'état est marqué perdant (mémoïsé) et on revient en arrière.
//
//  Pour revenir dans le passé, le solveur prend des checkpoints du jeu
//  (keyframes) régulièrement et rejoue les inputs à partir de là. La gravité,
//  la vitesse, les portails, les orbes et les triggers sont simulés par le
//  vrai moteur de GD, donc toutes les contraintes du niveau sont respectées.
//
//  Quand un chemin finit le niveau, il est rejoué entièrement depuis le début
//  sans aucun checkpoint (vérification). En cas de désynchronisation, la
//  recherche reprend un peu avant l'endroit fautif à partir d'un état
//  reconstruit depuis le début du niveau.
// ============================================================================

namespace ubot {

namespace {
    constexpr size_t kMaxKeyframes = 900;
    constexpr int kMaxRebuilds = 8;
    constexpr uint32_t kRebuildStep = 240; // 1 seconde

    uint64_t mix(uint64_t h, uint64_t v) {
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h * 0x100000001b3ull;
    }

    int64_t q(double v, double scale) {
        return static_cast<int64_t>(std::llround(v * scale));
    }

    uint64_t playerKey(PlayerObject* p, uint64_t h) {
        if (!p) return h;
        h = mix(h, static_cast<uint64_t>(q(p->getPositionX(), 100.0)));
        h = mix(h, static_cast<uint64_t>(q(p->getPositionY(), 100.0)));
        h = mix(h, static_cast<uint64_t>(q(p->m_yVelocity, 1000.0)));
        uint64_t flags = 0;
        flags |= uint64_t(p->m_isShip) << 0;
        flags |= uint64_t(p->m_isBall) << 1;
        flags |= uint64_t(p->m_isBird) << 2;
        flags |= uint64_t(p->m_isDart) << 3;
        flags |= uint64_t(p->m_isRobot) << 4;
        flags |= uint64_t(p->m_isSpider) << 5;
        flags |= uint64_t(p->m_isSwing) << 6;
        flags |= uint64_t(p->m_isUpsideDown) << 7;
        flags |= uint64_t(p->m_isOnGround) << 8;
        flags |= uint64_t(p->m_isDashing) << 9;
        flags |= uint64_t(p->m_isGoingLeft) << 10;
        flags |= uint64_t(p->m_isSideways) << 11;
        h = mix(h, flags);
        h = mix(h, static_cast<uint64_t>(q(p->m_vehicleSize, 1000.0)));
        h = mix(h, static_cast<uint64_t>(q(p->m_playerSpeed, 1000.0)));
        return h;
    }

    void releaseAll(PlayLayer* pl) {
        if (pl->m_player1) pl->m_player1->releaseAllButtons();
        if (pl->m_player2) pl->m_player2->releaseAllButtons();
        rt().appliedHold = false;
    }
}

// ---------------------------------------------------------------------------

void Solver::start(Env env, LevelInfo info) {
    cancel();
    m_env = std::move(env);
    m_info = info;
    m_cap = info.fineControl ? 120 : 240;
    m_keyframeInterval = 60;
    m_path.clear();
    m_prefixHash.assign(1, 0xcbf29ce484222325ull);
    m_stack.clear();
    m_dead.clear();
    m_bestTick = 0;
    m_nodes = 0;
    m_ticksSimulated = 0;
    m_rebuildAttempts = 0;
    m_anchorTick = 0;
    m_solution.clear();
    m_solutionTicks = 0;
    m_failReason.clear();
    m_stalled = false;
    m_bestPercent = 0.f;
    m_startTime = std::chrono::steady_clock::now();

    auto& r = rt();
    r.path = &m_path;
    r.interceptEnd = true;

    m_env.reset();
    releaseAll(m_env.pl);
    m_lastKeyframeTick = 0;
    startRoot(0);
}

void Solver::cancel() {
    clearKeyframes();
    m_stack.clear();
    m_dead.clear();
    m_phase = Phase::Idle;
}

void Solver::fail(std::string reason) {
    m_failReason = std::move(reason);
    m_phase = Phase::Failed;
    clearKeyframes();
    log::warn("Ubot: echec du calcul: {}", m_failReason);
}

void Solver::clearKeyframes() {
    for (auto& [_, kf] : m_keyframes) {
        if (kf.cp) kf.cp->release();
    }
    m_keyframes.clear();
    m_lastKeyframeTick = 0;
}

size_t Solver::countBefore(uint32_t tick) const {
    return static_cast<size_t>(
        std::lower_bound(m_path.begin(), m_path.end(), tick) - m_path.begin()
    );
}

uint64_t Solver::prefixSig(size_t count) const {
    return m_prefixHash[count];
}

void Solver::pushToggle(uint32_t tick) {
    m_path.push_back(tick);
    m_prefixHash.push_back(mix(m_prefixHash.back(), tick));
}

void Solver::popToggles(size_t newSize) {
    if (newSize < m_path.size()) {
        m_path.resize(newSize);
        m_prefixHash.resize(newSize + 1);
    }
}

uint64_t Solver::stateKey(uint32_t tick, bool hold) const {
    uint64_t h = mix(0x51ed270b27a2f1c3ull, tick);
    h = mix(h, hold ? 1 : 2);
    h = playerKey(m_env.pl->m_player1, h);
    if (m_env.pl->m_gameState.m_isDualMode) {
        h = playerKey(m_env.pl->m_player2, h);
    }
    return h;
}

void Solver::maybeKeyframe() {
    auto& r = rt();
    uint32_t tick = r.tick;
    if (tick < m_lastKeyframeTick + m_keyframeInterval) return;
    // Uniquement quand le clic est relâché : un checkpoint ne garde pas
    // forcément l'état "maintenu" des boutons.
    if (tick > 0 && holdingAt(m_path, tick - 1)) return;

    size_t count = countBefore(tick);
    uint64_t sig = prefixSig(count);

    auto it = m_keyframes.find(tick);
    if (it != m_keyframes.end()) {
        if (it->second.sig == sig && it->second.toggleCount == count) {
            m_lastKeyframeTick = tick;
            return;
        }
        if (it->second.cp) it->second.cp->release();
        m_keyframes.erase(it);
    }

    auto cp = m_env.pl->createCheckpoint();
    if (!cp) return;
    cp->retain();
    m_keyframes[tick] = Keyframe{cp, tick, count, sig};
    m_lastKeyframeTick = tick;

    // Limite mémoire : on éclaircit les vieilles keyframes
    if (m_keyframes.size() > kMaxKeyframes) {
        bool odd = false;
        for (auto kit = m_keyframes.begin(); kit != m_keyframes.end();) {
            if (kit->first + 20 * m_keyframeInterval >= tick) break;
            if (odd) {
                if (kit->second.cp) kit->second.cp->release();
                kit = m_keyframes.erase(kit);
            } else {
                ++kit;
            }
            odd = !odd;
        }
    }
}

bool Solver::stepTick() {
    auto& r = rt();
    maybeKeyframe();
    r.clearFlags();
    uint32_t before = r.tick;
    int guard = 0;
    while (r.tick == before && !r.died && !r.completed && guard++ < 32) {
        m_env.stepRaw();
    }
    m_ticksSimulated++;
    // Fin du niveau : l'animation de fin démarre avant levelComplete()
    if (m_env.pl->m_levelEndAnimationStarted) r.completed = true;

    if (r.tick == before && !r.died && !r.completed) {
        m_stalled = true;
        return false;
    }

    if (auto p = m_env.pl->m_player1; p && m_env.pl->m_levelLength > 0.f) {
        float pct = std::clamp(p->getPositionX() / m_env.pl->m_levelLength * 100.f, 0.f, 100.f);
        if (pct > m_bestPercent) {
            m_bestPercent = pct;
            m_bestTick = r.tick;
        }
    }
    return !(r.died || r.completed);
}

void Solver::beginRestore(uint32_t tick, Phase after, bool forceReset) {
    auto& r = rt();
    m_restoreTarget = tick;
    m_afterRestore = after;

    Keyframe* best = nullptr;
    if (!forceReset) {
        auto it = m_keyframes.upper_bound(tick);
        int checked = 0;
        while (it != m_keyframes.begin() && checked++ < 64) {
            --it;
            auto& kf = it->second;
            if (kf.toggleCount > m_path.size()) continue;
            if (m_prefixHash[kf.toggleCount] != kf.sig) continue;
            if (kf.toggleCount < m_path.size() && m_path[kf.toggleCount] < kf.tick) continue;
            best = &kf;
            break;
        }
    }

    if (best) {
        m_env.pl->loadFromCheckpoint(static_cast<CheckpointObject*>(best->cp));
        r.tick = best->tick;
        m_lastKeyframeTick = best->tick;
    } else {
        m_env.reset();
        m_lastKeyframeTick = 0;
    }
    releaseAll(m_env.pl);
    r.clearFlags();
    m_phase = Phase::Restore;
}

void Solver::startRoot(uint32_t tick) {
    m_stack.clear();
    Node root;
    root.tick = tick;
    root.toggleCount = m_path.size();
    root.viaToggle = false;
    root.hold = holdingAt(m_path, tick);
    root.key = stateKey(tick, root.hold);
    m_stack.push_back(std::move(root));
    m_surviveCount = 0;
    m_phase = Phase::Survive;
}

void Solver::finishSearch() {
    // Le chemin courant termine le niveau : vérification depuis le vrai début
    log::info("Ubot: chemin trouve ({} inputs), verification...", m_path.size());
    clearKeyframes();
    m_stack.clear();
    m_env.reset();
    releaseAll(m_env.pl);
    m_lastKeyframeTick = 0;
    rt().clearFlags();
    m_phase = Phase::Verify;
}

Solver::Status Solver::work(double budgetMs) {
    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    auto& r = rt();

    auto maxMinutes = Mod::get()->getSettingValue<int64_t>("max-solve-minutes");
    if (maxMinutes > 0 && clock::now() - m_startTime > std::chrono::minutes(maxMinutes)) {
        fail(fmt::format("temps limite atteint ({} min), meilleur: {:.1f}%", maxMinutes, m_bestPercent));
    }

    while (m_phase != Phase::Done && m_phase != Phase::Failed && m_phase != Phase::Idle) {
        switch (m_phase) {
            case Phase::Restore: {
                if (r.tick >= m_restoreTarget) {
                    m_phase = m_afterRestore;
                    break;
                }
                if (!stepTick()) {
                    if (m_stalled) { fail("le jeu ne progresse plus (pause ?)"); break; }
                    if (r.completed) { finishSearch(); break; }
                    // Mort pendant le rejeu d'un préfixe connu : checkpoint imprécis.
                    if (m_afterRestore == Phase::CheckCandidate) {
                        m_phase = Phase::NextCandidate;
                    } else if (m_afterRestore == Phase::StartRoot) {
                        // reconstruction depuis le début impossible : on recule encore
                        m_desyncTick = r.tick > 0 ? r.tick - 1 : 0;
                        m_phase = Phase::Rebuild;
                    } else {
                        m_phase = Phase::NextCandidate;
                    }
                }
                break;
            }

            case Phase::Survive: {
                Node& n = m_stack.back();
                bool alive = stepTick();
                if (!alive) {
                    if (m_stalled) { fail("le jeu ne progresse plus (pause ?)"); break; }
                    if (r.completed) { finishSearch(); break; }
                }
                uint32_t elapsed = r.tick - n.tick;
                bool reachedCap = alive && elapsed >= m_cap;
                if (alive && !reachedCap) break;

                // Fenêtre d'action : on peut inverser le clic à n.tick + d
                uint32_t window = alive ? m_cap : (r.tick - 1 - n.tick);
                uint32_t dmin = (m_stack.size() == 1) ? 0 : 1;
                n.cands.clear();
                if (reachedCap) n.cands.push_back({m_cap, false});
                for (int64_t d = window; d >= static_cast<int64_t>(dmin); --d) {
                    n.cands.push_back({static_cast<uint32_t>(d), true});
                }
                n.next = 0;
                n.expanded = true;
                m_phase = Phase::NextCandidate;
                break;
            }

            case Phase::NextCandidate: {
                Node& n = m_stack.back();
                if (n.next >= n.cands.size()) {
                    // Tous les choix depuis cet état mènent à la mort
                    m_dead.insert(n.key);
                    size_t parentSize = n.toggleCount - (n.viaToggle ? 1 : 0);
                    m_stack.pop_back();
                    popToggles(parentSize);
                    if (m_stack.empty()) {
                        fail(fmt::format("aucune solution trouvee, meilleur: {:.1f}%", m_bestPercent));
                    }
                    break;
                }
                m_pending = n.cands[n.next++];
                popToggles(n.toggleCount);
                beginRestore(n.tick + m_pending.delay, Phase::CheckCandidate);
                break;
            }

            case Phase::CheckCandidate: {
                Node& n = m_stack.back();
                uint32_t target = n.tick + m_pending.delay;
                bool newHold = m_pending.toggle ? !n.hold : n.hold;
                uint64_t key = stateKey(target, newHold);
                if (m_dead.contains(key)) {
                    m_phase = Phase::NextCandidate;
                    break;
                }
                if (m_pending.toggle) pushToggle(target);
                Node child;
                child.tick = target;
                child.toggleCount = m_path.size();
                child.viaToggle = m_pending.toggle;
                child.hold = newHold;
                child.key = key;
                m_stack.push_back(std::move(child));
                m_nodes++;
                m_surviveCount = 0;
                m_phase = Phase::Survive;
                break;
            }

            case Phase::Verify: {
                if (stepTick()) break;
                if (m_stalled) { fail("le jeu ne progresse plus (pause ?)"); break; }
                if (r.completed) {
                    m_solution = m_path;
                    m_solutionTicks = r.tick;
                    clearKeyframes();
                    m_phase = Phase::Done;
                    log::info("Ubot: solution verifiee ({} ticks)", m_solutionTicks);
                    break;
                }
                // Désynchronisation : la solution meurt quand on la rejoue d'une traite
                m_desyncTick = r.tick > 0 ? r.tick - 1 : 0;
                log::warn("Ubot: desynchronisation au tick {}", m_desyncTick);
                m_phase = Phase::Rebuild;
                break;
            }

            case Phase::Rebuild: {
                if (m_desyncTick > m_lastDesyncTick) m_rebuildAttempts = 0;
                m_lastDesyncTick = std::max(m_lastDesyncTick, m_desyncTick);
                m_rebuildAttempts++;
                if (m_rebuildAttempts > kMaxRebuilds) {
                    fail(fmt::format("desynchronisation persistante vers le tick {}", m_desyncTick));
                    break;
                }
                uint32_t back = kRebuildStep * static_cast<uint32_t>(m_rebuildAttempts);
                m_anchorTick = m_desyncTick > back ? m_desyncTick - back : 0;
                popToggles(countBefore(m_anchorTick));
                clearKeyframes();
                m_dead.clear();
                m_stack.clear();
                // On reconstruit l'état en rejouant depuis le tout début (aucun checkpoint)
                beginRestore(m_anchorTick, Phase::StartRoot, true);
                break;
            }

            case Phase::StartRoot: {
                startRoot(m_anchorTick);
                break;
            }

            default: break;
        }

        if (std::chrono::duration<double, std::milli>(clock::now() - start).count() >= budgetMs) break;
    }

    if (m_phase == Phase::Done) return Status::Solved;
    if (m_phase == Phase::Failed || m_phase == Phase::Idle) return Status::Failed;
    return Status::Running;
}

std::string Solver::statusText() const {
    char const* phase = "Recherche";
    switch (m_phase) {
        case Phase::Verify: phase = "Verification"; break;
        case Phase::Rebuild:
        case Phase::StartRoot: phase = "Resynchronisation"; break;
        case Phase::Done: phase = "Termine"; break;
        case Phase::Failed: phase = "Echec"; break;
        default: break;
    }
    auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_startTime).count();
    return fmt::format(
        "Ubot - {}\nMeilleur: {:.1f}%  |  Noeuds: {}  |  Etats perdants: {}\nTicks simules: {}  |  {:.0f}s",
        phase, m_bestPercent, m_nodes, m_dead.size(), m_ticksSimulated, secs
    );
}

} // namespace ubot
