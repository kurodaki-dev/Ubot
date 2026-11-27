#include "../Ubot.hpp"

#include <Geode/modify/EditLevelLayer.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

using namespace geode::prelude;
using namespace ubot;

// Vrai pendant que c'est notre bouton qui lance le niveau
static bool s_fromUbot = false;

namespace {
    CCMenuItemSpriteExtra* makeButton(char const* text, char const* bg, float scale, CCObject* target, SEL_MenuHandler sel) {
        auto spr = ButtonSprite::create(text, "bigFont.fnt", bg, 0.8f);
        spr->setScale(scale);
        return CCMenuItemSpriteExtra::create(spr, target, sel);
    }

    // Crée le menu Ubot : [Bot Showcase] [Supprimer]
    CCMenu* addUbotMenu(CCNode* parent, CCPoint pos, CCObject* target, SEL_MenuHandler onShowcase, SEL_MenuHandler onDelete) {
        auto menu = CCMenu::create();
        menu->setID("ubot-menu"_spr);
        menu->setPosition(pos);

        auto showcase = makeButton("Bot Showcase", "GJ_button_01.png", 0.5f, target, onShowcase);
        showcase->setID("bot-showcase-button"_spr);
        menu->addChild(showcase);

        auto del = makeButton("Suppr.", "GJ_button_06.png", 0.5f, target, onDelete);
        del->setID("delete-macro-button"_spr);
        menu->addChild(del);

        float w1 = showcase->getContentSize().width;
        float w2 = del->getContentSize().width;
        float gap = 6.f;
        float total = w1 + gap + w2;
        showcase->setPosition({-total / 2.f + w1 / 2.f, 0.f});
        del->setPosition({total / 2.f - w2 / 2.f, 0.f});

        parent->addChild(menu, 20);
        return menu;
    }

    // Position sous un noeud existant (en coordonnées du parent du menu)
    CCPoint below(CCNode* parent, CCNode* anchor, float offset) {
        auto world = anchor->getParent()
            ? anchor->getParent()->convertToWorldSpace(anchor->getPosition())
            : anchor->getPosition();
        auto local = parent->convertToNodeSpace(world);
        return {local.x, local.y - offset};
    }

    void confirmDelete(GJGameLevel* level) {
        if (!level) return;
        if (!hasMacro(level)) {
            Notification::create("Ubot: aucune macro pour ce niveau", NotificationIcon::Info)->show();
            return;
        }
        Ref<GJGameLevel> keep = level;
        createQuickPopup(
            "Ubot",
            "Supprimer la macro de ce niveau ?\nLe prochain <cy>Bot Showcase</c> recalculera <cr>tout</c> depuis zero.",
            "Annuler", "Supprimer",
            [keep](FLAlertLayer*, bool yes) {
                if (!yes) return;
                if (deleteMacro(keep)) {
                    Notification::create("Ubot: macro supprimee", NotificationIcon::Success)->show();
                } else {
                    Notification::create("Ubot: impossible de supprimer la macro", NotificationIcon::Error)->show();
                }
            }
        );
    }
}

// ---------------------------------------------------------------------------
// Page d'un niveau en ligne
// ---------------------------------------------------------------------------
class $modify(UbotLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;
        auto win = CCDirector::get()->getWinSize();
        CCPoint pos = m_playBtnMenu
            ? below(this, m_playBtnMenu, 62.f)
            : CCPoint{win.width / 2.f, win.height / 2.f - 20.f};
        addUbotMenu(this, pos, this,
            menu_selector(UbotLevelInfoLayer::onUbotShowcase),
            menu_selector(UbotLevelInfoLayer::onUbotDelete));
        return true;
    }

    void onUbotShowcase(CCObject*) {
        rt().pendingShowcase = true;
        s_fromUbot = true;
        this->onPlay(nullptr);
        s_fromUbot = false;
    }

    void onUbotDelete(CCObject*) { confirmDelete(m_level); }

    void onPlay(CCObject* sender) {
        // bouton Play normal : pas de showcase
        if (!s_fromUbot) rt().pendingShowcase = false;
        LevelInfoLayer::onPlay(sender);
    }
};

// ---------------------------------------------------------------------------
// Page d'un niveau local (créé dans l'éditeur)
// ---------------------------------------------------------------------------
class $modify(UbotEditLevelLayer, EditLevelLayer) {
    bool init(GJGameLevel* level) {
        if (!EditLevelLayer::init(level)) return false;
        auto win = CCDirector::get()->getWinSize();
        CCPoint pos{win.width / 2.f, win.height / 2.f - 95.f};
        if (auto play = this->getChildByIDRecursive("play-button")) {
            pos = below(this, play, 45.f);
        }
        addUbotMenu(this, pos, this,
            menu_selector(UbotEditLevelLayer::onUbotShowcase),
            menu_selector(UbotEditLevelLayer::onUbotDelete));
        return true;
    }

    void onUbotShowcase(CCObject*) {
        rt().pendingShowcase = true;
        s_fromUbot = true;
        this->onPlay(nullptr);
        s_fromUbot = false;
    }

    void onUbotDelete(CCObject*) { confirmDelete(m_level); }

    void onPlay(CCObject* sender) {
        if (!s_fromUbot) rt().pendingShowcase = false;
        EditLevelLayer::onPlay(sender);
    }
};

// ---------------------------------------------------------------------------
// Menu pause de l'éditeur
// ---------------------------------------------------------------------------
class $modify(UbotEditorPauseLayer, EditorPauseLayer) {
    bool init(LevelEditorLayer* layer) {
        if (!EditorPauseLayer::init(layer)) return false;
        auto win = CCDirector::get()->getWinSize();
        addUbotMenu(this, {win.width / 2.f, 28.f}, this,
            menu_selector(UbotEditorPauseLayer::onUbotShowcase),
            menu_selector(UbotEditorPauseLayer::onUbotDelete));
        return true;
    }

    void onUbotShowcase(CCObject*) {
        rt().pendingShowcase = true;
        s_fromUbot = true;
        this->onSaveAndPlay(nullptr);
        s_fromUbot = false;
    }

    void onUbotDelete(CCObject*) {
        confirmDelete(m_editorLayer ? m_editorLayer->m_level : nullptr);
    }

    void onSaveAndPlay(CCObject* sender) {
        if (!s_fromUbot) rt().pendingShowcase = false;
        EditorPauseLayer::onSaveAndPlay(sender);
    }
};

// ---------------------------------------------------------------------------
// Menu pause en jeu : "Recalculer" supprime la macro et relance tout le calcul
// ---------------------------------------------------------------------------
class $modify(UbotPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (!isShowcaseRunning()) return;

        auto win = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setID("ubot-pause-menu"_spr);
        menu->setPosition({win.width - 70.f, 28.f});
        auto btn = makeButton("Recalculer", "GJ_button_06.png", 0.55f, this,
            menu_selector(UbotPauseLayer::onUbotRecompute));
        btn->setID("recompute-button"_spr);
        menu->addChild(btn);
        this->addChild(menu, 20);
    }

    void onUbotRecompute(CCObject*) {
        createQuickPopup(
            "Ubot",
            "Supprimer la macro actuelle et <cr>tout recalculer</c> pour ce niveau ?",
            "Annuler", "Recalculer",
            [this](FLAlertLayer*, bool yes) {
                if (!yes) return;
                resolveCurrentLevel();
                this->onResume(nullptr);
            }
        );
    }
};
