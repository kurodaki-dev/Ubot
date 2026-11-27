# Ubot

Ajoute un bouton **Bot Showcase** sur la page des niveaux (en ligne, locaux et dans le menu pause de l'éditeur).

- **Bot Showcase** lance le niveau, mais c'est un algorithme qui joue.
- La première fois, le bot **calcule** le niveau. Il utilise le vrai moteur physique de GD (gravité, vitesse, portails, orbes, pads, triggers…) et essaie chaque instant possible pour cliquer ou relâcher, avec retour en arrière à chaque mort.
- L'algorithme suit des lois de gameplay pour ne pas tester l'inutile :
  1. **Actionnabilité** : en l'air sans orbe, un cube/robot/balle/araignée ne peut rien faire. Seuls les instants au sol, sur un orbe ou juste avant d'atterrir sont testés. Pour le vaisseau, la wave, l'ufo et le swing, chaque instant compte.
  2. **Fenêtre continue** : les bons instants de clic forment un intervalle. Le bot échantillonne grossièrement, puis affine.
  3. **Marge** : il évite de commencer par le clic frame-perfect.
  4. **Élargissement progressif** : budget de recherche par sous-arbre, multiplié par 4 si besoin (la recherche reste complète).
  5. **Convergence** : un état déjà prouvé perdant n'est jamais réexploré.
  6. **Retour rapide** : un point de retour est posé à chaque décision.
- La solution est vérifiée en la rejouant depuis le début, puis sauvegardée dans le dossier du mod (`macros/<id>.ubot`).
- Les fois suivantes, la macro est rejouée directement. Si le niveau a changé, elle est recalculée.
- **Suppr.** (à côté du bouton) supprime la macro du niveau. **Recalculer** (menu pause pendant un showcase) supprime la macro et relance tout le calcul.
- **Safe mode** : le niveau n'est jamais validé, aucun pourcentage n'est sauvegardé.

Les niveaux plateforme ne sont pas supportés.
