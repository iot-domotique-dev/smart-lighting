# V7.1 — API locale du CORE-WIFI

## Portée et statut

V7.1 expose en lecture seule l’état du CORE et l’inventaire qu’il a reçu. Le serveur HTTP utilise le composant ESP-IDF esp_http_server et n’est compilé que pour core_wifi_c6. Aucune application mobile, route de commande, base de données, VPN ou service cloud n’est inclus.

**Le Wi-Fi et les requêtes HTTP ont été vérifiés sur la carte CORE-WIFI. La validation matérielle complète V7 reste en attente** : les modules et appareils retournés par l’API dépendront du registre effectivement alimenté par la chaîne Zigbee et UART.

## Architecture

~~~text
Client mobile ou Web (futur)
          │ HTTP /api/v1
          ▼
 C6-WIFI : API + registre CORE
          │ UART
 C6-ZIGBEE : transport Zigbee
          │ un PAN
          ▼
       MAINs
~~~

Le client appelle uniquement C6-WIFI. Le JSON est construit à partir du registre; les structures C++ internes ne sont pas exposées. L’API est indépendante du transport Zigbee.

## Conventions

- Les routes décrites ci-dessous acceptent GET et renvoient du JSON UTF-8.
- Les IDs sont des entiers non signés sur 32 bits. L’ID racine est 1; les IDs des modules sont calculés selon le contrat {parentId, localId}.
- modules liste les MAIN connus; devices liste leurs appareils enfants. Le CORE n’est inclus dans aucune liste.
- online et status décrivent la présence enregistrée, pas l’état électrique d’une lampe.
- state vaut null tant qu’un état métier tel que power n’a pas été transmis au registre CORE.
- Une collection vide renvoie 200 avec un tableau vide et count: 0.
- Les paramètres de requête ne sont pas pris en charge.

## Routes

### GET /api/v1/health

État de préparation et indicateurs de transport.

~~~json
{
  "status": "ok",
  "core_id": 1,
  "firmware_version": "...",
  "uptime_ms": 926638,
  "wifi_connected": true,
  "uart_driver_ready": true
}
~~~

uart_driver_ready confirme l’initialisation du pilote local, pas le fonctionnement de toute la chaîne Zigbee/UART.

### GET /api/v1/core

Identité et état du CORE.

~~~json
{
  "core": {
    "id": 1,
    "name": "CORE",
    "role": "core",
    "status": "online",
    "online": true,
    "api_version": "v1",
    "firmware_version": "...",
    "uptime_ms": 949575,
    "wifi_connected": true,
    "uart_driver_ready": true
  }
}
~~~

### GET /api/v1/modules

Liste les MAIN connus.

~~~json
{
  "modules": [],
  "count": 0
}
~~~

### GET /api/v1/modules/{id}

Retourne le MAIN correspondant sous la clé module. Un ID inconnu ou d’un autre type renvoie 404.

~~~json
{
  "module": {
    "id": 3081691923,
    "name": "MAIN_LIGHTING",
    "role": "main",
    "online": true,
    "status": "online",
    "parent_id": 1,
    "capabilities": ["lighting"],
    "last_seen_ms": 123456,
    "state": null
  }
}
~~~

### GET /api/v1/devices

Liste les appareils connus sous les MAIN; le CORE et les MAIN sont exclus.

~~~json
{
  "devices": [],
  "count": 0
}
~~~

### GET /api/v1/devices/{id}

Retourne l’appareil correspondant sous la clé device. Un ID inconnu ou d’un autre type renvoie 404.

~~~json
{
  "device": {
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
}
~~~

Les capacités indiquent les fonctions déclarées, mais ne promettent pas une route de commande. Les champs des exemples sont illustratifs; les valeurs renvoyées viennent du firmware et du registre.

## Erreurs

Les erreurs partagent cette forme :

~~~json
{
  "error": {
    "code": "not_found",
    "message": "The requested resource was not found."
  }
}
~~~

| HTTP | Code | Cas |
| --- | --- | --- |
| 400 | bad_request | ID nul, mal formé ou hors plage; paramètre de requête inconnu. |
| 404 | not_found | Route ou ressource inconnue; ID absent ou d’un type différent. |
| 405 | method_not_allowed | Méthode autre que GET sur une route connue; Allow: GET. |
| 500 | internal_error | Registre incohérent ou sérialisation impossible. |
| 503 | core_not_ready | Registre racine CORE absent ou invalide. |

## Sécurité et limites

- Le serveur utilise HTTP local sans authentification ni chiffrement applicatif. Ne pas exposer le port 80 sur Internet.
- Le VPN, l’authentification et la sécurisation d’un accès distant sont hors périmètre.
- Les états électriques ne sont pas encore disponibles dans le registre global; state: null l’indique.
- Le firmware CORE-WIFI a été mesuré à **978 900 octets sur 1 048 576 (93,4 %)** pour la partition application, soit environ 69 676 octets libres. La marge est limitée; toute nouvelle dépendance ou fonctionnalité doit être mesurée.
- Le champ firmware_version observé lors de l’essai affichait v7.0.0-dirty; il s’agit de la métadonnée de build embarquée, distincte du jalon fonctionnel documenté ici.

## Résultat du test réseau sur carte

Depuis un PC connecté au même Wi-Fi que CORE-WIFI :

- le port TCP 80 était joignable;
- /health, /core, /modules et /devices ont répondu 200;
- /modules et /devices ont renvoyé des listes vides, car aucune entrée n’était encore présente dans le registre;
- les recherches d’IDs inconnus dans /modules/{id} et /devices/{id} ont répondu 404;
- POST sur /api/v1/core a répondu 405 et annoncé Allow: GET.

Ces résultats valident le démarrage Wi-Fi/HTTP et le contrat réseau en lecture. Ils ne valident pas le parcours d’annonces ni l’attribution d’ID V7 entre les quatre cartes. Voir le suivi du [CORE double C6](V7_CORE_DOUBLE_C6.md).
