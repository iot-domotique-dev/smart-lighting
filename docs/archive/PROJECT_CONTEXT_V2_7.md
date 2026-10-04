> Archive historique : contexte V2.7. L’état et les procédures actuels sont dans le [README du dépôt](../../README.md) et les [documents actifs](../README.md).

# Smart Lighting --- Contexte historique du projet

> Ce document décrit l'architecture avant V5. Les profils `lamp`, `lamp_a` et `lamp_b` sont retirés; la lampe actuelle est `lamp_c6`. Voir `README.md` pour l'état courant.

## 1. Objectif

Projet de système d'éclairage intelligent et modulaire basé sur ESP32.

Le système doit pouvoir évoluer progressivement vers d'autres domaines
domotiques sans remettre en cause l'architecture centrale.

Objectif actuel : construire d'abord une base logicielle locale robuste
pour l'éclairage, avant d'intégrer les communications réseau.

------------------------------------------------------------------------

## 2. Architecture globale prévue

Architecture logique :

``` text
                         ┌──────────────────┐
                         │       CORE       │
                         │ Coordination /   │
                         │ passerelle future│
                         └────────┬─────────┘
                                  │
                    ┌─────────────┼────────────  ─┐
                    │             │               │
             ┌──────▼──────┐  ┌────▼─────┐    ┌────▼─────┐
             │ MAIN_LIGHTING│ │MAIN_SECURITY│ │MAIN_VIDEO│
             └──────┬──────┘  └──────────┘    └──────────┘
                    │
              ┌─────┼─────┐
              │     │     │
            LAMP   LAMP   RELAY
```

### Règles d'architecture

-   `CORE` : coordination générale et future passerelle Internet/mobile.
-   `MAIN` : coordinateur d'un domaine fonctionnel.
-   `LAMP` : équipement terrain.
-   `RELAY` : extension de portée lorsque la communication radio sera
    intégrée.
-   Les couches métier ne doivent pas dépendre directement du transport
    réseau.
-   ESP-NOW est volontairement mis en pause pendant la construction de
    la couche logique.
-   La communication réseau devra plus tard alimenter les mêmes
    structures et fonctions que celles actuellement testées localement.

------------------------------------------------------------------------

## 3. Environnement de développement

-   ESP32 Dev Module
-   Arduino framework
-   PlatformIO
-   VS Code
-   `SimulationTransport` pour les tests natifs
-   C++
-   Repository GitHub : `smart-lighting`

Configuration principale :

``` ini
[platformio]
default_envs = lamp_c6

[env]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
```

Environnements :

-   `core`
-   `main`
-   `relay`

------------------------------------------------------------------------

# 4. Architecture logicielle actuelle

## 4.1 Device

Fichier :

``` text
include/device.h
```

Un périphérique possède notamment :

-   `id`
-   `name`
-   `role`
-   `status`
-   `lastSeen`

Statuts :

``` cpp
enum class DeviceStatus {
    OFFLINE,
    ONLINE
};
```

------------------------------------------------------------------------

## 4.2 Lamp

Fichiers :

``` text
include/lamp.h
include/lamp_controller.h
src/lamp_controller.cpp
```

État d'une lampe :

``` cpp
struct LampState {
    bool power;
    uint8_t brightness;
    bool automatic;
};
```

Fonctions principales :

-   `setLampPower()`
-   `setLampBrightness()`
-   `setLampAutomatic()`
-   `printLampState()`

------------------------------------------------------------------------

## 4.3 Device Registry

Fichiers :

``` text
include/device_registry.h
src/device_registry.cpp
```

Le registre actuel peut contenir jusqu'à :

``` cpp
constexpr uint8_t MAX_LAMPS = 10;
```

Fonctions :

-   `initLampRegistry()`
-   `findLamp()`
-   `addLamp()`
-   `printLampRegistry()`

------------------------------------------------------------------------

# 5. Commandes

Fichier :

``` text
include/command.h
```

Types actuels :

``` cpp
SET_POWER
SET_BRIGHTNESS
SET_AUTOMATIC
SET_GROUP_POWER
SET_GROUP_BRIGHTNESS
```

Statuts :

``` cpp
PENDING
EXECUTED
PARTIAL
FAILED
```

Les commandes peuvent être exécutées sur une lampe ou sur un groupe.

------------------------------------------------------------------------

# 6. Groupes

Fichiers :

``` text
include/group.h
include/group_manager.h
src/group_manager.cpp
```

Un groupe contient une liste d'identifiants de lampes.

Fonctions :

-   création/suppression de groupes ;
-   recherche de groupe ;
-   ajout/suppression de lampe ;
-   test d'appartenance ;
-   affichage du registre.

Limite actuelle :

``` cpp
constexpr uint8_t MAX_GROUPS = 10;
constexpr uint8_t MAX_GROUP_LAMPS = 10;
```

------------------------------------------------------------------------

# 7. Scènes

Fichiers :

``` text
include/scene.h
include/scene_manager.h
src/scene_manager.cpp
include/scene_executor.h
src/scene_executor.cpp
```

Une scène est une suite d'actions de groupe.

Une action de scène contient :

``` cpp
struct SceneAction {
    uint32_t groupId;
    CommandType commandType;
    int32_t value;
};
```

Une scène peut contenir jusqu'à :

``` cpp
constexpr uint8_t MAX_SCENE_ACTIONS = 10;
```

Le moteur de scène gère :

-   `EXECUTED`
-   `PARTIAL`
-   `FAILED`

Une lampe `OFFLINE` est ignorée lors de l'exécution d'un groupe.

------------------------------------------------------------------------

# 8. Automatisations

## Version V2.4

Les automatisations supportent :

-   plusieurs conditions ;
-   logique `AND` ;
-   logique `OR` ;
-   luminosité ;
-   présence ;
-   heure.

Conditions :

``` cpp
LIGHT_LEVEL
PRESENCE
TIME
```

Opérateurs :

``` cpp
LESS_THAN
LESS_OR_EQUAL
GREATER_THAN
GREATER_OR_EQUAL
EQUAL
```

------------------------------------------------------------------------

# 9. Temporisation des automatisations --- V2.5

Une automatisation possède :

``` cpp
triggerDelayMs
cooldownMs
```

Comportement :

``` text
condition FALSE
      ↓
condition TRUE
      ↓
attente du délai
      ↓
vérification du cooldown
      ↓
déclenchement
```

Si la condition redevient fausse avant la fin du délai, le déclenchement
est annulé.

------------------------------------------------------------------------

# 10. Modes de déclenchement --- V2.6

Deux modes :

``` cpp
enum class AutomationTriggerMode {
    ONCE,
    REPEAT
};
```

### ONCE

L'automatisation ne se déclenche qu'une fois tant que la condition reste
vraie.

Elle doit repasser par `FALSE` avant de pouvoir être déclenchée à
nouveau.

### REPEAT

L'automatisation peut se déclencher plusieurs fois tant que la condition
reste vraie, en respectant le `cooldownMs`.

------------------------------------------------------------------------

# 11. Couche Action --- V2.7

C'est la dernière version validée.

Fichier :

``` text
include/action.h
```

Types :

``` cpp
enum class ActionType {
    EXECUTE_SCENE,
    SET_LAMP_POWER,
    SET_LAMP_BRIGHTNESS,
    SET_LAMP_AUTOMATIC,
    SET_GROUP_POWER,
    SET_GROUP_BRIGHTNESS
};
```

Une action :

``` cpp
struct Action {
    ActionType type;
    uint32_t targetId;
    int32_t value;
};
```

------------------------------------------------------------------------

# 12. Action Executor

Fichiers :

``` text
include/action_executor.h
src/action_executor.cpp
```

Fonction centrale :

``` cpp
executeAction()
```

Elle permet de transformer une action générique en opération concrète.

Architecture :

``` text
Automation
      │
      ▼
AutomationAction
      │
      ▼
Action
      │
      ▼
ActionExecutor
      │
      ├── Lampe
      ├── Groupe
      └── Scène
```

------------------------------------------------------------------------

# 13. Automatisation V2.7

Une automatisation possède maintenant une liste d'actions :

``` cpp
constexpr uint8_t MAX_AUTOMATION_ACTIONS = 5;
```

Chaque action possède :

``` cpp
ActionType type;
uint32_t targetId;
int32_t value;
```

Fonctions :

``` cpp
addActionToAutomation()
removeActionFromAutomation()
```

------------------------------------------------------------------------

# 14. V2.7 validée

Test actuel :

### `SOIR_PRESENCE`

Conditions :

``` text
LUMINOSITE <= 30
ET
PRESENCE = 1
```

Actions :

``` text
SET_LAMP_BRIGHTNESS
LAMP_01
25%

SET_LAMP_POWER
LAMP_01
ON
```

Résultat validé :

``` text
AUTOMATISATION DECLENCHEE : SOIR_PRESENCE
Action #0
Brightness -> 25%
Action #1
Power -> ON

Actions executees : 2
Actions echouees : 0
```

### `SOIR_HORAIRE`

Conditions :

``` text
LUMINOSITE <= 20
OU
HEURE >= 21:00
```

Action :

``` text
EXECUTE_SCENE
Scene 1
```

Résultat validé :

``` text
Action : EXECUTE_SCENE -> 1
```

La scène `SOIR` s'exécute correctement.

------------------------------------------------------------------------

# 15. Gestion des équipements OFFLINE

Une lampe OFFLINE ne reçoit pas d'action.

Exemple :

``` text
Lampe OFFLINE - commande ignoree : LAMP_03
```

Pour les groupes :

-   au moins une lampe exécutée + une lampe ignorée → `PARTIAL`
-   aucune lampe exécutée → `FAILED`
-   toutes les lampes exécutées → `EXECUTED`

------------------------------------------------------------------------

# 16. Timeout des équipements

Fichier :

``` text
include/device_manager.h
src/device_manager.cpp
```

Timeout actuel :

``` cpp
constexpr uint32_t DEVICE_TIMEOUT = 10000;
```

Une lampe passe `OFFLINE` si aucun `updateDeviceSeen()` n'est reçu
pendant ce délai.

Important : dans les tests locaux actuels, les lampes ne reçoivent pas
encore de heartbeat réel. Elles peuvent donc finir par passer OFFLINE
après environ 10 secondes.

Ce comportement est attendu dans le simulateur actuel.

------------------------------------------------------------------------

# 17. Test V2.7 actuel

Le `main.cpp` contient des scénarios de test :

-   TEST 1 : conditions fausses
-   TEST 2 / 2B : déclenchement initial
-   TEST 3 : condition toujours vraie
-   TEST 4 : retour à FALSE
-   TEST 5 / 5B : nouvel armement
-   TEST 6 : vérification ONCE
-   TEST 7 / 7B : répétition
-   TEST 8 : cooldown supplémentaire

La sortie série actuelle confirme le fonctionnement de :

-   conditions ;
-   AND / OR ;
-   délai ;
-   cooldown ;
-   ONCE ;
-   REPEAT ;
-   scènes ;
-   actions directes ;
-   actions de groupes ;
-   gestion OFFLINE.

------------------------------------------------------------------------

# 18. Décision importante : réseau

ESP-NOW n'est pas encore intégré à la couche logique.

C'est volontaire.

Architecture souhaitée :

``` text
APPLICATION / AUTOMATION
          │
          ▼
     ACTION LAYER
          │
          ▼
    DEVICE / GROUP
          │
          ▼
      NETWORK API
          │
     ┌────┴────┐
     │         │
  ESP-NOW   RS-485/CAN
```

La logique métier ne doit pas dépendre directement d'ESP-NOW.

Plus tard, ESP-NOW pourra être utilisé entre :

``` text
MAIN_LIGHTING
      │
      ├── LAMP
      ├── LAMP
      └── RELAY
```

Les relais pourront éventuellement transmettre les messages pour étendre
la portée.

------------------------------------------------------------------------

# 19. Architecture réseau future

Une communication distribuée pourra utiliser des métadonnées telles que
:

``` text
messageId
source
destination
ttl
```

Un relais devra pouvoir détecter les messages déjà vus afin d'éviter les
boucles et le flooding incontrôlé.

La cryptographie et la sécurité réseau seront traitées lorsque la couche
communication sera intégrée.

------------------------------------------------------------------------

# 20. Architecture multi-domaines future

Le système est destiné à évoluer vers plusieurs domaines :

``` text
CORE
 │
 ├── MAIN_LIGHTING
 │      ├── LAMP
 │      ├── SENSOR
 │      └── RELAY
 │
 ├── MAIN_SECURITY
 │
 ├── MAIN_VIDEO
 │
 └── autres modules
```

Chaque domaine doit pouvoir rester relativement autonome tout en pouvant
être coordonné par le CORE.

------------------------------------------------------------------------

# 21. Règles de développement

1.  Ne pas intégrer le réseau trop tôt.
2.  Tester la logique localement avant d'ajouter ESP-NOW.
3.  Séparer les responsabilités dans des fichiers distincts.
4.  Éviter que les automatisations dépendent directement du transport
    réseau.
5.  Utiliser des identifiants plutôt que des pointeurs ou références
    réseau dans les structures persistantes.
6.  Garder les actions génériques.
7.  Faire évoluer l'architecture par versions validées.
8.  Compiler et tester chaque étape avant de passer à la suivante.
9.  Ne pas supprimer une abstraction existante sans raison
    architecturale.
10. Toute nouvelle couche doit rester compatible avec les couches
    précédentes.

------------------------------------------------------------------------

# 22. État actuel

``` text
V1
├── Device
├── Lamp
├── Registry
├── Controller
└── Commandes

V2
├── Groupes
├── Commandes de groupe
└── Scènes

V2.4
└── Automatisations multi-conditions

V2.5
└── Delay + Cooldown

V2.6
└── ONCE / REPEAT

V2.7
└── Action Layer
    ├── Actions directes
    ├── Actions groupe
    └── Actions scène
```

**V2.7 est actuellement la version de référence fonctionnelle.**

------------------------------------------------------------------------

# 23. Prochaine étape

Avant d'ajouter ESP-NOW, poursuivre la consolidation de la couche
logique.

Point à traiter en priorité :

``` text
Automation
```

Le champ historique :

``` cpp
sceneId
```

est désormais redondant avec :

``` cpp
AutomationAction actions[];
```

Il devra être évalué puis probablement supprimé de `Automation` une fois
que tout le code n'en dépendra plus.

Ensuite, les prochaines évolutions possibles sont :

1.  événements ;
2.  capteurs génériques ;
3.  état environnemental centralisé ;
4.  scheduler ;
5.  historique / logs ;
6.  gestion plus robuste des erreurs ;
7.  abstraction de transport ;
8.  intégration ESP-NOW ;
9.  découverte des appareils ;
10. relais et réseau maillé.

------------------------------------------------------------------------

# 24. Git

Après chaque étape validée :

``` bash
git status
git add .
git commit -m "feat: description"
git push
```

Les commits doivent représenter des étapes fonctionnelles et testées.

Exemple pour V2.7 :

``` bash
git add .
git commit -m "feat: add automation action layer"
git push
```

------------------------------------------------------------------------

## Point de reprise

**Version : V2.7**

**Dernière fonctionnalité validée : couche générique `Action` /
`ActionExecutor`.**

**Dernier test validé :**

``` text
SOIR_PRESENCE
    → SET_LAMP_BRIGHTNESS 25%
    → SET_LAMP_POWER ON

SOIR_HORAIRE
    → EXECUTE_SCENE 1
```

**Prochaine tâche : nettoyer l'ancienne dépendance `sceneId` de
`Automation`, puis continuer la conception de la couche logique avant le
réseau.**
