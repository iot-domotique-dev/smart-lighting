> Archive historique : instantané V2.9. L’état et les procédures actuels sont dans le [README du dépôt](../../README.md) et les [documents actifs](../README.md).

# PROJECT CONTEXT — SMART LIGHTING

## Version actuelle : V2.9

Ce document décrit l'état fonctionnel et architectural du projet Smart Lighting à la fin de la V2.9. Il sert de référence pour reprendre le développement dans une nouvelle conversation.

### Architecture actuelle

```text
Event
  ↓
EventBus
  ↓
EventProcessor
  ↓
AutomationEngine
  ↓
Action
  ↓
Lamp / Group / Scene
```

### Objectif du projet

Smart Lighting est un système domotique modulaire basé sur ESP32. Le projet vise notamment l'éclairage intelligent, les capteurs, les automatisations, les groupes, les scènes et, à terme, la communication entre plusieurs catégories de modules.

La logique métier est développée avant l'intégration réseau afin de rester indépendante du transport.

### Environnement

- VS Code
- PlatformIO
- Arduino
- ESP32 Dev Module
- Wokwi
- Git / GitHub

Projet :

```text
C:\Users\merve\Documents\GitHub\smart-lighting
```

La V2.9 fonctionne avec une simulation locale. ESP-NOW n'est pas encore intégré.

### Rôles

- CORE
- MAIN
- LAMP
- RELAY

Architecture cible :

```text
CORE
 |
 +-------------------+
 |                   |
MAIN_LIGHTING     autres MAIN
 |
 +---------+---------+
 |                   |
LAMPES              RELAIS
```

### Appareils

`Device` contient notamment :

- ID
- nom
- rôle
- statut ONLINE/OFFLINE
- `lastSeen`

`Lamp` ajoute :

- `power`
- `brightness`
- `automatic`

`LampRegistry` gère les lampes. Capacité actuelle : `MAX_LAMPS = 10`.

### Gestion ONLINE/OFFLINE

Timeout actuel :

```text
DEVICE_TIMEOUT = 10000 ms
```

`updateDeviceSeen()` actualise `lastSeen` et met l'appareil ONLINE.

`updateDeviceStatus()` vérifie les timeouts.

Dans la simulation, les appareils peuvent devenir OFFLINE si aucun nouveau `lastSeen` n'est produit. Ce comportement est attendu.

### Commandes

Types :

- SET_POWER
- SET_BRIGHTNESS
- SET_AUTOMATIC
- SET_GROUP_POWER
- SET_GROUP_BRIGHTNESS

Statuts :

- PENDING
- EXECUTED
- PARTIAL
- FAILED

### Groupes

Un `LampGroup` contient :

- ID
- nom
- IDs des lampes
- nombre de lampes

Capacité actuelle : `MAX_GROUPS = 10`.

Les doublons, groupes inexistants, lampes inexistantes et dépassements de capacité sont contrôlés.

Les lampes OFFLINE sont ignorées lors des commandes de groupe.

### Scènes

Une scène est une collection d'actions.

Scène actuelle : `SOIR`

```text
Action #0
SET_GROUP_POWER
Groupe ENTREE
ON

Action #1
SET_GROUP_BRIGHTNESS
Groupe ENTREE
40 %
```

Une scène peut être `PARTIAL` lorsqu'une partie de ses cibles est indisponible.

### Automatisations

Une automatisation possède :

- ID
- nom
- état actif/inactif
- conditions
- logique AND/OR
- actions
- état de condition
- `conditionSince`
- `lastTriggered`
- `triggerDelayMs`
- `cooldownMs`
- `triggerMode`

Conditions :

- LIGHT_LEVEL
- PRESENCE
- TIME

Opérateurs :

- LESS_THAN
- LESS_OR_EQUAL
- GREATER_THAN
- GREATER_OR_EQUAL
- EQUAL

### Temporisation

`triggerDelayMs` impose que la condition reste vraie avant le déclenchement.

Si elle repasse à FALSE avant la fin du délai, le déclenchement est annulé.

`cooldownMs` impose un délai minimal entre deux déclenchements.

### Modes

`ONCE` : déclenche une fois tant que la condition reste vraie, puis attend un retour à FALSE.

`REPEAT` : peut se redéclencher tant que la condition reste vraie, en respectant le cooldown.

Ces comportements ont été validés.

### Couche d'actions

Types :

- EXECUTE_SCENE
- SET_LAMP_POWER
- SET_LAMP_BRIGHTNESS
- SET_LAMP_AUTOMATIC
- SET_GROUP_POWER
- SET_GROUP_BRIGHTNESS

Une automatisation peut agir directement sur une lampe ou un groupe, exécuter une scène ou combiner plusieurs actions.

### Découplage V2.8

Avant V2.8, une automatisation possédait une référence directe vers une scène.

Cette dépendance a été supprimée.

```text
AUTOMATION
 |
 +--> Conditions
 |
 +--> Timing
 |
 +--> Actions
       |
       +--> LAMPE
       +--> GROUPE
       +--> SCÈNE
```

Une automatisation n'a plus besoin d'une scène pour exister.

# Event Bus — V2.9

La V2.9 introduit :

- `event.h`
- `event_bus.h`
- `event_bus.cpp`
- `event_processor.h`
- `event_processor.cpp`
- `id_generator.h`
- `id_generator.cpp`
- `automation_context.h`

## Event

Un événement contient :

- ID
- type
- `sourceId`
- `value`
- `value2`
- `timestamp`

Types actuellement définis :

- LIGHT_LEVEL_CHANGED
- PRESENCE_CHANGED
- TIME_CHANGED
- LAMP_ONLINE
- LAMP_OFFLINE
- COMMAND_RECEIVED
- DEVICE_STATE_CHANGED

## Event Bus

Le bus est une file circulaire statique.

Capacité :

```text
MAX_EVENTS = 20
```

Structure :

```text
events[]
head
tail
count
```

Fonctions :

- `initEventBus()`
- `publishEvent()`
- `consumeEvent()`

## Event Processor

Flux :

```text
EventBus
   |
   v
EventProcessor
   |
   v
AutomationEngine
```

Les événements pertinents déclenchent l'évaluation des automatisations.

## Identifiants

Les événements utilisent actuellement un compteur local :

```text
Event #1
Event #2
Event #3
...
```

Une stratégie d'identification distribuée devra être définie avant le réseau.

## AutomationContext

Contient actuellement :

- `timestamp`
- `lightLevel`
- `presenceDetected`
- `hour`
- `minute`

# Automatisations actuelles

## SOIR_PRESENCE

```text
ID       : 1
Logique  : AND
Mode     : ONCE
Délai    : 3000 ms
Cooldown : 10000 ms
```

Conditions :

```text
LUMINOSITE <= 30
PRESENCE = 1
```

Actions :

```text
SET_LAMP_BRIGHTNESS
LAMP_01
25 %

SET_LAMP_POWER
LAMP_01
ON
```

## SOIR_HORAIRE

```text
ID       : 2
Logique  : OR
Mode     : REPEAT
Délai    : 2000 ms
Cooldown : 8000 ms
```

Conditions :

```text
LUMINOSITE <= 20
HEURE >= 21:00
```

Action :

```text
EXECUTE_SCENE
SCENE ID 1
```

# Validation V2.9

La simulation V2.9 a été compilée et exécutée avec succès.

Le log confirme le passage réel par l'Event Bus :

```text
Evenement publie

>>> TRAITEMENT EVENEMENT

===== EVENT =====
ID        : 3
Type      : LIGHT_LEVEL_CHANGED
Value     : 1
Value 2   : 1
=================
```

Puis :

```text
AUTOMATISATION DECLENCHEE : SOIR_PRESENCE

Action #0
Brightness -> 25%

Action #1
Power -> ON
```

Pour `SOIR_HORAIRE` :

```text
AUTOMATISATION DECLENCHEE : SOIR_HORAIRE

Action #0
Action : EXECUTE_SCENE -> 1
```

Puis la scène `SOIR` est exécutée.

Le comportement `ONCE`, `REPEAT`, délai et cooldown a été validé.

### État final observé

```text
LAMP_01
    ONLINE
    Power : ON
    Brightness : 40 %
    Automatic : NON

LAMP_02
    ONLINE
    Power : ON
    Brightness : 40 %
    Automatic : NON

LAMP_03
    OFFLINE
    Power : OFF
    Brightness : 100 %
    Automatic : OUI
```

LAMP_03 est volontairement initialisée OFFLINE pour tester le comportement `PARTIAL`.

# Ce qui n'est pas encore implémenté

- ESP-NOW
- réseau maillé
- relais ESP-NOW
- communication CORE ↔ MAIN
- RS-485
- CAN
- Internet
- application mobile
- Home Assistant
- cloud
- authentification réseau
- chiffrement réseau
- découverte automatique des modules
- synchronisation distribuée
- persistance de configuration

# Prochaine étape : V3

La prochaine grande étape est :

```text
V3 — COMMUNICATION INTER-MODULES
```

Objectif : introduire la communication entre ESP32 sans réécrire la logique métier déjà développée.

Architecture cible :

```text
                    CORE
                     |
              communication
                     |
                MAIN_LIGHTING
                     |
          +----------+----------+
          |          |          |
          v          v          v
       LAMP_01    LAMP_02     RELAY
                                  |
                                  +--> autres modules
```

Le transport réseau devra alimenter la même architecture événementielle :

```text
RÉSEAU
  |
  v
MESSAGE
  |
  v
EVENT
  |
  v
EVENT BUS
  |
  v
LOGIQUE EXISTANTE
```

L'EventBus devient la frontière entre la communication et la logique applicative.

# Méthode de développement

Pour chaque version :

1. définir l'objectif ;
2. modifier les structures nécessaires ;
3. compiler avec PlatformIO ;
4. tester dans Wokwi ;
5. vérifier les sorties série ;
6. corriger les erreurs ;
7. valider le comportement ;
8. mettre à jour `PROJECT_CONTEXT.md` ;
9. commit Git ;
10. push GitHub.

Une version n'est considérée comme validée qu'après compilation et test réel.

# Commit V2.9

```bash
git status
git add .
git commit -m "feat: add event bus architecture"
git push
```

# État de référence

Version actuelle :

```text
V2.9
```

Architecture validée :

```text
Event
  ↓
EventBus
  ↓
EventProcessor
  ↓
AutomationEngine
  ↓
Action
  ↓
Lamp / Group / Scene
```

Prochaine version :

```text
V3 — Communication inter-modules
```

La V3 devra construire la communication autour de cette architecture existante et ne pas remplacer la logique métier déjà validée.
