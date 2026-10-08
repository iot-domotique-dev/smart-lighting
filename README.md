# Smart Lighting

Projet de modules d’éclairage ESP32. V7.1 fournit un CORE logique composé de deux ESP32-C6 et un inventaire hiérarchique en lecture seule. V7.2 valide le trajet SET_POWER entre les quatre cartes; V7.3 expose maintenant cette commande par l’API HTTP locale authentifiée.

## État actuel

- **V5 éclairage :** MAIN_LIGHTING communique avec LAMP_C6 par Zigbee; la commande power pilote la sortie GPIO18 de la lampe.
- **V7.1.1 réseau/API :** CORE-WIFI rejoint le Wi-Fi avec DHCP. Depuis un PC du même réseau, les routes HTTP ont répondu comme prévu : 200 pour health, core, modules et devices; 404 pour un ID inconnu; 405 pour POST sur une route en GET seulement.
- **V7.1 validée sur matériel :** MAIN_LIGHTING et lamp001 apparaissent en ligne dans l’API, avec leurs IDs V7 et leurs parents. Les annonces et l’attribution d’ID ont été observées entre les quatre cartes.
- **V7.2 validée sur matériel :** SET_POWER fonctionne de CORE-WIFI jusqu’à LAMP, avec ACK d’acceptation et d’exécution, retransmission, déduplication et expiration lorsque le retour d’ACK est coupé. Les 85 tests natifs passent et les quatre profils matériels compilent. Le [dossier V7.2](docs/V7_2_COMMANDS.md) consigne les résultats, la procédure et les limites.
- **V7.3 validée fonctionnellement :** l’API HTTP authentifiée de SET_POWER fonctionne sur les quatre cartes; ON/OFF, le suivi terminal, les refus d’accès, les corps invalides, les destinations inconnues et les cibles offline ont des résultats consignés. L’utilisateur confirme aussi la reprise et l’expiration; l’image utilise 994 816 octets sur 1 Mio et laisse 53 760 octets.

Le jeton de l’API de commande doit être défini dans le fichier local `include/wifi_credentials.h`; sans jeton valide, les routes de commande restent désactivées. Vérifier la marge flash CORE-WIFI après compilation avant flash matériel.

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

Le CORE garde l’ID racine 1. L’application ne communique pas directement avec Zigbee. Le VPN distant et l’application mobile restent à réaliser.

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
