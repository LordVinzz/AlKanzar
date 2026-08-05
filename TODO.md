# AlKanzar — feuille de route CRPG isométrique

## Vision et garde-fous

Créer un CRPG solo en 3D stylisée, à caméra isométrique fixe, inspiré de la
richesse systémique et du rythme « temps réel avec pause » des grands CRPG
classiques. Le but est un jeu complet : campagne, groupe, exploration, combat,
dialogues et progression.

Le jeu doit être **entièrement original** : univers, règles nommées,
personnages, lieux, créatures, textes, quêtes, musiques et assets. Baldur's
Gate I & II servent de référence d'expérience, pas de contenu à reproduire.

### Conventions

- `[x]` : fondation déjà présente dans le dépôt.
- `[ ]` : travail à réaliser.
- Les jalons sont dans l'ordre de dépendance ; un jalon doit être jouable et
  testable avant d'étendre le suivant.

### Fondations déjà disponibles

- `[x]` Application C++20, SDL2, OpenGL, CMake et journalisation.
- `[x]` ECS, transformations, scène, éclairage, picking et extraction de rendu.
- `[x]` Rendu direct/différé, ombres, matériaux, import glTF et textures.
- `[x]` Navigation/navmesh, recherche de chemin et évitement d'agents.
- `[x]` Systèmes initiaux d'animation, physique, tâches, profilage et éditeur.
- `[x]` Tests unitaires du moteur et contrôle de qualité des sources.

---

## Jalon 0 — Fondations de l'application de jeu

### Architecture et exécution

- [ ] Séparer explicitement les modes **éditeur**, **jeu** et **outil de test**.
- [x] Ajouter une boucle de simulation à pas fixe, distincte du rendu, avec
  pause (`Espace`), accélération (`+`/`-`) et gestion sûre du delta temps.
- [ ] Définir les couches `données de contenu -> règles -> simulation ->
  présentation/UI`, sans dépendance de règles vers le rendu.
- [ ] Créer un gestionnaire de ressources asynchrone : chargement, cache,
  références, erreurs, rechargement en développement et écran de chargement.
- [ ] Ajouter configuration utilisateur, profils, raccourcis reconfigurables,
  gestion manette/clavier/souris et gestion des résolutions/fenêtre.
- [ ] Définir un format de données versionné pour les contenus et les
  sauvegardes ; fournir validation et messages d'erreur exploitables.
- [ ] Mettre en place les builds debug/release, symboles, rapports de crash et
  une CI qui compile et exécute les tests sur les plateformes ciblées.

### Présentation isométrique

- [ ] Implémenter une caméra isométrique fixe : zoom borné, panoramique,
  rotation facultative par incréments et suivi doux du groupe.
- [ ] Ajouter culling, LOD, batching/instancing et budgets de rendu adaptés aux
  zones remplies de personnages et de décor.
- [ ] Gérer la transparence/masquage des éléments qui occultent les personnages,
  les toits escamotables et les volumes d'intérieur.
- [ ] Créer le pipeline d'éclairage de jeu : cycle jour/nuit si retenu,
  lumières locales, brouillard, particules et VFX lisibles en vue isométrique.
- [ ] Ajouter une chaîne d'animation de personnages : états locomotion/combat,
  blend trees, événements d'animation, armes et points d'attache.

### Outils et données

- [ ] Définir des formats source et de livraison pour modèles, textures,
  animations, audio, niveaux et données de jeu.
- [ ] Créer des validateurs de contenu : références cassées, IDs dupliqués,
  données invalides, zones non atteignables et dialogues/quêtes non terminables.
- [ ] Étendre l'éditeur avec placement d'entités, volumes, points d'intérêt,
  spawns, portes, déclencheurs, caméras et prévisualisation en mode jeu.
- [ ] Créer les outils d'auteur pour dialogues, quêtes, rencontres, tables de
  butin et scripts d'événements ; exporter des données versionnées.

**Critère de sortie :** une zone de démonstration charge depuis des données,
est navigable avec la caméra de jeu et se comporte de façon identique en build
debug et release.

---

## Jalon 1 — Vertical slice jouable

### Groupe, sélection et déplacement

- [ ] Créer les composants runtime de personnage : identité, équipe, position,
  état, cible, contrôleur, statistiques, inventaire et rendu.
    - [x] Ajouter un profil ECS commun aux PJ et PNJ : affiliation, race, kit,
      expérience et rayon de l'indicateur au sol.
    - [x] Séparer les caractéristiques, rangs de compétence et ressources
      PV/PM dans des composants dédiés avec cycle de vie ECS cohérent.
    - [x] Ajouter trois profils de démonstration : joueur, PNJ amical et PNJ
      hostile, sans contrôleur IA pour les PNJ.
    - [x] Afficher un anneau vert pour le joueur, bleu pour un PNJ amical et
      rouge pour un PNJ hostile dans les rendus direct et différé.
    - [ ] Séparer l'affiliation visuelle de l'équipe de combat, de la faction,
      de la réputation et de la disposition individuelle.
    - [ ] Ajouter les composants d'état runtime : vivant/à terre/mort, cible,
      contrôleur, ordre courant, inventaire, équipement et effets actifs.
    - [ ] Définir des identifiants persistants et la sérialisation des
      personnages pour les scènes, sauvegardes et changements de zone.
- [ ] Sélectionner un ou plusieurs personnages par clic, cadre de sélection,
  portraits UI et raccourcis de sélection du groupe.
    - [x] Rediriger le picking d'une sous-section glTF vers la racine ECS du
      personnage afin d'ouvrir la bonne fiche dans l'éditeur.
    - [ ] Distinguer la sélection d'édition de la sélection de groupe en jeu.
    - [ ] Ajouter sélection multiple, cadre, ajout/retrait avec modificateur,
      chef de sélection et limite de taille du groupe.
    - [ ] Ajouter portraits, raccourcis numériques, retour caméra et retours
      visuels/sonores de sélection.
- [ ] Donner des ordres par clic : déplacement, interaction, attaque, maintien
  de position et annulation ; afficher chemin, destination et curseur contextuel.
- [ ] Implémenter formations, rattrapage des retardataires, anti-blocage,
  répartition autour des cibles et traversée de portes/étroits passages.
- [ ] Faire respecter les zones interdites, obstacles dynamiques, hauteur et
  transitions de navmesh par les entités de jeu.

### Combat temps réel avec pause

- [ ] Définir la cadence de combat : acquisition de cible, portée, ligne de vue,
  temps d'attaque, récupération, interruption et mort/inconscience.
- [ ] Implémenter pause active, ralenti éventuel et file d'ordres visible par
  personnage ; les ordres donnés en pause s'exécutent après reprise.
- [ ] Ajouter jets, modificateurs, défense, dégâts, critiques, résistances,
  immunités et journal de combat compréhensible.
- [ ] Créer comportements IA minimaux : neutre, allié, agressif, distance,
  fuite, priorité de cible et retour à la position initiale.
- [ ] Ajouter projectiles, zones d'effet, effets temporaires, mort, butin et
  nettoyage des combats.

### Interaction et interface minimale

- [ ] Ajouter raycast/picking de jeu, surbrillance, interactions contextuelles,
  portes, coffres, objets au sol, déclencheurs et points de transition.
- [ ] Créer le HUD : portraits, points de vie, états, sélection, barre d'actions,
  pause/vitesse, notifications et journal de combat.
- [ ] Implémenter une fenêtre d'inventaire simple, équipement, transfert de
  butin et utilisation d'objet.
- [ ] Implémenter un dialogue à embranchements avec conditions et conséquences.
- [ ] Implémenter une quête courte : déclenchement, objectifs, progression,
  récompense, échec et journal.
- [ ] Ajouter sauvegarde/chargement manuel du monde, du groupe, des objets, de
  la quête et de la zone courante.

**Critère de sortie :** un groupe peut explorer une zone, combattre, parler à
un PNJ, terminer une quête, changer de zone et reprendre l'état exact après
chargement d'une sauvegarde.

---

## Jalon 2 — Système RPG complet

### Personnages et progression

- [ ] Concevoir un système de règles original : attributs, ressources,
  compétences, jets, difficulté, défense, initiative/cadence et progression.
    - [x] Implémenter les six caractéristiques, modificateurs raciaux, onze
      kits, robustesse, progression magique et seize compétences.
    - [x] Implémenter XP/niveaux 1 à 40, bonus de niveau, rangs de compétence,
      PV initiaux/par niveau et PM des lanceurs complets ou partiels.
    - [x] Calculer les valeurs dérivées : initiative, esquive, armure, défenses,
      résistance au poison, attaque/DD magique, concentration, charge et
      déplacement de base.
    - [x] Conserver les PV maximum historiques au lieu de les recalculer
      rétroactivement après un changement de caractéristique ou de kit.
    - [x] Ajouter normalisation des données et tests unitaires des formules,
      profils de scène, affiliations et restrictions de contrôle des PNJ.
    - [ ] Décider si les défenses sont des seuils statiques, des jets au d20 ou
      les deux selon l'effet, puis unifier les termes du document de règles.
    - [ ] Définir les types de dégâts, résistances en pourcentage,
      vulnérabilités, immunités et l'ordre exact de réduction.
    - [ ] Ajouter les bonus d'équipement, de situation, d'effet et de maîtrise
      sans coupler le noyau de règles au rendu ou à ImGui.
    - [ ] Implémenter les traits raciaux restants qui nécessitent un contexte de
      combat, de perception, de déplacement forcé ou de type de dégâts.
    - [ ] Définir les règles de cadence, jets opposés, critiques, états à
      0 PV, soins, repos et récupération hors combat.
- [x] Rendre les statistiques des PJ et PNJ accessibles dans l'éditeur ImGui.
    - [x] Éditer affiliation, race, kit, niveau/XP, rayon, caractéristiques,
      compétences, PV maximum/courants et PM avec annulation/rétablissement.
    - [x] Afficher séparément les valeurs effectives, modificateurs et valeurs
      dérivées en lecture seule avec des identifiants ImGui stables.
    - [ ] Ajouter explications de formule, provenance des bonus, avertissements
      de données incohérentes et recherche/filtrage des compétences.
- [ ] Ajouter création de personnage : origine, apparence, voix, archétype,
  choix de départ, validation et récapitulatif.
- [ ] Implémenter niveaux, expérience, montée de niveau, choix de capacités,
  spécialisations, améliorations permanentes et respec si prévu.
    - [x] Déduire le niveau de l'XP et permettre à l'éditeur de positionner l'XP
      au seuil exact d'un niveau choisi.
    - [ ] Ajouter un service de montée de niveau qui applique et mémorise les
      gains historiques de PV, maîtrise, talents, caractéristiques et
      compétences.
    - [ ] Définir les choix différés, validations, annulations et migrations
      lorsque les règles ou tables de progression changent.
- [ ] Ajouter effets persistants : blessures, maladies, malédictions, bonus,
  pénalités, durées, empilement, dissipation et affichage explicite.
- [ ] Concevoir une table de progression et un système d'équilibrage piloté par
  données, avec outils de simulation de combat.

### Capacités et magie

- [ ] Créer des capacités actives/passives à données : coût, prérequis, cible,
  portée, lancement, interruption, cooldown/charges et effets.
- [ ] Implémenter sorts ou pouvoirs : ciblage unitaire/sol/zone, sauvegardes,
  projectiles, invocations, contrôle, soins, buffs, debuffs et dissipation.
- [ ] Gérer les emplacements/ressources de pouvoir, repos, préparation éventuelle
  et grimoire/liste de capacités.
- [ ] Ajouter IA d'utilisation de capacités, zones dangereuses et prévention du
  tir allié lorsque le comportement le demande.

### Équipement et économie

- [ ] Définir objets, piles, poids/charge si retenu, rareté, identification,
  charges, consommables, armes, armures, accessoires et objets de quête.
- [ ] Implémenter slots d'équipement, incompatibilités, statistiques dérivées,
  comparaison d'objets, raccourcis rapides et apparence/paper doll.
- [ ] Ajouter marchands : inventaires, prix, vente, achat, vol si retenu,
  monnaie, réapprovisionnement et validation anti-duplication.
- [ ] Décider et implémenter seulement les systèmes de fabrication/enchâssement
  qui servent l'univers original ; ne pas les ajouter par défaut.

**Critère de sortie :** un personnage créé peut progresser, acquérir/équiper
des objets et utiliser des capacités cohérentes dans un combat complet.

---

## Jalon 3 — Monde, exploration et narration

### Zones et voyage

- [ ] Créer zones extérieures et intérieures, instances, entrées/sorties,
  portes verrouillées, clés, pièges, secrets, conteneurs et objets interactifs.
- [ ] Ajouter carte locale, carte du monde, destinations découvertes, voyage,
  rencontres aléatoires/scénarisées, repos et gestion du temps.
- [ ] Implémenter brouillard de guerre, visibilité, exploration enregistrée,
  marqueurs, annotations et points de voyage rapide si prévus.
- [ ] Ajouter état persistant du monde : PNJ déplacés/morts, portes/coffres,
  objets ramassés, déclencheurs consommés et conséquences de quêtes.
- [ ] Créer scripts événementiels sûrs et déterministes : cinématiques,
  embuscades, renforts, transitions, dialogues forcés et conditions de zone.

### Dialogue, quêtes et compagnons

- [ ] Étendre les dialogues : conditions de statistiques/objets/quête/faction,
  tests, lignes conditionnelles, conséquences, interruption de combat et voix.
- [ ] Créer journal de quêtes : objectifs, historique, embranchements, échecs,
  récompenses et indicateurs de progression sans spoilers excessifs.
- [ ] Implémenter factions, réputation, dispositions individuelles et réactions
  du monde ; rendre leurs règles visibles au joueur quand cela est pertinent.
- [ ] Ajouter compagnons : recrutement, départ, conversations, approbation,
  équipement/IA, conflits narratifs et arcs de quête originaux.
- [ ] Prévoir compagnons temporaires, invités et membres indisponibles sans
  corrompre l'état du groupe ou des quêtes.

### Contenu de campagne

- [ ] Écrire une bible d'univers, une chronologie, factions, géographie,
  bestiaire, panthéon et terminologie propres à AlKanzar.
- [ ] Produire l'arc principal original, ses fins multiples et les quêtes
  secondaires ; cartographier les dépendances et les états irréversibles.
- [ ] Concevoir la progression de difficulté, la répartition de butin et les
  rencontres de chaque chapitre/zone.
- [ ] Produire PNJ, créatures, boss, objets uniques, environnements et
  cinématiques qui soutiennent cette campagne, sans contenu sous licence.

**Critère de sortie :** une campagne complète peut être jouée du début à une
fin, avec embranchements durables, compagnie, exploration et rejouabilité.

---

## Jalon 4 — Direction artistique, audio et expérience utilisateur

### Production audiovisuelle

- [ ] Établir une bible visuelle : silhouette, palette, matériaux, échelle,
  éclairage, interface et lisibilité à distance isométrique.
- [ ] Produire modèles, rigging, animations, textures, décors modulaires,
  accessoires, portraits, icônes, VFX et effets de terrain originaux.
- [ ] Ajouter audio spatial : ambiances de zone, musique adaptative, SFX de
  combat/UI, mixage, sous-titres, voix et priorisation des sons.
- [ ] Mettre en place import, compression, budgets mémoire et contrôle qualité
  des assets ; documenter les licences de chaque ressource externe autorisée.

### Interface et accessibilité

- [ ] Finaliser écrans de création, inventaire, fiche personnage, capacités,
  grimoire, journal, carte, réputation, marchands, repos, options et menus.
- [ ] Ajouter infobulles exhaustives, comparaison de statistiques, journal de
  combat filtrable, recherches et raccourcis cohérents.
- [ ] Prévoir taille de texte, contraste, sous-titres, remappage, navigation
  clavier/manette, réduction des flashes/secousses et options audio séparées.
- [ ] Localiser textes et UI dès les données source : pluriels, genres,
  caractères Unicode, débordements et tests de langues longues.

---

## Jalon 5 — Fiabilité, qualité et livraison

- [ ] Couvrir les règles par tests unitaires : jets, dégâts, résistances,
  capacités, inventaires, progression, quêtes, factions et scripts.
- [ ] Ajouter tests d'intégration du vertical slice : groupe de six, formation,
  pause/file d'ordres, combat, dialogue conditionnel, quête, transition et
  sauvegarde/chargement.
- [ ] Tester migration et compatibilité des sauvegardes, chargements corrompus,
  références de contenu manquantes et nettoyage des entités de zone.
- [ ] Créer scènes/tests de performance pour combats chargés, zones denses,
  chargements, mémoire, fuites et stabilité sur longues sessions.
- [ ] Organiser playtests : onboarding, lisibilité des combats, difficulté,
  compréhension des quêtes, accessibilité et blocages de progression.
- [ ] Ajouter outils de diagnostic de développement : console, logs structurés,
  capture de repro, inspecteur de règles/états et vérificateur de sauvegardes.
- [ ] Produire installateurs, notes de version, crédits/licences, politique de
  données et procédures de support ; tester une installation propre.

**Définition de terminé :** campagne finissable sans bloqueur connu, contenus
validés automatiquement, sauvegardes fiables, performances dans les budgets
cibles et expérience testée sur les plateformes publiées.
