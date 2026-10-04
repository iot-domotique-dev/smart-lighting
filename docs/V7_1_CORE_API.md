# V7.1 — API locale du CORE-WIFI

## Objectif et statut

V7.1 prépare l’API locale du CORE-WIFI. La validation matérielle complète V7 reste en attente.

Cette version fournit un accès HTTP en lecture à l’état du CORE et à l’inventaire déjà connu de son registre. Elle prépare l’interface pour une future application mobile ou Web, sans implémenter cette application ni les commandes d’éclairage.

## Architecture

```text
Application mobile / interface Web (future)
                 │ HTTP local, /api/v1
                 ▼
       CORE-WIFI — serveur HTTP
                 │
                 ▼
         CORE logic / Registry
                 │ UART interne
                 ▼
       CORE-ZIGBEE — transport Zigbee
                 │ Zigbee
                 ▼
              MAINs
```

L’application appelle uniquement CORE-WIFI. Elle ne se connecte ni directement au réseau Zigbee ni à CORE-ZIGBEE. Le service HTTP utilise un modèle JSON explicite dérivé du registre; il n’expose pas les structures C++ internes. Le routage Zigbee ne fait pas partie de l’API HTTP.

Le serveur s’appuie sur le composant `esp_http_server` fourni par ESP-IDF, sans nouvelle bibliothèque externe. Il est compilé uniquement dans le profil `core_wifi_c6`.

## Conventions

- Préfixe et version : `/api/v1`.
- Les routes présentes dans cette version acceptent uniquement `GET`.
- Les réponses sont en JSON UTF-8 avec `Content-Type: application/json; charset=utf-8`.
- Les valeurs `firmware_version`, IDs et états montrés dans les exemples illustrent la forme de réponse; la version du firmware est lue dans les métadonnées de l'image compilée.
- Les IDs sont des entiers non signés 32 bits JSON. Ils suivent le contrat V7 : CORE racine `1`, IDs dérivés de `{parentId, localId}`, stables dans un CORE et non globaux entre plusieurs foyers.
- `modules` désigne les MAIN connus du registre. `devices` désigne leurs appareils enfants connus. CORE lui-même n’apparaît dans aucune de ces deux listes.
- `status` et `online` représentent la présence générique enregistrée (`online` ou `offline`), pas l’état électrique d’une lampe.
- `state` vaut `null` tant que l’état métier de l’appareil (par exemple `power`) n’est pas disponible dans le registre CORE. L’API ne déduit pas cet état des capacités ou des annonces.
- Les collections sans entrée renvoient `200` avec une liste vide et `count: 0`.
- Les paramètres de requête ne sont pas encore pris en charge.

## Endpoints

### `GET /api/v1/health`

Vérifie que le registre CORE est prêt et retourne des indicateurs de fonctionnement.

```json
{
  "status": "ok",
  "core_id": 1,
  "firmware_version": "...",
  "uptime_ms": 123456,
  "wifi_connected": true,
  "uart_driver_ready": true
}
```

`wifi_connected` indique l’état rapporté par le transport Wi-Fi; `uart_driver_ready` indique que le transport UART est initialisé. Ces indicateurs ne certifient pas à eux seuls que la chaîne Zigbee ou les MAIN sont opérationnels.

### `GET /api/v1/core`

Retourne l’identité racine et l’état logiciel du CORE.

```json
{
  "core": {
    "id": 1,
    "name": "CORE",
    "role": "core",
    "status": "online",
    "online": true,
    "api_version": "v1",
    "firmware_version": "...",
    "uptime_ms": 123456,
    "wifi_connected": true,
    "uart_driver_ready": true
  }
}
```

### `GET /api/v1/modules`

Retourne les MAIN connus, avec leur nombre.

### `GET /api/v1/modules/{id}`

Retourne un MAIN connu. Un ID existant d’un autre type n’est pas un module et renvoie `404`.

```json
{
  "modules": [
    {
      "id": 3081691923,
      "name": "MAIN_LIGHTING",
      "role": "main",
      "online": true,
      "status": "online",
      "parent_id": 1,
      "capabilities": ["lighting", "groups", "scenes"],
      "last_seen_ms": 123456,
      "state": null
    }
  ],
  "count": 1
}
```

La route détail enveloppe le même objet sous la clé `module` : `{"module": { ... }}`.

### `GET /api/v1/devices`

Retourne les appareils enfants connus, avec leur nombre. CORE et les MAIN sont exclus.

### `GET /api/v1/devices/{id}`

Retourne un appareil connu. Un ID existant d’un autre type n’est pas un appareil et renvoie `404`.

```json
{
  "devices": [
    {
      "id": 2655085312,
      "name": "LAMP_C6",
      "role": "lamp",
      "online": true,
      "status": "online",
      "parent_id": 3081691923,
      "capabilities": ["power", "brightness"],
      "last_seen_ms": 120000,
      "state": null
    }
  ],
  "count": 1
}
```

La route détail enveloppe le même objet sous la clé `device` : `{"device": { ... }}`. Les noms de capacités sont ceux connus du registre; leur présence ne garantit pas encore qu’une commande correspondante soit routable par cette API.

## Erreurs HTTP

Les erreurs ont le format commun suivant :

```json
{
  "error": {
    "code": "not_found",
    "message": "The requested resource was not found."
  }
}
```

| HTTP | Code JSON | Cas |
| --- | --- | --- |
| `400 Bad Request` | `bad_request` | ID mal formé, hors plage ou nul; paramètres de requête non pris en charge. |
| `404 Not Found` | `not_found` | Route ou ressource inconnue; ID qui n’est pas du type demandé. |
| `405 Method Not Allowed` | `method_not_allowed` | Méthode autre que GET sur une route connue. L’en-tête `Allow: GET` est fourni. |
| `500 Internal Server Error` | `internal_error` | Registre incohérent ou réponse impossible à sérialiser. |
| `503 Service Unavailable` | `core_not_ready` | Racine CORE absente ou invalide, donc registre non prêt. |

Le sérialiseur contrôle les chaînes JSON, la taille de sortie et les IDs non signés. Une collision ou identité incohérente rejetée lors de l’ingestion ne doit pas apparaître comme une entrée distincte dans l’inventaire.

## Sécurité et limites

- L’API est en HTTP local sans authentification ni chiffrement applicatif dans cette version. Elle ne doit pas être exposée directement à Internet. VPN, terminaison d’accès distant et authentification restent hors périmètre.
- La station Wi-Fi rejoint le réseau avec les credentials locaux décrits dans [`V7_1_1_WIFI_SETUP.md`](V7_1_1_WIFI_SETUP.md). Sans credentials, le serveur HTTP démarre mais ne sera pas joignable depuis le réseau.
- Toutes les routes sont en lecture seule. Il n’existe aucune route de commande d’éclairage, de provisioning ou de configuration.
- Les états électriques réels ne sont pas encore remontés jusqu’au registre exposé; `state: null` le signale explicitement.
- Le serveur tourne uniquement sur CORE-WIFI. CORE-ZIGBEE ne lance aucun serveur HTTP.
- La capacité de partition doit être surveillée : sur la configuration actuelle, CORE-WIFI utilise 976 464 octets sur 1 048 576 (93,1 %), contre 908 660 octets (86,7 %) avant l’ajout de l’API, soit +67 804 octets. Il reste environ 72 KiB dans la partition applicative. Une évolution importante nécessitera de reconsidérer la table des partitions ou la taille des fonctionnalités.
- Aucun test HTTP réseau sur carte n’a encore été exécuté. Les réponses et routes sont testées nativement; le démarrage réel du serveur, l’accès depuis le Wi-Fi et les réponses vues par un client restent à vérifier sur matériel.
- La validation matérielle complète V7, comprenant attribution/retour d’ID et chaîne des quatre modules, reste en attente. L’API reflète uniquement les entrées effectivement présentes dans le registre.

## Suite prévue

1. Vérifier sur carte le démarrage de CORE-WIFI et interroger chaque endpoint depuis un client du réseau local.
2. Avec les cartes disponibles, valider le parcours des annonces et IDs V7 jusqu’au registre CORE-WIFI; répéter après réception des cartes restantes.
3. Ajouter le transport de l’état métier lorsqu’il est disponible depuis le MAIN et vérifier que l’API le présente sans en déduire la valeur.
4. Concevoir ultérieurement les commandes avec accusé d’acceptation et état final, après validation de la chaîne matérielle Zigbee.
5. Définir authentification et accès VPN avant toute utilisation hors du réseau local.

