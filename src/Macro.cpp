#include "Ubot.hpp"

#include <Geode/utils/file.hpp>
#include <asp/fs/fs.hpp>

#include <algorithm>
#include <sstream>
#include <unordered_set>

using namespace geode::prelude;

namespace ubot {

Runtime& rt() {
    static Runtime s_runtime;
    return s_runtime;
}

bool holdingAt(std::vector<uint32_t> const& toggles, uint32_t tick) {
    // nombre de toggles <= tick ; impair = bouton maintenu
    auto it = std::upper_bound(toggles.begin(), toggles.end(), tick);
    return (std::distance(toggles.begin(), it) & 1) != 0;
}

void setHold(GJBaseGameLayer* layer, bool hold) {
    auto& r = rt();
    if (r.appliedHold == hold) return;
    r.appliedHold = hold;
    // handleButton gère aussi le mode dual (les deux joueurs suivent le clic)
    r.injecting = true;
    layer->handleButton(hold, static_cast<int>(PlayerButton::Jump), true);
    r.injecting = false;
}

void applyInputForTick(GJBaseGameLayer* layer) {
    auto& r = rt();
    if (!r.path) return;
    setHold(layer, holdingAt(*r.path, r.tick));
}

uint64_t fnv1a(std::string_view data, uint64_t seed) {
    uint64_t h = seed;
    for (unsigned char c : data) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

uint64_t levelHash(GJGameLevel* level) {
    if (!level) return 0;
    uint64_t h = fnv1a(level->m_levelString);
    return fnv1a(std::to_string(level->m_levelID.value()), h);
}

std::string macroFileName(GJGameLevel* level) {
    int id = level ? level->m_levelID.value() : 0;
    if (id > 0) return fmt::format("{}.ubot", id);
    // niveau local / éditeur : identifié par le hash de son contenu
    return fmt::format("local_{:016x}.ubot", levelHash(level));
}

static asp::fs::Path macroDir() {
    auto dir = Mod::get()->getSaveDir().join("macros");
    (void)file::createDirectoryAll(dir);
    return dir;
}

std::string LevelInfo::summary() const {
    return fmt::format(
        "{} objets, {} triggers, {} vitesses, {} gravites, {} modes, {} tailles, "
        "{} miroirs, {} duals, {} TP, {} orbes/pads",
        objects, triggers, speedPortals, gravityPortals, modePortals, sizePortals,
        mirrorPortals, dualPortals, teleportPortals, orbsAndPads
    );
}

LevelInfo analyzeLevel(PlayLayer* pl) {
    LevelInfo info;
    if (!pl || !pl->m_objects) return info;

    static const std::unordered_set<int> speed = {200, 201, 202, 203, 1334};
    static const std::unordered_set<int> gravity = {10, 11, 2926};
    static const std::unordered_set<int> modes = {12, 13, 47, 111, 660, 745, 1331, 1933};
    static const std::unordered_set<int> fine = {13, 111, 660, 1933};
    static const std::unordered_set<int> size = {99, 101};
    static const std::unordered_set<int> mirror = {45, 46};
    static const std::unordered_set<int> dual = {286, 287};
    static const std::unordered_set<int> teleport = {747, 2902};
    static const std::unordered_set<int> orbsPads = {
        35, 36, 67, 84, 140, 141, 1022, 1330, 1332, 1333, 1594, 1704, 1751, 3004, 3005, 3027
    };

    for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
        if (!obj) continue;
        info.objects++;
        int id = obj->m_objectID;
        if (obj->m_isTrigger) info.triggers++;
        if (speed.contains(id)) info.speedPortals++;
        if (gravity.contains(id)) info.gravityPortals++;
        if (modes.contains(id)) info.modePortals++;
        if (fine.contains(id)) info.fineControl = true;
        if (size.contains(id)) info.sizePortals++;
        if (mirror.contains(id)) info.mirrorPortals++;
        if (dual.contains(id)) info.dualPortals++;
        if (teleport.contains(id)) info.teleportPortals++;
        if (orbsPads.contains(id)) info.orbsAndPads++;
    }

    if (auto p = pl->m_player1) {
        if (p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing) info.fineControl = true;
    }
    return info;
}

std::optional<Macro> loadMacro(GJGameLevel* level) {
    auto path = macroDir().join(macroFileName(level));
    auto res = file::readString(path);
    if (!res) return std::nullopt;

    std::istringstream in(res.unwrap());
    std::string magic;
    in >> magic;
    if (magic != "UBOT2") return std::nullopt;

    Macro m;
    std::string key;
    while (in >> key) {
        if (key == "id") in >> m.levelID;
        else if (key == "hash") in >> std::hex >> m.levelHash >> std::dec;
        else if (key == "ticks") in >> m.totalTicks;
        else if (key == "name") { in >> std::ws; std::getline(in, m.levelName); }
        else if (key == "toggles") {
            size_t n = 0;
            in >> n;
            m.toggles.resize(n);
            for (auto& t : m.toggles) in >> t;
        }
        else { std::string skip; std::getline(in, skip); } // infos d'analyse : lecture humaine
    }
    if (!in.eof() && in.fail()) return std::nullopt;
    if (!std::is_sorted(m.toggles.begin(), m.toggles.end())) return std::nullopt;

    // Le niveau a changé (mise à jour du créateur ou édition) -> il faut recalculer
    if (m.levelHash != levelHash(level)) {
        log::info("Ubot: macro trouvee mais le niveau a change, nouveau calcul");
        return std::nullopt;
    }
    return m;
}

bool saveMacro(Macro const& m, GJGameLevel* level) {
    std::ostringstream out;
    out << "UBOT2\n";
    out << "id " << m.levelID << "\n";
    out << "hash " << std::hex << m.levelHash << std::dec << "\n";
    out << "ticks " << m.totalTicks << "\n";
    out << "info " << m.info.summary() << "\n";
    out << "name " << m.levelName << "\n";
    out << "toggles " << m.toggles.size() << "\n";
    for (size_t i = 0; i < m.toggles.size(); ++i) {
        out << m.toggles[i] << ((i % 16 == 15) ? '\n' : ' ');
    }
    out << "\n";

    auto path = macroDir().join(macroFileName(level));
    auto res = file::writeStringSafe(path, out.str());
    if (!res) {
        log::error("Ubot: impossible d'ecrire la macro dans {}", path.string());
        return false;
    }
    log::info("Ubot: macro sauvegardee dans {}", path.string());
    return true;
}

bool hasMacro(GJGameLevel* level) {
    return asp::fs::exists(macroDir().join(macroFileName(level)));
}

bool deleteMacro(GJGameLevel* level) {
    auto path = macroDir().join(macroFileName(level));
    if (!asp::fs::exists(path)) return false;
    auto res = asp::fs::removeFile(path);
    if (!res) {
        log::error("Ubot: impossible de supprimer {}", path.string());
        return false;
    }
    log::info("Ubot: macro supprimee: {}", path.string());
    return true;
}

} // namespace ubot
