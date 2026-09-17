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
`V1NAV\0\0\0\0\0`. Une scène texte courante V2 commence par `V2SCN-----` ;
le lecteur accepte encore `V1SCN-----` pour la migration.

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
| `SCN` | Scène Lua déclarative | `V2` |

Le parseur NAV sait encore lire l'ancien préambule texte `version 1` afin de
permettre une migration progressive. Toute nouvelle sérialisation utilise
l'en-tête commun de 10 octets.

## Scènes SCN V1/V2

Le payload d'une scène est du Lua lisible, limité à une API déclarative. La
scène par défaut se trouve dans `assets/scenes/DefaultScene.scene` et suit le
flux suivant :

```lua
scene = Create({ type = "Scene", navmesh = "navmeshes/DefaultScene.navmesh" })
ground = Create({
    type = "Primitive",
    id = "ground",
    name = "Ground",
    shape = "Plane",
    material = "Soil",
    layer = "Ground",
})
ground.transform({ scale = { x = 1000, y = 1, z = 1000 } })
scene.add(ground)
player = Create({
    type = "Model",
    id = "player",
    name = "Player",
    asset = "Adventurer.glb",
})
player.transform({ position = { x = 0, y = 0, z = 0 } })
player.character({
    affiliation = "Player",
    controller = "Player",
    party_slot = 0,
    -- race, kit, abilities, skills and vitals omitted here
})
player.combatant({
    weapon_mode = "Melee",
    state = "Idle",
    script_phase = 0,
    animations = {
        melee_ready = "Sword Ready",
        melee_attack = "Sword Attack",
        downed = "Downed",
        dead = "Death",
    },
})
scene.add(player)
sun = Create({
    type = "DirectionalLight",
    id = "sun",
    name = "Sun",
    direction = { x = -0.35, y = -1, z = -0.25 },
    color = { x = 1, y = 0.93, z = 0.82 },
    intensity = 3,
})
scene.add(sun)
scene.build()
```

`scene.build()` est obligatoire et termine la construction. Tous les objets
créés doivent être passés une seule fois à `scene.add`. Le chargeur transforme
ensuite les tables validées en `SceneBlueprint`; le Lua ne manipule jamais le
monde ECS ou le renderer.

SCN V2 exige un `id` stable et unique pour chaque objet auteur. Un objet doté
d'un transform peut déclarer `child.parent(parent)` ; la cible doit elle-même
être transformable et la hiérarchie ne peut contenir ni référence absente, ni
auto-parentage, ni cycle. La lecture V1 attribue des IDs déterministes pour
conserver la compatibilité, mais `SceneDocument` garde le document sale jusqu'à
son premier enregistrement en V2. Toute nouvelle sérialisation est canonique,
locale-indépendante, revalidée avant écriture et remplacée atomiquement.

`SceneDocument` possède le blueprint auteur, ses chemins source/staging, son
état sale et les liaisons temporaires entre IDs SCN et entités ECS. Seules les
racines portant `AuthoredSceneObjectComponent` sont éditables et persistées ;
les enfants générés par l'import glTF restent visibles mais en lecture seule.
Les commandes structurelles restaurent un snapshot auteur puis reconstruisent
le monde et la navigation, tandis que les inspecteurs capturent les propriétés
SCN prises en charge. Créer, dupliquer, supprimer et reparent-er sont annulables.
Le reparentage conserve le transform monde et refuse une décomposition ambiguë
due au cisaillement.

La composition visible est intégralement déclarée par les objets SCN. Les
primitives `Plane` et `Box` choisissent explicitement leur preset de matériau
(`Soil`, `Rock` ou `Wood`), leur couche de rendu et leur transform ; leur
échelle constitue leurs dimensions. `SceneFactory` ne crée aucun sol, mur ou
objet de test implicite. Le profil de matériau spécial d'un modèle est lui
aussi un champ auteur (`material_profile`) et ne dépend pas de son nom. Les
anciens champs globaux de sol/murs de V1, ainsi que les premiers fichiers V2
qui les contenaient encore, sont matérialisés en cinq primitives et marqués
pour migration lors du prochain enregistrement.

Le picking éditeur remonte toute section de rendu importée jusqu'à la première
racine portant `AuthoredSceneObjectComponent`. La racine du modèle reste donc
l'objet sélectionné et sauvegardé, tandis que ses sous-maillages glTF restent
des détails techniques consultables en lecture seule dans la hiérarchie.

Une scène peut déclarer au plus un `DirectionalLight`. Sa direction non nulle,
sa couleur et son intensité positives ou nulles sont validées au chargement,
puis la direction est normalisée dans le blueprint. `SceneFactory` crée le
composant ECS technique ; `RenderExtractionSystem` le copie dans l'unique
`FrameDirectionalLight` du snapshot et les rendus direct et différé consomment
ce snapshot. La lumière directionnelle ne rejoint pas les listes de lumières
locales ou leurs volumes. Son descripteur éditeur applique les mêmes invariants
et publie une modification de lumière pour chaque édition annulable.

`CameraMatrices` transporte aussi les plans near/far ayant servi à construire
la projection. Les cascades d'ombre directionnelles utilisent ces valeurs et
bornent défensivement le near plane au-dessus de zéro : un near nul rendrait le
découpage logarithmique non fini et annulerait l'éclairage solaire différé.

Le runtime Lua est volontairement restreint : chargement de chunks texte
uniquement, aucune bibliothèque standard IO/OS/package/debug, plafond mémoire
et budget d'instructions. Les champs inconnus, types incorrects, valeurs non
finies, enums inconnus, mauvais en-tête et version non supportée produisent une
erreur exploitable. Les références de ressources doivent être des chemins
relatifs portables sans remontée `..`. Lua 5.5.0 est récupéré depuis le dépôt
officiel et épinglé par CMake.

## Socle de combat

La capacité à combattre est explicite et indépendante de l'affiliation : la
présence optionnelle de `CombatantComponent` sur une racine de personnage est
le seul critère. `CombatData.hpp` porte les modes d'arme et noms de clips
configurables ; `CombatComponents.hpp` porte l'état runtime, la phase de script
et le temps passé dans l'état. Une scène SCN V2 peut persister ces données par
`model.combatant({...})`, uniquement après avoir déclaré `model.character`.

`CombatSystem` s'exécute au pas fixe après la navigation et avant
`AnimationSystem`. Il observe les transitions entre `Idle`, `Combat`,
`Attacking`, `HitReaction`, `Fleeing`, `Scripted`, `Downed` et `Dead`, bloque
le mouvement pour les états qui interdisent une action concurrente et choisit
le clip demandé selon le mode `Unarmed`, `Melee` ou `Ranged`. Une arrivée à
zéro PV place un combattant à terre et annule sa destination ; un personnage
sans ce composant reste hors de cette mécanique.

`Attacking` et `HitReaction` sont des nœuds transitoires non bouclés. Leur
entrée mémorise le dernier état reprenable (`Idle`, `Combat`, `Fleeing` ou
`Scripted`) ; la fin effective du clip revient vers ce nœud, tandis que zéro
PV dirige immédiatement vers `Downed`. Pour éviter une oscillation pendant le
fondu, la navigation ne demande aucune animation de locomotion lorsqu'un état
verrouille le mouvement. L'arbitre combat annule aussi toute requête moins
prioritaire déjà présente et restaure la politique de boucle antérieure une
fois la transition stabilisée.

Ce socle expose des entrées déterministes aux futurs scripts et IA, notamment
`scriptPhase` pour une fuite ou une phase de boss. Il ne contient encore ni
sélection autonome de cible, ni jets d'attaque, ni dégâts, ni transition
automatique entre toutes les phases. Ces décisions devront alimenter le
composant ou des commandes de simulation sans être placées dans l'éditeur ou
le rendu.

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
`CharacterControllerComponent` décrit qui peut donner des ordres et
`PartyMemberComponent` décrit l'appartenance active ainsi que l'emplacement
stable dans un groupe limité à six. Ces informations sont indépendantes de
`CharacterAffiliation` : un compagnon à affiliation amicale peut donc être
contrôlé et afficher un anneau vert.

`PartySelectionModel` conserve la sélection runtime ordonnée et son premier
membre comme leader. En Gameplay, `PartySelectionSystem` projette les limites
des membres actifs contrôlés par le joueur pour résoudre le cadre de sélection
et les trie par emplacement de groupe ; un cadre vide vide la sélection. À
chaque frame de présentation, il assombrit leur `FrameGroundIndicator` vert
lorsqu'ils ne sont pas sélectionnés, y compris quand la simulation est en
pause. Le rectangle vert est transmis au rendu par
`FramePartySelectionMarquee`, sans requête du renderer vers l'ECS. Seule la
sélection éditeur peut alimenter l'outline et les gizmos de l'éditeur.

En mode Editor, ImGuizmo manipule uniquement le transform d'une racine SCN
sélectionnée. Translation, rotation et échelle acceptent les espaces local ou
monde et le snapping ; l'échelle reste locale. Une interaction continue met à
jour la présentation immédiatement mais ne crée qu'une commande undo à son
relâchement. La rotation monde est désactivée sous un parent à échelle non
uniforme, car la conversion monde-vers-local pourrait introduire du
cisaillement que `TransformComponent` ne sait pas représenter.

Un clic de déplacement est transformé par `PartyOrderSystem` en destinations
de formation distinctes, centrées sur le point demandé et espacées selon les
rayons des personnages. `NavigationSystem::projectAgentDestination` valide
chaque destination sur le snapshot de navmesh avant de lancer une requête
asynchrone par agent. Une nouvelle requête remplace et annule l'ancienne pour
le même agent. Les ordres peuvent être saisis pendant la pause ; le mouvement
reste toutefois exclusivement exécuté par les mises à jour à pas fixe après
la reprise.

Lorsqu'un membre devient actif et contrôlé par le joueur,
`NavigationSystem::syncCharacterAgentControl` lui ajoute, s'ils sont absents,
un rigidbody dynamique sans gravité et un box collider de dimensions complètes
`0.44 × 2.0 × 0.44`, centré à mi-hauteur et aligné aux axes du monde. Le
nouvel agent utilise ce collider comme source de dégagement pour ses requêtes
de chemin. Une nouvelle synchronisation conserve les composants et réglages
déjà présents. Les autres box colliders continuent par défaut de suivre la
rotation de leur entité ; `BoxColliderComponent::rotatesWithEntity` rend ce
choix explicite et éditable.

Pour un agent doté d'un rigidbody dynamique, la navigation ne translate plus
directement son transform. `NavigationAgentMotion` calcule une
`desiredVelocity`, corrigée par un évitement local déterministe à partir d'un
instantané des corps voisins. `PhysicsSystem` consomme ensuite cette intention,
intègre le mouvement et résout les contacts en supprimant la vitesse normale
tout en conservant le glissement tangentiel. Les agents dépourvus de corps
physique gardent leur déplacement direct historique.

Le cap visible ne saute pas directement vers chaque nouveau segment ou chaque
correction d'évitement. Il interpole le chemin angulaire le plus court à pas
fixe, sous la limite de `turnSpeedDeg`. La vitesse de translation est pondérée
par l'alignement courant : un agent tourne presque sur place lors d'un virage
fort, puis retrouve progressivement sa vitesse. La récupération valide le box
collider avec la politique d'orientation du composant. Le collider joueur par
défaut ne reçoit donc pas le yaw visuel : sa boîte physique, son empreinte de
dégagement navmesh et sa matrice de debug restent toutes alignées au monde.
Un collider rotatif conserve le comportement orienté et emploie le cap
réellement appliqué, jamais la direction cible encore à atteindre.

Le rayon d'arrivée ne suffit pas à consommer un point tangent intermédiaire.
Pour un agent mû par la physique, le sweep de son empreinte complète depuis la
position courante jusqu'au point suivant doit également rester dans la surface
marchable. `NavigationAgentMotion` et le trimming post-physique partagent ce
même prédicat ; l'agent rejoint donc réellement le coin avant de tourner au
lieu de couper brièvement l'obstacle puis de déclencher un recalcul.

Après la physique, `NavigationAgentRecovery` vérifie le résultat contre le
snapshot de navmesh. Une correction qui sortirait le collider de la surface
est annulée vers la pose complète précédant le pas : position et orientation
sont restaurées ensemble. Les traversées de liens restent autorisées hors des
surfaces planes. Si une collision au coin d'un obstacle rend le chemin
inexécutable, l'agent s'arrête et demande un nouveau calcul asynchrone vers sa
destination. Le recalcul conserve exactement une empreinte déjà alignée au
monde ; une empreinte rotative est convertie en dégagement conservateur
indépendant de l'orientation. Les échecs transitoires sont réessayés avec
temporisation croissante et un nombre maximal de tentatives afin d'éviter une
boucle de requêtes permanente.
