# AlKanzar — matrice d'exigences du CRPG isométrique

Cette matrice remplace la checklist par un historique traçable des exigences.
Une exigence générale reste dans la matrice lorsqu'elle est raffinée : ses
sous-exigences disposent alors de leurs propres lignes.

## Itérations et légende

| Colonne | Livraison de référence |
|---|---|
| I0 | `7a4aa9d` — introduction de `TODO.md` et du backlog CRPG (2026-04-08) |
| I1 | `3b7ae2e` — modularisation du moteur `core` / `render` (2026-04-09) |
| I2 | `abe1ea6` — persistance des outils ImGui et consolidation animation/éditeur (2026-04-11) |
| I3 | `7db4e35` — navigation et pathfinding Polyanya (2026-08-01) |
| I4 | `bfa206f` — boucle de simulation fixe et feuille de route détaillée (2026-08-05) |
| I5 | `a8caa1e` — ECS des personnages, règles RPG et inspecteur ImGui (2026-08-06) |
| I6 | `feat(architecture): enforce layered RPG boundaries` — séparation contenu / règles / simulation / présentation (2026-08-06) |
| I7 | Travail courant — isolation des modes et outil de test déterministe (2026-08-06) |
| I8 | Travail courant — en-tête de contenu commun et format NAV versionné (2026-08-06) |
| I9 | Travail courant — scènes `SCN V1` déclaratives en Lua restreint (2026-08-06) |

Dans les colonnes d'itération : **I** = introduction, **R→** = raffinement en
sous-exigences, **L** = livraison, **I+L** = introduction et livraison dans la
même itération, **—** = aucun changement. Une exigence est **livrée** seulement
si sa ligne contient un `L`; sinon elle reste **à faire**. `L*` désigne une
fondation déjà présente lors de l'introduction de la feuille de route.

## Tableau des exigences

| ID | Domaine | Exigence | I0 | I1 | I2 | I3 | I4 | I5 | I6 | I7 | I8 | I9 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| VIS-01 | Vision | Créer un CRPG solo complet en 3D stylisée, à caméra isométrique fixe et en temps réel avec pause : campagne, groupe, exploration, combat, dialogues et progression. | I | — | — | — | R→J0…J5 | — | — | — | — | — |
| VIS-02 | Originalité | Produire un univers, des règles nommées, personnages, lieux, créatures, textes, quêtes, musiques et assets entièrement originaux ; Baldur's Gate I & II ne sont qu'une référence d'expérience. | I | — | — | — | — | — | — | — | — | — |
| FND-01 | Fondation | Disposer d'une application C++20 avec SDL2, OpenGL, CMake et journalisation. | L* | — | — | — | — | — | — | — | — | — |
| FND-02 | Fondation | Disposer d'un ECS, de transformations, d'une scène, de l'éclairage, du picking et de l'extraction de rendu. | I | L | — | — | — | R→J1-GRP-01 | — | — | — | — |
| FND-03 | Fondation | Disposer des rendus direct/différé, ombres, matériaux, import glTF et textures. | I | L | — | — | — | — | — | — | — | — |
| FND-04 | Fondation | Disposer d'un navmesh, de recherche de chemin et d'évitement d'agents. | I | — | — | L | R→J1-GRP-03…05 | — | — | — | — | — |
| FND-05 | Fondation | Disposer de systèmes initiaux d'animation, physique, tâches, profilage et éditeur. | I | L | L | — | R→J0-PRE-05 | — | — | — | — | — |
| FND-06 | Fondation | Disposer de tests unitaires moteur et d'un contrôle qualité des sources. | I | L | — | — | R→J5-01…07 | — | — | — | — | — |
| HIST-CHAR | Historique | Gérer les statistiques des personnages et le groupe. | I | — | — | — | R→J1-GRP-01,J2-PROG-01…06 | — | — | — | — | — |
| HIST-COMBAT | Historique | Fournir un combat en temps réel avec pause. | I | — | — | — | R→J1-CBT-01…05 | — | — | — | — | — |
| HIST-DIA | Historique | Fournir dialogues et quêtes. | I | — | — | — | R→J1-UI-04…05,J3-NAR-01…05 | — | — | — | — | — |
| HIST-WORLD | Historique | Fournir zones, cartes, exploration et voyage. | I | — | — | — | R→J3-ZON-01…05 | — | — | — | — | — |
| HIST-INV | Historique | Fournir inventaire, équipement et économie. | I | — | — | — | R→J1-UI-03,J2-ECO-01…04 | — | — | — | — | — |
| HIST-AUDIO | Historique | Fournir musique, ambiances, effets sonores et voix. | I | — | — | — | R→J4-AV-03 | — | — | — | — | — |
| HIST-SAVE | Historique | Sauvegarder et charger fidèlement la partie. | I | — | — | — | R→J1-UI-06,J5-03 | — | — | — | — | — |
| J0-ARC-01 | J0 · Architecture | Séparer explicitement les modes éditeur, jeu et outil de test. | — | — | — | — | I | — | — | R→J0-ARC-01.1…06, L | — | — |
| J0-ARC-01.1 | J0 · Architecture | Ajouter un état `TestTool` indépendant aux états Bootstrap, Gameplay, Editor et Shutdown. | — | — | — | — | — | — | — | I+L | — | — |
| J0-ARC-01.2 | J0 · Architecture | Définir centralement les systèmes, entrées et présentations autorisés pour chaque mode. | — | — | — | — | — | — | — | I+L | — | — |
| J0-ARC-01.3 | J0 · Architecture | Remplacer les conditions de mode dispersées par des capacités et transitions explicites. | — | — | — | — | — | — | — | I+L | — | — |
| J0-ARC-01.4 | J0 · Architecture | Isoler la sélection d'entité/composant de l'éditeur du leader de groupe utilisé par les ordres Gameplay. | — | — | — | — | — | — | — | I+L | — | — |
| J0-ARC-01.5 | J0 · Architecture | Lancer avec `--test-tool` une scène minimale déterministe sans UI éditeur ni entrée de gameplay. | — | — | — | — | — | — | — | I+L | — | — |
| J0-ARC-01.6 | J0 · Architecture | Tester les capacités, transitions, états, sélections, extraction isolée et scène déterministe des modes. | — | — | — | — | — | — | — | I+L | — | — |
| J0-ARC-02 | J0 · Architecture | Exécuter une simulation à pas fixe distincte du rendu, avec pause, accélération et delta temps sécurisé. | — | — | — | — | I+L | — | — | — | — | — |
| J0-ARC-03 | J0 · Architecture | Définir les couches `contenu → règles → simulation → présentation/UI`, sans dépendance des règles vers le rendu. | — | — | — | — | I | — | R→J0-ARC-03.1…05, L | — | — | — |
| J0-ARC-03.1 | J0 · Architecture | Isoler les données RPG sérialisables dans `core/content`. | — | — | — | — | — | — | I+L | — | — | — |
| J0-ARC-03.2 | J0 · Architecture | Compiler les règles pures dans une bibliothèque autonome liée uniquement au contenu. | — | — | — | — | — | — | I+L | — | — | — |
| J0-ARC-03.3 | J0 · Architecture | Adapter explicitement les composants ECS aux entrées de règles, sans accès au monde depuis les calculs. | — | — | — | — | — | — | I+L | — | — | — |
| J0-ARC-03.4 | J0 · Architecture | Placer l'inspecteur de personnage dans la couche éditeur/UI. | — | — | — | — | — | — | I+L | — | — | — |
| J0-ARC-03.5 | J0 · Architecture | Documenter les dépendances autorisées et les contrôler avec CTest. | — | — | — | — | — | — | I+L | — | — | — |
| J0-ARC-04 | J0 · Architecture | Créer un gestionnaire de ressources asynchrone : chargement, cache, références, erreurs, rechargement de développement et écran de chargement. | — | — | — | — | I | — | — | — | — | — |
| J0-ARC-05 | J0 · Architecture | Gérer configuration, profils, raccourcis reconfigurables, manette/clavier/souris, résolutions et fenêtre. | — | — | — | — | I | — | — | — | — | — |
| J0-ARC-06 | J0 · Architecture | Versionner contenus et sauvegardes, avec validation et erreurs exploitables. | — | — | — | — | I | — | — | — | R→J0-ARC-06.1…04 | R→J0-ARC-06.5…07 |
| J0-ARC-06.1 | J0 · Architecture | Réserver un en-tête commun de 10 octets `V<version><TYPE>`, complété jusqu'à sa taille fixe et indépendant du moteur. | — | — | — | — | — | — | — | — | I+L | — |
| J0-ARC-06.2 | J0 · Architecture | Écrire et valider les navmeshes avec le type `NAV`, la version portée par l'en-tête et un payload sans version dupliquée. | — | — | — | — | — | — | — | — | I+L | — |
| J0-ARC-06.3 | J0 · Architecture | Conserver la lecture de l'ancien format NAV texte et migrer le navmesh livré vers l'en-tête commun. | — | — | — | — | — | — | — | — | I+L | — |
| J0-ARC-06.4 | J0 · Architecture | Tester la taille, l'encodage, le décodage, les erreurs de validation, la compatibilité et le type des en-têtes de contenu. | — | — | — | — | — | — | — | — | I+L | — |
| J0-ARC-06.5 | J0 · Architecture | Attribuer `SCN V1` aux scènes avec l'en-tête texte visible `V1SCN-----`, toujours limité à 10 octets. | — | — | — | — | — | — | — | — | — | I+L |
| J0-ARC-06.6 | J0 · Architecture | Charger uniquement le payload Lua texte des scènes dans un runtime restreint en mémoire, instructions et capacités système. | — | — | — | — | — | — | — | — | — | I+L |
| J0-ARC-06.7 | J0 · Architecture | Valider strictement commandes, champs, types, enums et valeurs SCN, migrer la scène livrée et couvrir erreurs/compatibilité par des tests. | — | — | — | — | — | — | — | — | — | I+L |
| J0-ARC-07 | J0 · Architecture | Fournir builds debug/release, symboles, rapports de crash et CI multiplateforme exécutant les tests. | — | — | — | — | I | — | — | — | — | — |
| J0-PRE-01 | J0 · Présentation | Implémenter une caméra isométrique fixe : zoom borné, panoramique, rotation facultative par incréments et suivi doux du groupe. | — | — | — | — | I | — | — | — | — | — |
| J0-PRE-02 | J0 · Présentation | Ajouter culling, LOD, batching/instancing et budgets adaptés aux zones denses. | — | — | — | — | I | — | — | — | — | — |
| J0-PRE-03 | J0 · Présentation | Masquer ou rendre transparents les obstacles visuels, toits et volumes intérieurs. | — | — | — | — | I | — | — | — | — | — |
| J0-PRE-04 | J0 · Présentation | Créer l'éclairage de jeu : éventuel cycle jour/nuit, lumières locales, brouillard, particules et VFX lisibles. | — | — | — | — | I | — | — | — | — | — |
| J0-PRE-05 | J0 · Animation | Fournir une chaîne d'animation complète pour les personnages. | R→J0-PRE-05.1…07 | — | — | — | R→J0-PRE-05.8…10 | — | — | — | — | — |
| J0-PRE-05.1 | J0 · Animation | Représenter squelette, articulations, hiérarchie, inverse bind matrices et palette de skinning. | I | — | L | — | — | — | — | — | — | — |
| J0-PRE-05.2 | J0 · Animation | Représenter clips, canaux et keyframes de translation, rotation et échelle. | I | — | L | — | — | — | — | — | — | — |
| J0-PRE-05.3 | J0 · Animation | Évaluer et interpoler les poses d'animation. | I | — | L | — | — | — | — | — | — | — |
| J0-PRE-05.4 | J0 · Animation | Appliquer le skinning GPU aux personnages. | I | — | L | — | — | — | — | — | — | — |
| J0-PRE-05.5 | J0 · Animation | Charger squelettes, skins et animations depuis glTF. | I | L | — | — | — | — | — | — | — | — |
| J0-PRE-05.6 | J0 · Animation | Gérer états d'animation et transitions avec fondu. | I | — | L | — | — | — | — | — | — | — |
| J0-PRE-05.7 | J0 · Animation | Mettre à jour les animations par un système moteur planifié. | I | L | L | — | — | — | — | — | — | — |
| J0-PRE-05.8 | J0 · Animation | Ajouter les états de locomotion et de combat et leurs blend trees. | — | — | — | — | I | — | — | — | — | — |
| J0-PRE-05.9 | J0 · Animation | Déclencher des événements d'animation synchronisés avec la simulation. | — | — | — | — | I | — | — | — | — | — |
| J0-PRE-05.10 | J0 · Animation | Gérer armes, accessoires et points d'attache. | — | — | — | — | I | — | — | — | — | — |
| J0-TOOL-01 | J0 · Outils | Définir les formats source et livrés des modèles, textures, animations, audio, niveaux et données de jeu. | — | — | — | — | I | — | — | — | — | R→J0-TOOL-01.1 |
| J0-TOOL-01.1 | J0 · Outils | Rendre les scènes éditables en Lua avec `Create`, les méthodes d'objet, `scene.add` et un `scene.build()` final obligatoire. | — | — | — | — | — | — | — | — | — | I+L |
| J0-TOOL-02 | J0 · Outils | Valider références, IDs, données, accessibilité des zones et possibilité de terminer dialogues/quêtes. | — | — | — | — | I | — | — | — | — | — |
| J0-TOOL-03 | J0 · Outils | Étendre l'éditeur : entités, volumes, points d'intérêt, spawns, portes, déclencheurs, caméras et aperçu jeu. | — | — | — | — | I | — | — | — | — | — |
| J0-TOOL-04 | J0 · Outils | Créer les outils d'auteur de dialogues, quêtes, rencontres, butins et scripts, avec export versionné. | — | — | — | — | I | — | — | — | — | — |
| J0-EXIT | J0 · Validation | Charger depuis les données une zone navigable avec la caméra de jeu et au comportement identique en debug et release. | — | — | — | — | I | — | — | — | — | — |
| J1-GRP-01 | J1 · Personnages | Créer les composants runtime de personnage : identité, équipe, position, état, cible, contrôleur, statistiques, inventaire et rendu. | — | — | — | — | I | R→J1-GRP-01.1…07 | — | — | — | — |
| J1-GRP-01.1 | J1 · Personnages | Partager entre PJ et PNJ un profil ECS : affiliation, race, kit, expérience et rayon d'indicateur. | — | — | — | — | — | I+L | — | — | — | — |
| J1-GRP-01.2 | J1 · Personnages | Séparer caractéristiques, compétences et ressources PV/PM en composants au cycle de vie cohérent. | — | — | — | — | — | I+L | — | — | — | — |
| J1-GRP-01.3 | J1 · Personnages | Fournir des profils de démonstration joueur, PNJ amical et PNJ hostile, sans IA PNJ. | — | — | — | — | — | I+L | — | — | — | — |
| J1-GRP-01.4 | J1 · Personnages | Afficher un anneau vert pour le joueur, bleu pour un PNJ amical et rouge pour un PNJ hostile, en rendu direct et différé. | — | — | — | — | — | I+L | — | — | — | — |
| J1-GRP-01.5 | J1 · Personnages | Séparer affiliation visuelle, équipe de combat, faction, réputation et disposition individuelle. | — | — | — | — | — | I | — | — | — | — |
| J1-GRP-01.6 | J1 · Personnages | Ajouter vivant/à terre/mort, cible, contrôleur, ordre, inventaire, équipement et effets actifs. | — | — | — | — | — | I | — | — | — | — |
| J1-GRP-01.7 | J1 · Personnages | Définir IDs persistants et sérialisation des personnages entre scènes, sauvegardes et zones. | — | — | — | — | — | I | — | — | — | — |
| J1-GRP-02 | J1 · Sélection | Sélectionner un ou plusieurs personnages par clic, cadre, portraits et raccourcis de groupe. | — | — | — | — | I | R→J1-GRP-02.1…04 | — | — | — | — |
| J1-GRP-02.1 | J1 · Sélection | Rediriger le picking d'une sous-section glTF vers la racine ECS du personnage. | — | — | — | — | — | I+L | — | — | — | — |
| J1-GRP-02.2 | J1 · Sélection | Distinguer sélection d'édition et sélection du groupe en jeu. | — | — | — | — | — | I | — | L | — | — |
| J1-GRP-02.3 | J1 · Sélection | Ajouter sélection multiple, cadre, modificateur, chef et limite de groupe. | — | — | — | — | — | I | — | — | — | — |
| J1-GRP-02.4 | J1 · Sélection | Ajouter portraits, raccourcis numériques, recentrage caméra et retours audiovisuels. | — | — | — | — | — | I | — | — | — | — |
| J1-GRP-03 | J1 · Ordres | Donner par clic des ordres de déplacement, interaction, attaque, maintien et annulation, avec chemin, destination et curseur. | — | — | — | — | I | — | — | — | — | — |
| J1-GRP-04 | J1 · Déplacement | Gérer formations, retardataires, anti-blocage, répartition autour des cibles, portes et passages étroits. | — | — | — | — | I | — | — | — | — | — |
| J1-GRP-05 | J1 · Déplacement | Respecter zones interdites, obstacles dynamiques, hauteur et transitions de navmesh. | — | — | — | — | I | — | — | — | — | — |
| J1-CBT-01 | J1 · Combat | Définir acquisition de cible, portée, ligne de vue, attaque, récupération, interruption et mort/inconscience. | — | — | — | — | I | — | — | — | — | — |
| J1-CBT-02 | J1 · Combat | Implémenter pause active, éventuel ralenti et file d'ordres visible exécutée à la reprise. | — | — | — | — | I | — | — | — | — | — |
| J1-CBT-03 | J1 · Combat | Ajouter jets, modificateurs, défense, dégâts, critiques, résistances, immunités et journal compréhensible. | — | — | — | — | I | — | — | — | — | — |
| J1-CBT-04 | J1 · Combat | Créer les IA minimales neutre, alliée, agressive, distance, fuite, priorité de cible et retour. | — | — | — | — | I | — | — | — | — | — |
| J1-CBT-05 | J1 · Combat | Ajouter projectiles, zones d'effet, effets temporaires, mort, butin et nettoyage de combat. | — | — | — | — | I | — | — | — | — | — |
| J1-UI-01 | J1 · Interaction | Ajouter picking de jeu, surbrillance, interactions, portes, coffres, objets au sol, déclencheurs et transitions. | — | — | — | — | I | — | — | — | — | — |
| J1-UI-02 | J1 · UI | Créer le HUD : portraits, PV, états, sélection, actions, pause/vitesse, notifications et journal de combat. | — | — | — | — | I | — | — | — | — | — |
| J1-UI-03 | J1 · Inventaire | Créer une fenêtre simple d'inventaire, équipement, transfert de butin et utilisation d'objet. | — | — | — | — | I | — | — | — | — | — |
| J1-UI-04 | J1 · Dialogue | Implémenter un dialogue à embranchements avec conditions et conséquences. | — | — | — | — | I | — | — | — | — | — |
| J1-UI-05 | J1 · Quête | Implémenter une quête courte : déclenchement, objectifs, progression, récompense, échec et journal. | — | — | — | — | I | — | — | — | — | — |
| J1-UI-06 | J1 · Sauvegarde | Sauvegarder/charger manuellement monde, groupe, objets, quête et zone courante. | — | — | — | — | I | — | — | — | — | — |
| J1-EXIT | J1 · Validation | Explorer, combattre, dialoguer, terminer une quête, changer de zone et restaurer exactement l'état sauvegardé. | — | — | — | — | I | — | — | — | — | — |
| J2-PROG-01 | J2 · Règles | Concevoir les attributs, ressources, compétences, jets, difficulté, défense, initiative/cadence et progression. | — | — | — | — | I | R→J2-PROG-01.1…10 | — | — | — | — |
| J2-PROG-01.1 | J2 · Règles | Implémenter six caractéristiques, modificateurs raciaux, onze kits, robustesse, magie et seize compétences. | — | — | — | — | — | I+L | — | — | — | — |
| J2-PROG-01.2 | J2 · Règles | Implémenter niveaux 1–40, XP, bonus de niveau, compétences, PV initiaux/par niveau et PM. | — | — | — | — | — | I+L | — | — | — | — |
| J2-PROG-01.3 | J2 · Règles | Calculer initiative, esquive, armure, défenses, poison, attaque/DD magique, concentration, charge et déplacement. | — | — | — | — | — | I+L | — | — | — | — |
| J2-PROG-01.4 | J2 · Règles | Conserver les PV maximum historiques malgré les changements ultérieurs de caractéristique ou kit. | — | — | — | — | — | I+L | — | — | — | — |
| J2-PROG-01.5 | J2 · Règles | Normaliser les données et tester formules, profils, affiliations et restrictions de contrôle des PNJ. | — | — | — | — | — | I+L | — | — | — | — |
| J2-PROG-01.6 | J2 · Règles | Choisir seuils statiques, jets au d20 ou combinaison pour les défenses et unifier la terminologie. | — | — | — | — | — | I | — | — | — | — |
| J2-PROG-01.7 | J2 · Règles | Définir dégâts, résistances, vulnérabilités, immunités et ordre de réduction. | — | — | — | — | — | I | — | — | — | — |
| J2-PROG-01.8 | J2 · Règles | Ajouter bonus d'équipement, situation, effet et maîtrise sans couplage au rendu ou à ImGui. | — | — | — | — | — | I | — | — | — | — |
| J2-PROG-01.9 | J2 · Règles | Implémenter les traits raciaux dépendant du combat, de la perception, du déplacement forcé ou des dégâts. | — | — | — | — | — | I | — | — | — | — |
| J2-PROG-01.10 | J2 · Règles | Définir cadence, jets opposés, critiques, état à 0 PV, soins, repos et récupération hors combat. | — | — | — | — | — | I | — | — | — | — |
| J2-PROG-02 | J2 · Inspecteur | Rendre les statistiques des PJ et PNJ accessibles dans l'éditeur ImGui. | — | — | — | — | — | R→J2-PROG-02.1…03, L | — | — | — | — |
| J2-PROG-02.1 | J2 · Inspecteur | Éditer profil, niveau/XP, rayon, caractéristiques, compétences, PV et PM avec annulation/rétablissement. | — | — | — | — | — | I+L | — | — | — | — |
| J2-PROG-02.2 | J2 · Inspecteur | Afficher valeurs effectives, modificateurs et valeurs dérivées en lecture seule avec IDs ImGui stables. | — | — | — | — | — | I+L | — | — | — | — |
| J2-PROG-02.3 | J2 · Inspecteur | Ajouter explications des formules, provenance des bonus, alertes et recherche/filtrage des compétences. | — | — | — | — | — | I | — | — | — | — |
| J2-PROG-03 | J2 · Création | Créer un personnage : origine, apparence, voix, archétype, choix initiaux, validation et récapitulatif. | — | — | — | — | I | — | — | — | — | — |
| J2-PROG-04 | J2 · Progression | Implémenter expérience, niveaux, choix de capacités, spécialisations, améliorations permanentes et éventuel respec. | — | — | — | — | I | R→J2-PROG-04.1…03 | — | — | — | — |
| J2-PROG-04.1 | J2 · Progression | Déduire le niveau de l'XP et positionner l'XP au seuil exact d'un niveau depuis l'éditeur. | — | — | — | — | — | I+L | — | — | — | — |
| J2-PROG-04.2 | J2 · Progression | Appliquer et mémoriser les gains historiques de PV, maîtrise, talents, caractéristiques et compétences. | — | — | — | — | — | I | — | — | — | — |
| J2-PROG-04.3 | J2 · Progression | Gérer choix différés, validation, annulation et migration des tables de progression. | — | — | — | — | — | I | — | — | — | — |
| J2-PROG-05 | J2 · Effets | Gérer blessures, maladies, malédictions, bonus, pénalités, durées, empilement, dissipation et affichage. | — | — | — | — | I | — | — | — | — | — |
| J2-PROG-06 | J2 · Équilibrage | Piloter progression et équilibrage par données et fournir des outils de simulation de combat. | — | — | — | — | I | — | — | — | — | — |
| J2-MAG-01 | J2 · Capacités | Définir capacités actives/passives : coût, prérequis, cible, portée, lancement, interruption, recharge/charges et effets. | — | — | — | — | I | — | — | — | — | — |
| J2-MAG-02 | J2 · Magie | Gérer ciblage, sauvegardes, projectiles, invocations, contrôle, soins, buffs, debuffs et dissipation. | — | — | — | — | I | — | — | — | — | — |
| J2-MAG-03 | J2 · Magie | Gérer ressources, repos, éventuelle préparation et grimoire/liste de capacités. | — | — | — | — | I | — | — | — | — | — |
| J2-MAG-04 | J2 · IA | Utiliser les capacités, éviter les zones dangereuses et prévenir le tir allié selon le comportement. | — | — | — | — | I | — | — | — | — | — |
| J2-ECO-01 | J2 · Objets | Définir piles, éventuelle charge, rareté, identification, charges, consommables, équipements et objets de quête. | — | — | — | — | I | — | — | — | — | — |
| J2-ECO-02 | J2 · Équipement | Gérer slots, incompatibilités, statistiques, comparaison, raccourcis et apparence/paper doll. | — | — | — | — | I | — | — | — | — | — |
| J2-ECO-03 | J2 · Commerce | Gérer inventaires marchands, prix, achat/vente, éventuel vol, monnaie, réapprovisionnement et anti-duplication. | — | — | — | — | I | — | — | — | — | — |
| J2-ECO-04 | J2 · Artisanat | N'implémenter fabrication/enchâssement que s'ils servent l'univers original. | — | — | — | — | I | — | — | — | — | — |
| J2-EXIT | J2 · Validation | Permettre à un personnage créé de progresser, s'équiper et employer des capacités cohérentes dans un combat complet. | — | — | — | — | I | — | — | — | — | — |
| J3-ZON-01 | J3 · Zones | Créer extérieurs/intérieurs, instances, transitions, portes, clés, pièges, secrets, conteneurs et interactions. | — | — | — | — | I | — | — | — | — | — |
| J3-ZON-02 | J3 · Voyage | Ajouter cartes locale/monde, découverte, voyage, rencontres, repos et temps. | — | — | — | — | I | — | — | — | — | — |
| J3-ZON-03 | J3 · Exploration | Gérer brouillard de guerre, visibilité, exploration persistée, marqueurs, annotations et éventuel voyage rapide. | — | — | — | — | I | — | — | — | — | — |
| J3-ZON-04 | J3 · Persistance | Persister PNJ, portes/coffres, objets, déclencheurs et conséquences de quêtes. | — | — | — | — | I | — | — | — | — | — |
| J3-ZON-05 | J3 · Scripts | Créer des événements sûrs et déterministes : cinématiques, embuscades, renforts, transitions, dialogues forcés et conditions. | — | — | — | — | I | — | — | — | — | — |
| J3-NAR-01 | J3 · Dialogue | Étendre conditions, tests, conséquences, interruption de combat et voix des dialogues. | — | — | — | — | I | — | — | — | — | — |
| J3-NAR-02 | J3 · Quêtes | Créer journal, objectifs, historique, embranchements, échecs, récompenses et progression sans spoilers excessifs. | — | — | — | — | I | — | — | — | — | — |
| J3-NAR-03 | J3 · Factions | Gérer factions, réputation, dispositions et réactions visibles lorsque pertinent. | — | — | — | — | I | — | — | — | — | — |
| J3-NAR-04 | J3 · Compagnons | Gérer recrutement, départ, conversations, approbation, équipement/IA, conflits et arcs originaux. | — | — | — | — | I | — | — | — | — | — |
| J3-NAR-05 | J3 · Compagnons | Gérer temporaires, invités et indisponibles sans corrompre groupe ou quêtes. | — | — | — | — | I | — | — | — | — | — |
| J3-CMP-01 | J3 · Campagne | Écrire bible d'univers, chronologie, factions, géographie, bestiaire, panthéon et terminologie d'AlKanzar. | — | — | — | — | I | — | — | — | — | — |
| J3-CMP-02 | J3 · Campagne | Produire arc principal original, fins et quêtes secondaires avec dépendances et états irréversibles cartographiés. | — | — | — | — | I | — | — | — | — | — |
| J3-CMP-03 | J3 · Campagne | Concevoir difficulté, butin et rencontres par chapitre/zone. | — | — | — | — | I | — | — | — | — | — |
| J3-CMP-04 | J3 · Campagne | Produire PNJ, créatures, boss, objets, environnements et cinématiques originaux. | — | — | — | — | I | — | — | — | — | — |
| J3-EXIT | J3 · Validation | Rendre la campagne jouable du début à une fin, avec embranchements durables, compagnons, exploration et rejouabilité. | — | — | — | — | I | — | — | — | — | — |
| J4-AV-01 | J4 · Direction artistique | Établir bible visuelle, silhouettes, palette, matériaux, échelle, éclairage, interface et lisibilité isométrique. | — | — | — | — | I | — | — | — | — | — |
| J4-AV-02 | J4 · Assets | Produire modèles, rigs, animations, textures, décors, accessoires, portraits, icônes, VFX et terrains originaux. | — | — | — | — | I | — | — | — | — | — |
| J4-AV-03 | J4 · Audio | Ajouter ambiances, musique adaptative, SFX combat/UI, mixage, sous-titres, voix et priorités sonores. | — | — | — | — | I | — | — | — | — | — |
| J4-AV-04 | J4 · Pipeline | Gérer import, compression, budgets mémoire, qualité et licences des ressources externes autorisées. | — | — | — | — | I | — | — | — | — | — |
| J4-UX-01 | J4 · UI | Finaliser création, inventaire, fiche, capacités, grimoire, journal, carte, réputation, marchands, repos, options et menus. | — | — | — | — | I | — | — | — | — | — |
| J4-UX-02 | J4 · UI | Ajouter infobulles, comparaison, journal filtrable, recherches et raccourcis cohérents. | — | — | — | — | I | — | — | — | — | — |
| J4-UX-03 | J4 · Accessibilité | Gérer texte, contraste, sous-titres, remappage, clavier/manette, réduction des flashes/secousses et options audio. | — | — | — | — | I | — | — | — | — | — |
| J4-UX-04 | J4 · Localisation | Prévoir pluriels, genres, Unicode, débordements et langues longues dans les données source. | — | — | — | — | I | — | — | — | — | — |
| J5-01 | J5 · Tests | Tester unitairement jets, dégâts, résistances, capacités, inventaires, progression, quêtes, factions et scripts. | — | — | — | — | I | — | — | — | — | — |
| J5-02 | J5 · Intégration | Tester groupe de six, formation, pause/ordres, combat, dialogue, quête, transition et sauvegarde/chargement. | — | — | — | — | I | — | — | — | — | — |
| J5-03 | J5 · Sauvegarde | Tester migrations, compatibilité, corruption, références absentes et nettoyage des entités de zone. | — | — | — | — | I | — | — | — | — | — |
| J5-04 | J5 · Performance | Tester combats chargés, zones denses, chargements, mémoire, fuites et longues sessions. | — | — | — | — | I | — | — | — | — | — |
| J5-05 | J5 · Playtests | Tester onboarding, lisibilité, difficulté, compréhension des quêtes, accessibilité et blocages. | — | — | — | — | I | — | — | — | — | — |
| J5-06 | J5 · Diagnostic | Ajouter console, logs structurés, capture de reproduction, inspecteur règles/états et vérificateur de sauvegardes. | — | — | — | — | I | — | — | — | — | — |
| J5-07 | J5 · Livraison | Produire installateurs, notes, crédits/licences, politique de données et support ; tester une installation propre. | — | — | — | — | I | — | — | — | — | — |
| J5-DONE | J5 · Validation | Livrer une campagne finissable sans bloqueur connu, validée automatiquement, avec sauvegardes fiables, budgets tenus et plateformes testées. | — | — | — | — | I | — | — | — | — | — |
