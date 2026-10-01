# Smart Lighting

La préparation du provisioning V4 MAIN ↔ LAMP est décrite dans
[`V4_PAIRING.md`](V4_PAIRING.md).

Système d'éclairage intelligent modulaire basé sur ESP32, conçu pour évoluer progressivement vers une architecture domotique distribuée utilisant Zigbee.

Le projet est développé avec **PlatformIO / VS Code** et utilise actuellement **Wokwi** pour les simulations.

Voir aussi l'[audit complet du projet](AUDIT_PROJET.md).

## Architecture

```text
                         ┌───────────────────┐
                         │       CORE        │
                         │ Coordination /    │
                         │ Gateway / Logic   │
                         └─────────┬─────────┘
                                   │
                              Zigbee
                                   │
             ┌─────────────────────┼─────────────────────┐
             │                     │                     │
             ▼                     ▼                     ▼
      ┌─────────────┐       ┌─────────────┐       ┌─────────────┐
      │ MAIN LIGHT  │       │ MAIN SECURITY│      │ MAIN VIDEO  │
      └──────┬──────┘       └─────────────┘       └─────────────┘
             │
       ┌─────┼─────┐
       ▼     ▼     ▼
    LAMP01 LAMP02 LAMP03
```

### Rôles

- **CORE** : coordination globale et passerelle vers les systèmes externes.
- **MAIN** : contrôleur principal d'un sous-système (`MAIN_LIGHTING`, `MAIN_SECURITY`, etc.).
- **LAMP** : équipement physique d'éclairage.
- **RELAY** : rôle prévu pour des fonctions intermédiaires selon l'évolution du réseau.

## Architecture logicielle

La logique applicative est séparée de la communication :

```text
┌───────────────────────────────────────────────┐
│              APPLICATION LOGIC                │
│  Lampes / Groupes / Scènes / Automatisations │
│  Événements / Commandes / États               │
└──────────────────────────┬────────────────────┘
                           │
┌──────────────────────────▼────────────────────┐
│              MESSAGE MANAGEMENT               │
│  Messages / ACK / Retry / Deduplication       │
└──────────────────────────┬────────────────────┘
                           │
┌──────────────────────────▼────────────────────┐
│             COMMUNICATION LAYER               │
│              Transport abstraction            │
└──────────────────────────┬────────────────────┘
                           │
             ┌─────────────┴─────────────┐
             ▼                           ▼
     SimulationTransport          ZigbeeTransport
          (actuel)                    (futur)
```

L'objectif est de pouvoir remplacer le transport simulé par Zigbee sans réécrire la logique applicative.

## Fonctionnalités

### Appareils et lampes

Gestion de :

- identifiant ;
- nom ;
- rôle ;
- état `ONLINE` / `OFFLINE` ;
- dernière activité ;
- alimentation ;
- luminosité ;
- mode automatique.

Commandes principales :

```text
SET_POWER
SET_BRIGHTNESS
SET_AUTOMATIC
```

### Groupes

Les lampes peuvent être regroupées afin d'appliquer des commandes à plusieurs appareils.

```text
Groupe : JARDIN
 ├── LAMP01
 ├── LAMP02
 ├── LAMP03
 └── LAMP04
```

Commandes :

```text
SET_GROUP_POWER
SET_GROUP_BRIGHTNESS
```

### Scènes

Une scène regroupe plusieurs actions permettant de créer un état prédéfini.

États d'exécution :

```text
EXECUTED
PARTIAL
FAILED
```

### Automatisations

Conditions :

```text
LIGHT_LEVEL
PRESENCE
TIME
```

Opérateurs :

```text
LESS_THAN
LESS_OR_EQUAL
GREATER_THAN
GREATER_OR_EQUAL
EQUAL
```

Logique :

```text
AND
OR
```

Modes :

```text
ONCE
REPEAT
```

Les automatisations prennent également en compte les délais de déclenchement, cooldowns et états précédents des conditions.

### Event Bus

Événements actuellement définis :

```text
LIGHT_LEVEL_CHANGED
PRESENCE_CHANGED
TIME_CHANGED
LAMP_ONLINE
LAMP_OFFLINE
COMMAND_RECEIVED
DEVICE_STATE_CHANGED
```

Flux général :

```text
Sensor
   │
   ▼
 Event
   │
   ▼
Event Bus
   │
   ▼
Event Processor
   │
   ▼
Automation Engine
   │
   ▼
Action
```

### Messages

Types :

```text
COMMAND
EVENT
STATE
HEARTBEAT
ACK
```

Chaque message possède notamment un identifiant, une source, une destination, un type, un timestamp, des valeurs et des états de transmission/exécution.

### Fiabilité

Le système gère :

- ACK ;
- timeout ;
- retransmissions ;
- nombre maximal de tentatives ;
- suivi des messages ;
- déduplication.

Exemple :

```text
COMMAND #42
     │
     ▼
Execution
     │
     ▼
ACK perdu
     │
     ▼
Retry #42
     │
     ▼
Détection duplicate
     │
     ▼
Pas de seconde exécution
```

## Transport

L'interface commune est :

```text
CommunicationTransportInterface
```

avec notamment :

```text
begin()
send()
receive()
isReady()
name()
```

### Simulation

`SimulationTransport` est actuellement utilisé pour les tests locaux avec Wokwi.

### Zigbee

`ZigbeeTransport` est la couche prévue pour l'intégration du réseau Zigbee réel.

## État du projet

### V3.9 — Transport Abstraction

Fonctionnalités validées :

- [x] gestion des appareils
- [x] gestion des lampes
- [x] groupes
- [x] scènes
- [x] automatisations
- [x] Event Bus
- [x] messages
- [x] ACK
- [x] timeout
- [x] retransmissions
- [x] déduplication
- [x] message tracker
- [x] message router
- [x] abstraction du transport
- [x] simulation du transport

Scénarios de démonstration Wokwi historiques (distincts de la suite native) :

1. transmission normale avec ACK ;
2. perte de message et retransmissions ;
3. perte d'ACK et déduplication.

### V3.9 stabilisée — simulation

- Les six environnements ESP32 (`core`, `main`, `lamp`, `relay`, `lamp_a`, `lamp_b`) compilent.
- La suite PlatformIO native comprend neuf tests reproductibles ; les neuf passent.
- `lamp_a` et `lamp_b` restent des rôles `DEVICE_ROLE_LAMP` et utilisent des identifiants locaux distincts (1 et 2).
- `lamp` conserve les démonstrations Wokwi ; les autres profils démarrent le service applicatif sans injecter d'événements de démonstration.
- Aucun pilote de capteur ne publie encore les événements de luminosité, présence ou temps ; les automatisations ne sont donc pas validées en service autonome.
- Le transport actif reste `SimulationTransport`. La compilation ne valide ni le matériel ESP32-C6, ni Zigbee, ni les sorties électriques.

## Simulation Wokwi

La simulation actuelle utilise un seul ESP32 et teste la logique logicielle avant le passage au matériel réel.

```text
PlatformIO
    │
    ▼
ESP32 Dev Module
    │
    ▼
Wokwi
```

Le transport utilisé est :

```text
SimulationTransport
```

Il ne s'agit pas encore d'un réseau Zigbee réel.

## Environnements PlatformIO

Environnements actuellement définis :

```text
core
main
lamp
relay
lamp_a
lamp_b
```

Les rôles sont sélectionnés par des flags de compilation :

```text
DEVICE_ROLE_CORE
DEVICE_ROLE_MAIN
DEVICE_ROLE_LAMP
DEVICE_ROLE_RELAY
```

Les identités des deux profils de lampe sont indépendantes du rôle : `DEVICE_LAMP_A` fixe l'identifiant local 1 et `DEVICE_LAMP_B` l'identifiant 2.

## Matériel cible

La prochaine étape matérielle utilise des microcontrôleurs **ESP32-C6** compatibles avec la connectivité nécessaire au futur réseau Zigbee.

Architecture de test prévue :

```text
             Zigbee
        ┌───────────────┐
        │               │
        ▼               ▼

     CORE C6         LAMP01 C6
                         │
                         │
                      Zigbee
                         │
                         ▼
                     LAMP02 C6
```

Le matériel définitif des lampes, de l'alimentation et des cartes électroniques sera déterminé après validation du prototype.

## Principes de conception

### Modularité

Chaque fonction importante doit être isolée dans un module indépendant.

### Séparation des responsabilités

La logique applicative ne doit pas dépendre directement du matériel de communication.

### Évolutivité

L'architecture doit permettre d'ajouter de nouveaux sous-systèmes sans réécrire le système existant.

### Testabilité

Chaque couche doit pouvoir être testée indépendamment.

### Communication abstraite

Le protocole réseau doit rester interchangeable autant que possible.

### Développement progressif

Chaque couche est validée avant d'ajouter la suivante.

## Roadmap

### V1 — Gestion des appareils

- [x] rôles des appareils
- [x] gestion des lampes
- [x] registre des appareils
- [x] détection ONLINE/OFFLINE

### V2 — Logique domotique

- [x] groupes
- [x] scènes
- [x] actions
- [x] automatisations
- [x] Event Bus

### V3 — Communication logicielle

- [x] messages
- [x] ACK
- [x] timeout
- [x] retransmissions
- [x] déduplication
- [x] message tracker
- [x] message router
- [x] transport abstraction

### V4 — Zigbee

- [ ] choix définitif du matériel
- [ ] premier ESP32-C6 réel
- [ ] initialisation Zigbee
- [ ] création du réseau
- [ ] CORE Zigbee
- [ ] premier module LAMP Zigbee
- [ ] communication CORE ↔ LAMP
- [ ] découverte des appareils
- [ ] adressage
- [ ] commandes réelles
- [ ] remontée des états

### V5 — Prototype réel

- [ ] plusieurs lampes physiques
- [ ] capteur de luminosité
- [ ] détection de présence
- [ ] synchronisation des lampes
- [ ] alimentation réelle
- [ ] boîtier
- [ ] tests de portée
- [ ] tests de stabilité
- [ ] tests de sécurité

### Évolutions futures

```text
SMART LIGHTING
      │
      ├── LIGHTING
      ├── SECURITY
      ├── VIDEO
      ├── ACCESSIBILITY
      └── OTHER MODULES
```

Chaque sous-système pourra rester autonome tout en pouvant communiquer avec le CORE.

## Structure du projet

```text
smart-lighting/
│
├── include/
│   ├── action.h
│   ├── automation.h
│   ├── automation_context.h
│   ├── automation_engine.h
│   ├── automation_manager.h
│   ├── command.h
│   ├── command_handler.h
│   ├── communication.h
│   ├── communication_transport.h
│   ├── device.h
│   ├── device_manager.h
│   ├── device_registry.h
│   ├── event.h
│   ├── event_bus.h
│   ├── event_processor.h
│   ├── group.h
│   ├── group_manager.h
│   ├── lamp.h
│   ├── lamp_controller.h
│   ├── message.h
│   ├── message_deduplicator.h
│   ├── message_manager.h
│   ├── message_router.h
│   ├── message_tracker.h
│   ├── roles.h
│   ├── scene.h
│   ├── scene_executor.h
│   ├── scene_manager.h
│   ├── simulation_transport.h
│   └── zigbee_transport.h
│
├── src/
│   └── ...
│
├── platformio.ini
├── wokwi.toml
└── README.md
```

## Installation

Cloner le projet :

```bash
git clone <repository-url>
cd smart-lighting
```

Compiler :

```bash
pio run
```

Compiler un environnement spécifique :

```bash
pio run -e lamp
```

Compiler le CORE :

```bash
pio run -e core
```

Compiler le MAIN :

```bash
pio run -e main
```

## Version actuelle

```text
Project: Smart Lighting
Current version: V3.9 stabilisée (simulation)
Status: Builds ESP32 validés, 9 tests natifs validés
Current transport: Simulation
Target transport: Zigbee
Target hardware: ESP32-C6
```

Vérifications locales :

```bash
pio run -e core -e main -e lamp -e relay -e lamp_a -e lamp_b
pio test -e native
```

L'environnement de test `native` nécessite un compilateur C/C++ compatible GCC disponible sur le poste. Le profil Wokwi `lamp` conserve les scénarios de démonstration.

## Principe général

```text
Construire d'abord une architecture logicielle
stable et indépendante du matériel,

puis remplacer progressivement la simulation
par la communication Zigbee réelle.
```

L'objectif est d'éviter de reconstruire toute l'architecture lorsque le projet passe de la simulation au matériel physique.
