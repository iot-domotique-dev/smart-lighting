# Smart Lighting

La préparation du provisioning V4 MAIN ↔ LAMP est décrite dans
[`V4_PAIRING.md`](V4_PAIRING.md).

Système d'éclairage intelligent modulaire basé sur ESP32, conçu pour évoluer progressivement vers une architecture domotique distribuée utilisant Zigbee.

La liaison Zigbee V5 entre `MAIN_LIGHTING` et `LAMP_C6` est validée sur le matériel. La V6 ajoute le registre générique CORE; la V7 prépare son interface pour l'application mobile sans remplacer le chemin de commandes V5.

Le projet est développé avec **PlatformIO / VS Code**. Les tests locaux utilisent `SimulationTransport` et Unity.

Voir aussi l'[audit complet du projet](AUDIT_PROJET.md).

## Architecture

```text
                         ┌───────────────────┐
                         │       CORE        │
                         │  DeviceRegistry   │
                         └─────────┬─────────┘
                                   │
             ┌─────────────────────┼─────────────────────┐
             │                     │                     │
             ▼                     ▼                     ▼
      MAIN_LIGHTING         MAIN_SECURITY           MAIN_VIDEO
             │              (representable)         (representable)
           Zigbee
             │
       ┌─────┼─────┐
       ▼     ▼     ▼
    LAMP_C6 LAMP_C6 ...
```

Le registre générique représente les modules et leur hiérarchie. Le lien Zigbee V5 déjà validé reste `MAIN_LIGHTING` ↔ `LAMP_C6`; le lien réseau CORE ↔ MAIN et les fonctions SECURITY/VIDEO ne font pas partie de la V6.

### Rôles

- **CORE** : coordination globale et passerelle vers les systèmes externes.
- **MAIN** : contrôleur principal d'un sous-système (`MAIN_LIGHTING`, `MAIN_SECURITY`, etc.).
- **LAMP** : équipement physique d'éclairage.
- **RELAY** : rôle prévu pour des fonctions intermédiaires selon l'évolution du réseau.
- **SENSOR** et **CAMERA** : rôles réservés pour les extensions futures.

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
- parent ;
- capacités descriptives ;
- alimentation ;
- luminosité ;
- mode automatique.

### DeviceRegistry V6

`DeviceRegistry` stocke jusqu'à 32 appareils de rôles différents. Il permet l'enregistrement, le retrait, la recherche par ID ou par rôle, l'énumération et la mise à jour d'un appareil. `parentId` décrit la relation CORE → MAIN → device.

Les capacités sont un masque de bits descriptif. Les valeurs actuelles couvrent `POWER`, `BRIGHTNESS`, `AUTOMATIC`, `LIGHTING`, `GROUPS`, `SCENES` et `AUTOMATION`. Des indicateurs `ALARM`, `PRESENCE`, `DOOR_SENSOR` et `SECURITY_MODE` peuvent représenter un futur module SECURITY; aucun comportement SECURITY ou VIDEO n'est implémenté.

`LampRegistry` reste le registre spécialisé utilisé par l'éclairage V5 pour l'état `LampState`, l'identité de pairing et les commandes. `DeviceManager` sait aussi surveiller les statuts du registre générique; les événements existants `LAMP_ONLINE` et `LAMP_OFFLINE` sont conservés pour les lampes.

Pour une future API du CORE, `getDevices()` correspond à `getAllDevices()`, et `getDeviceState(id)` peut lire la fiche avec `findDeviceById()`. `sendCommand(command)` reste à relier au service de commandes V5; aucune API HTTP/mobile n'est incluse en V6.

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

`SimulationTransport` est utilisé pour les tests natifs; le profil Wokwi a été retiré avant la V7.

### Zigbee

`ZigbeeTransport` implémente le réseau Zigbee réel V5 entre `MAIN_LIGHTING` et `LAMP_C6`. `SimulationTransport` reste disponible pour les tests natifs.

## État du projet

### V5 - Zigbee réel

- [x] Liaison Zigbee `MAIN_LIGHTING` <-> `LAMP_C6` validée sur le matériel par l'utilisateur.
- [x] Commande `power <id> on|off` et sortie GPIO18 de `LAMP_C6`.
- [x] Chemin V5 de messages, pairing, ACK, retry et déduplication conservé.

### V6 - Registre générique

- [x] Registre CORE/MAIN/LAMP/RELAY/SENSOR/CAMERA avec capacités, parent, recherche, mise à jour et statut.
- [x] 36 tests natifs et builds Arduino réussis.
- [ ] Build C6 non confirmé: le compilateur RISC-V s'arrête dans le test CMake avant la compilation du code du projet.

### V7 - Services du CORE

En cours: définir l'interface commune qui permettra à l'application mobile de consulter les modules et leur état, puis de leur envoyer des commandes. Le CORE initialise son registre, ingère les annonces de modules sans doublons et calcule leurs IDs stables à partir du parent et de l'ID local. Voir le [contrat de l'API CORE V7](V7_CORE_API.md). L'accès distant est prévu par VPN privé; le transport CORE <-> MAIN et la coexistence Wi-Fi/Zigbee restent à valider.

### Simulation locale

`SimulationTransport` reste disponible pour les tests natifs. Les profils et fichiers Wokwi ont été retirés: ils ne représentaient qu'une carte et rejouaient des scénarios de démonstration inutiles.

### Environnements PlatformIO

```text
core
main
relay
main_light_c6
lamp_c6
native
```

`lamp_c6` est l'unique firmware de lampe maintenu et correspond au module V5 validé sur GPIO18. `lamp_a` et `lamp_b` étaient des variantes d'identité de la démonstration. `SENSOR` et `CAMERA` restent des rôles du registre générique sans profil firmware dédié.

### Étapes V7

- [ ] Interface d'inventaire consommable par l'application mobile.
- [ ] Lecture des modules et de leur état par une API commune.
- [ ] Routage générique des commandes vers les MAIN.
- [ ] Choix du transport CORE <-> MAIN et validation Wi-Fi/Zigbee sur le matériel.
- [ ] Accès distant par VPN privé vers le réseau de la maison.

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
│   ├── core_module_service.h
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
pio run -e lamp_c6
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
Current version: V7 - services CORE pour l'application mobile (en cours)
V5: liaison Zigbee MAIN_LIGHTING <-> LAMP_C6 validée sur le matériel
V6: registre générique implémenté; build C6 non vérifié
Default firmware: lamp_c6
Target hardware: ESP32-C6
```

Vérifications réussies pendant la V6:

```bash
pio run -e core -e main -e relay
pio test -e native
```

Les builds Arduino et 36 tests natifs ont réussi. Les builds C6 ont été tentés, mais le compilateur RISC-V s'arrête dans son test CMake avec une erreur Windows d'accès au chemin, avant la compilation des sources du projet. Le profil `native` requiert un compilateur C/C++ compatible GCC.

## Principe général

```text
Construire d'abord une architecture logicielle
stable et indépendante des transports,

puis relier l'application mobile au CORE,
et le CORE aux MAIN sans remplacer les liens
de terrain déjà validés.
```

L'objectif est de garder un contrat commun pour l'éclairage, la sécurité, la vidéosurveillance et les futurs modules.
