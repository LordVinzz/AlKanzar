# Architecture en couches

Le code de gameplay suit un flux à sens unique :

`données de contenu -> règles -> simulation -> présentation/UI`

La flèche décrit à la fois le flux des données et les dépendances autorisées.
Une couche peut connaître les couches situées à sa gauche, jamais celles qui
sont à sa droite.

## Responsabilités

| Couche | Responsabilité | Emplacements principaux |
| --- | --- | --- |
| Données de contenu | Types sérialisables, identifiants, valeurs configurées et primitives de format pures. Aucun comportement moteur. | `src/core/content` |
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

## En-têtes des fichiers de contenu

Les fichiers de contenu versionnés commencent par un en-tête de taille fixe de
10 octets défini dans `core/content/ContentFileHeader.hpp`. Il contient le jeton
ASCII `V<version décimale><TYPE>` puis un remplissage jusqu'à atteindre
exactement 10 octets. Un navmesh binaire V1 commence par
`V1NAV\0\0\0\0\0`. Une scène texte V1 commence par `V1SCN-----`.

- La version est strictement positive et peut comporter plusieurs chiffres.
- Le type ne contient que des lettres ASCII majuscules.
- Un format binaire remplit les octets inutilisés avec `\0` ; un format texte
  les remplit avec `-` pour rester lisible dans un éditeur. Les deux formes
  sont validées strictement et ne peuvent pas être mélangées.
- Le lecteur valide le type avant de déléguer au parseur du contenu.
- L'en-tête porte la version du format : le payload ne doit pas la dupliquer.
- Les fichiers sont lus en mode binaire afin de ne jamais transformer leurs
  dix premiers octets, même lorsque leur payload est textuel.

Types attribués actuellement :

| Type | Contenu | Version courante |
| --- | --- | --- |
| `NAV` | Navmesh | `V1` |
| `SCN` | Scène Lua déclarative | `V1` |

Le parseur NAV sait encore lire l'ancien préambule texte `version 1` afin de
permettre une migration progressive. Toute nouvelle sérialisation utilise
l'en-tête commun de 10 octets.

## Scènes SCN V1

Le payload d'une scène est du Lua lisible, limité à une API déclarative. La
scène par défaut se trouve dans `assets/scenes/DefaultScene.scene` et suit le
flux suivant :

```lua
scene = Create({ type = "Scene", navmesh = "navmeshes/DefaultScene.navmesh" })
player = Create({ type = "Model", name = "Player", asset = "Adventurer.glb" })
player.transform({ position = { x = 0, y = 0, z = 0 } })
scene.add(player)
scene.build()
```

`scene.build()` est obligatoire et termine la construction. Tous les objets
créés doivent être passés une seule fois à `scene.add`. Le chargeur transforme
ensuite les tables validées en `SceneBlueprint`; le Lua ne manipule jamais le
monde ECS ou le renderer.

Le runtime Lua est volontairement restreint : chargement de chunks texte
uniquement, aucune bibliothèque standard IO/OS/package/debug, plafond mémoire
et budget d'instructions. Les champs inconnus, types incorrects, valeurs non
finies, enums inconnus, mauvais en-tête et version non supportée produisent une
erreur exploitable. Les références de ressources doivent être des chemins
relatifs portables sans remontée `..`. Lua 5.5.0 est récupéré depuis le dépôt
officiel et épinglé par CMake.

## Modes d'exécution

`core/app/AppMode.hpp` définit les capacités de `Gameplay`, `Editor` et
`TestTool`. La boucle principale consulte ces capacités pour décider quels
systèmes à pas fixe, entrées, outils et overlays sont autorisés ; elle ne doit
pas disperser de tests directs du mode dans les sous-systèmes.

- `Gameplay` accepte les ordres du groupe et utilise la sélection runtime.
- `Editor` expose ImGui, la sélection d'entité/composant et les outils de
  navigation.
- `TestTool` exécute les systèmes runtime sur une scène déterministe, sans UI
  éditeur, sélection d'édition ni ordre de gameplay.

La sélection éditeur et la sélection du groupe sont deux états indépendants.
`PartySelectionModel` conserve la sélection runtime ordonnée et son premier
membre comme leader. En Gameplay, `PartySelectionSystem` projette les limites
des personnages `Player` pour résoudre le cadre de sélection ; un cadre vide
vide la sélection. À chaque frame de présentation, il assombrit également les
`FrameGroundIndicator` des personnages `Player` non sélectionnés, y compris
quand la simulation est en pause. Le rectangle vert est transmis au rendu par
`FramePartySelectionMarquee`, sans requête du renderer vers l'ECS. Seule la
sélection éditeur peut alimenter l'outline et les gizmos de l'éditeur ; les
ordres de jeu ciblent le leader de la sélection runtime.
