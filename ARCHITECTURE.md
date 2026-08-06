# Architecture en couches

Le code de gameplay suit un flux à sens unique :

`données de contenu -> règles -> simulation -> présentation/UI`

La flèche décrit à la fois le flux des données et les dépendances autorisées.
Une couche peut connaître les couches situées à sa gauche, jamais celles qui
sont à sa droite.

## Responsabilités

| Couche | Responsabilité | Emplacements principaux |
| --- | --- | --- |
| Données de contenu | Types sérialisables, identifiants et valeurs configurées par les auteurs. Aucun comportement moteur. | `src/core/content` |
| Règles | Calculs déterministes et validation du domaine. Aucun ECS, temps global, rendu, SDL ou ImGui. | `src/core/rules`, cible `alkanzar_rules` |
| Simulation | Composants runtime, adaptation des données vers les règles et mutation de l'état au pas fixe. | `src/core/simulation`, `src/core/ecs`, systèmes de gameplay |
| Présentation/UI | Extraction d'instantanés, rendu, audio et outils d'édition. Cette couche peut demander une mutation via une commande de simulation. | `src/render`, `src/core/editor`, `src/core/presentation`, `FrameData` et extraction de rendu |

## Contrats de dépendance

- `alkanzar_content` est une cible CMake d'en-têtes sans dépendance moteur.
- `alkanzar_rules` compile séparément et ne lie que `alkanzar_content`.
- Les fonctions de règles reçoivent leurs entrées explicitement et retournent
  une valeur ; elles ne lisent ni le monde ECS, ni l'horloge, ni le renderer.
- La simulation convertit ses composants en données de règles avec un
  adaptateur explicite, par exemple `characterRuleData`.
- La normalisation propre au domaine reste dans les règles. Les contraintes
  techniques de composant, comme le rayon d'un indicateur, restent dans la
  simulation.
- L'éditeur ImGui et le renderer consomment les résultats dérivés, mais aucun
  en-tête de règles ne peut inclure ou nommer leurs API.
- Le registre et les inspecteurs de composants appartiennent à `core/editor` ;
  l'ECS ne connaît que ses composants et le contrat léger `ComponentKind`.
- Les composants techniques de rendu présents dans l'ECS sont des adaptateurs
  de présentation ; ils ne sont jamais exposés aux règles de jeu.

## Ajouter une fonctionnalité

1. Ajouter les données configurables dans `core/content`.
2. Ajouter les calculs purs et leurs tests dans `core/rules`.
3. Ajouter les composants et systèmes mutables dans `core/simulation` ou dans
   un système ECS approprié.
4. Extraire uniquement les données nécessaires à `FrameData`, puis les afficher
   dans `render` ou `core/editor`.

Le test CTest `alkanzar_architecture_layers` analyse les sources de ces couches
et échoue dès qu'une dépendance interdite est introduite.
