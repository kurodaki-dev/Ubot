# Ubot

Ajoute un bouton **Bot Showcase** sur la page des niveaux (en ligne, locaux et dans le menu pause de l'éditeur).

- **Bot Showcase** lance le niveau, mais c'est un algorithme qui joue.
- La première fois, le bot **calcule** le niveau. Il utilise le vrai moteur physique de GD (gravité, vitesse, portails, orbes, pads, triggers…) et essaie chaque instant possible pour cliquer ou relâcher, avec retour en arrière à chaque mort.
- La solution est vérifiée en la rejouant depuis le début, puis sauvegardée dans le dossier du mod (`macros/<id>.ubot`).
- Les fois suivantes, la macro est rejouée directement. Si le niveau a changé, elle est recalculée.
- **Suppr.** (à côté du bouton) supprime la macro du niveau. **Recalculer** (menu pause pendant un showcase) supprime la macro et relance tout le calcul.
- **Safe mode** : le niveau n'est jamais validé, aucun pourcentage n'est sauvegardé.

Les niveaux plateforme ne sont pas supportés.
