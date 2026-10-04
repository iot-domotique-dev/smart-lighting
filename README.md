# Smart Lighting

Projet de modules d’éclairage ESP32. La référence terrain actuelle est le lien Zigbee V5 entre MAIN_LIGHTING et LAMP_C6. Le jalon V7 prépare un CORE logique composé de deux ESP32-C6 et une API locale pour une future application.

## État actuel

- **V5 éclairage :** MAIN_LIGHTING communique avec LAMP_C6 par Zigbee; la commande power pilote la sortie GPIO18 de la lampe.
- **V7.1.1 réseau/API :** CORE-WIFI rejoint le Wi-Fi avec DHCP. Depuis un PC du même réseau, les routes HTTP ont répondu comme prévu : 200 pour health, core, modules et devices; 404 pour un ID inconnu; 405 pour POST sur une route en GET seulement.
- **Encore à valider sur le matériel :** le trajet complet des annonces et de l’attribution d’ID entre les quatre cartes CORE-WIFI, CORE-ZIGBEE, MAIN_LIGHTING et LAMP_C6. Les listes API actuellement vides sont cohérentes tant que le registre CORE n’a pas reçu les annonces.

La validation complète V7 reste en attente du test matériel avec les quatre cartes réunies.

## Architecture

~~~text
Application mobile (future)
             │ HTTP local /api/v1
             ▼
   C6-WIFI — registre CORE, IDs, Wi-Fi, API
             │ UART
   C6-ZIGBEE — transport Zigbee, annonces et états
             │ un PAN Zigbee
             ▼
 MAIN_LIGHTING ─── Zigbee V5 ─── LAMP_C6
~~~

Le CORE garde l’ID racine 1. L’application ne communique pas directement avec Zigbee. Le VPN distant et les commandes API ne sont pas implémentés à cette étape.

## Profils PlatformIO maintenus

- **core_wifi_c6**
- **core_zigbee_c6**
- **main_light_c6**
- **lamp_c6**
- **native** pour les tests hôte

Les anciens environnements génériques **core**, **main** et **relay** restent déclarés pour compatibilité et ne remplacent pas les profils matériels C6.

## Compiler et tester

Depuis la racine du dépôt, avec PlatformIO :

~~~powershell
pio run -e core_wifi_c6 -e core_zigbee_c6 -e main_light_c6 -e lamp_c6
pio test -e native
~~~

Pour créer les credentials Wi-Fi, flasher CORE-WIFI et interroger l’API, suivre le [guide Wi-Fi](docs/V7_1_1_WIFI_SETUP.md). Les contrats et limites de l’API sont décrits dans [API CORE V7.1](docs/V7_1_CORE_API.md).

## Documentation

L’[index des documents](docs/README.md) mène aux références V5 et V7. Les anciens documents de versions sont conservés dans [docs/archive](docs/archive/README.md); ils décrivent l’historique et ne sont pas les instructions courantes.
