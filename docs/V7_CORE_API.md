# V7 — Contrat de l'API CORE

## Objectif

L'application mobile parle au CORE. Elle n'a pas à connaître les détails Zigbee ni le protocole des MAIN. Le CORE présente un inventaire commun, lit l'état des modules et route les commandes vers le MAIN propriétaire.

```text
Application mobile
        │ API versionnée
        ▼
CORE : inventaire + état + commandes
        │ C6-WIFI -- UART -- C6-ZIGBEE -- Zigbee/PAN
        ▼
MAIN_LIGHTING / MAIN_SECURITY / MAIN_VIDEO ...
        │
   modules de terrain
```

## Ressource module

Chaque entrée s'appuie sur `Device` et `DeviceRegistry` de la V6. Le format exposé à l'application contient :

```json
{
  "id": 3081691923,
  "name": "MAIN_LIGHTING",
  "role": "main",
  "status": "online",
  "parentId": 1,
  "capabilities": ["lighting", "groups", "scenes"],
  "lastSeen": 123456
}
```

`id` est l'identifiant stable calculé par le CORE; `parentId` décrit la hiérarchie CORE → MAIN → module terrain, et `capabilities` annonce les opérations prises en charge. `localId` reste interne au CORE et sert au routage vers le parent. L'ID de cet exemple est calculé pour `parentId = 1` et `localId = 4660`; le `localId` n'est pas exposé à l'application. Les données propres à un domaine, comme power ou brightness pour une lampe, viennent de l'état du MAIN; elles ne sont pas inventées à partir du seul descripteur `Device`.

Chaque parent fournit un `localId` non nul, stable après redémarrage et unique parmi ses enfants. Deux parents peuvent réutiliser le même `localId`. Le CORE calcule un ID stable à partir de `{parentId, localId}`; cet ID est unique seulement dans l'inventaire d'un CORE, pas entre plusieurs foyers. Les collisions de calcul sont détectées et rejetées. Une annonce répétée reconstruit le même ID après redémarrage, tant que le `localId` du module et celui de ses parents restent stables. La table `{parentId, localId}` fournit les informations nécessaires au routage.

Le CORE utilise l'ID logique `1` pour sa racine. Un MAIN s'annonce d'abord avec `parentId = 1`; après son enregistrement, le CORE lui fournit son ID logique calculé. Le MAIN utilise ensuite cet ID comme `parentId` lorsqu'il annonce ses propres modules.

## Opérations de l'application

Le contrat logique V7 expose trois opérations, quel que soit le transport client :

| Opération | Résultat |
| --- | --- |
| Lister les modules | Ressources `Device` connues du CORE, avec filtre facultatif par rôle, parent ou statut. |
| Lire un module | Descripteur générique et état fourni par son MAIN lorsqu'il est disponible. |
| Envoyer une commande | Résultat d'acceptation avec `commandId`; le résultat final dépend de l'ACK du MAIN. |

Première commande visée: `power.set` pour les lampes paired de la V5. Toute commande est vérifiée selon les capacités de l'appareil, son état ONLINE et le MAIN parent. Une capacité absente donne une erreur explicite au lieu d'être ignorée.

## Règles de comportement

- Le CORE est la source de l'inventaire global; un MAIN reste responsable de ses appareils enfants.
- Une annonce répétée met à jour l'entrée existante au lieu de créer un doublon.
- L'application reçoit les IDs logiques et n'a pas accès aux adresses radio.
- Une commande acceptée n'est pas présentée comme exécutée avant l'ACK ou l'état final du MAIN.
- Le modèle d'API reste indépendant du chemin local ou distant utilisé par l'application.

## Transport et accès

**Accès distant retenu:** VPN privé vers le réseau domestique. Le CORE expose son API sur le réseau local; depuis l'extérieur, le téléphone rejoint ce même réseau par VPN. Le VPN se termine sur le routeur ou une passerelle du domicile, pas sur le microcontrôleur.

Le CORE ↔ MAIN passe par Zigbee via C6-ZIGBEE; C6-WIFI héberge l'inventaire et accueillera l'API mobile. Le lien V5 `MAIN_LIGHTING` ↔ `LAMP_C6` reste intact. Les rôles Wi-Fi et Zigbee sont séparés sur deux C6; le VPN privé relie le téléphone au réseau domestique sans transporter les messages CORE ↔ MAIN.

L'application utilise le même contrat localement et via VPN. L'authentification de l'API sur le réseau local reste à définir avant d'exposer le serveur; aucun secret ni service cloud n'est inclus dans cette étape.

## Étapes V7

1. [x] Instancier l'inventaire générique sur CORE et ajouter une entrée CORE racine.
2. [x] Ajouter l'ingestion idempotente d'une annonce MAIN dans le registre CORE.
3. [x] Calculer des IDs CORE stables à partir du parent et de l'ID local, avec détection de collision.
4. [ ] Relier les annonces MAIN au CORE sur le transport retenu; l'implémentation et les tests natifs existent. Les essais matériels confirment la réception Zigbee par C6-ZIGBEE, mais pas encore le cycle complet d'attribution/retour d'ID via C6-WIFI.
5. [ ] Lire l'état générique d'un module depuis le MAIN qui le gère.
6. [ ] Router `power.set` vers le chemin de commande V5 existant.
7. [ ] Exposer les trois opérations à l'application mobile via l'adaptateur réseau choisi.
8. [ ] Valider l'accès distant par VPN privé et conserver la même API.

## Validation matérielle provisoire

- C6-WIFI démarre le pilote Wi-Fi et initialise son UART.
- C6-ZIGBEE reçoit l'ACK UART de C6-WIFI (`UART peer ready`).
- MAIN_LIGHTING rejoint le PAN; les annonces V7 du MAIN sont visibles côté C6-ZIGBEE.
- Le journal matériel n'a pas encore confirmé l'ID calculé sur C6-WIFI ni son retour visible sur MAIN (`[V7] MAIN CORE id=...`). Le routage des quatre modules, l'absence de doublon et le rejet d'identités incohérentes restent à vérifier sur le matériel.
