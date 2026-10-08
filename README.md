# Smart Lighting

Projet de modules d’éclairage ESP32. V7.1 fournit un CORE logique composé de deux ESP32-C6. V7.2 valide le trajet `SET_POWER`, V7.3 expose sa commande via HTTP local authentifié et V7.4 ajoute la validation native multi-lampes puis l’historique logiciel des états confirmés.

## État actuel

- **V5 éclairage :** MAIN_LIGHTING communique avec LAMP_C6 par Zigbee; la commande power pilote la sortie GPIO18 de la lampe.
- **V7.1.1 réseau/API :** CORE-WIFI rejoint le Wi-Fi avec DHCP. Depuis un PC du même réseau, les routes HTTP ont répondu comme prévu : 200 pour health, core, modules et devices; 404 pour un ID inconnu; 405 pour POST sur une route en GET seulement.
- **V7.1 validée sur matériel :** MAIN_LIGHTING et lamp001 apparaissent en ligne dans l’API, avec leurs IDs V7 et leurs parents. Les annonces et l’attribution d’ID ont été observées entre les quatre cartes.
- **V7.2 validée sur matériel :** `SET_POWER` fonctionne de CORE-WIFI jusqu’à LAMP, avec ACK d’acceptation et d’exécution, retransmission, déduplication et expiration lorsque le retour d’ACK est coupé. Les 85 tests natifs passaient à cette étape; voir le [dossier V7.2](docs/V7_2_COMMANDS.md).
- **V7.3 validée fonctionnellement :** l’API HTTP authentifiée de `SET_POWER` fonctionne sur les quatre cartes; ON/OFF, le suivi terminal et les erreurs HTTP sont consignés dans [la documentation V7.3](docs/V7_3_HTTP_COMMANDS.md). Son empreinte de 994 816 octets est une mesure historique de cette révision.
- **V7.4.1 validée en natif :** un scénario couvre le routage indépendant de deux lampes derrière le même MAIN; 86/86 tests passaient à cette étape. Le banc matériel utilisé ensuite ne comporte qu’une LAMP, donc le routage de deux lampes n’a pas été vérifié physiquement.
- **V7.4.2 validée fonctionnellement sur matériel :** 92/92 tests natifs réussis et 4/4 profils C6 compilés. ON/OFF, ACK `executed`, cohérence HTTP, `confirmed → stale` hors ligne, reconnexion, expiration et `execution_unknown` ont été observés. `last_confirmed_state` est un historique logiciel, pas une mesure électrique; il redevient `unknown` au redémarrage du CORE-WIFI. Détails et IDs figurent dans le [dossier V7.4](docs/V7_4_RELIABILITY.md). L’image CORE-WIFI utilise 995 810 octets sur 1 Mio et laisse 52 766 octets.

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
