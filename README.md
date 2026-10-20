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
- **V7.5.1/V7.5.2 — application mobile :** l’application Android Expo SDK 57 consulte CORE-WIFI et pilote les lampes en ON/OFF via l’API existante. Le premier essai nominal a été confirmé par l’utilisateur avec une LED de test raccordée à GPIO18 d’une LAMP_C6. Le bilan logiciel et les limites matérielles restantes sont dans le [dossier V7.5](docs/V7_5_MOBILE_APP.md).
- **V7.6.4 — ajout depuis l’application mobile :** le parcours nominal d’ajout a été confirmé sur matériel avec un MAIN et une LAMP. Le correctif de routage des annonces MAIN→CORE-WIFI a également été exercé dans ce parcours; le multi-MAIN et plusieurs lampes physiques restent hors de cette validation. Voir le [dossier V7.6](docs/V7_6_PAIRING.md).
- **V7.7.2/V7.7.3 — parcours nominal Android validé sur matériel le 15 octobre 2026 :** découverte, nommage, appairage, commandes ON/OFF, retrait depuis l’application, réappairage avec un nouvel ID V7 et persistance après redémarrage. La validation porte sur un MAIN et une LAMP. Les 119 tests natifs firmware et 56 tests mobiles simulés, le lint, le typage et l’export Android avaient réussi. Les essais de concurrence, multi-lampes simultané et de récupération après panne restent à faire. Voir le [dossier V7.7](docs/V7_7_UNPAIRING.md).
- **V7.8.0 — luminosité validée fonctionnellement sur matériel le 16 octobre 2026 :** depuis Android, la commande de 0 à 100 % traverse HTTP, UART et Zigbee et pilote la LED témoin physique sur GPIO18 de LAMP_C6. Le profil unique `lamp_c6` utilise LEDC et conserve ON/OFF. Les fonctions V7.7 d’appairage, retrait et réappairage restent dans le périmètre logiciel; leur validation matérielle antérieure du 15 octobre est documentée séparément. Le banc V7.8 utilise un MAIN et une LAMP. À cette version, le dernier niveau non nul n’était pas persisté en NVS; V7.9.2 ajoute cette persistance. Le multi-lampes simultané et les scénarios de panne/récupération ne sont pas validés. Voir [le dossier V7.8 et le complément V7.9.2](docs/V7_8_BRIGHTNESS.md).
- **V7.9.2 — persistance du dernier niveau LAMP :** validation matérielle nominale confirmée le 19 octobre 2026 : association conservée, réglage à 35 %, LED éteinte au redémarrage puis niveau restauré par ON, sans réappairage. Les tests natifs ciblés réussissent (14/14 tests de stabilisation et 31/31 tests d’appairage); les quatre profils C6 compilent. Les coupures pendant une écriture NVS et les scénarios de panne ne sont pas couverts. Voir [le complément V7.9.2](docs/V7_8_BRIGHTNESS.md#v792--persistance-du-dernier-niveau).
- **V7.9.5 — OFF prioritaire et retrait pendant un fade :** 157/157 tests natifs, 72/72 tests mobiles, TypeScript, lint, export Android Expo 57 et les quatre profils firmware compilent. OFF annule un fade; le retrait force la sortie à LOW avant d’écrire son reçu. La validation matérielle de ces nouveaux parcours reste à faire; le test natif ne couvre pas directement la transaction `UNPAIR_REQUEST` pendant le fade. Détails, marges mémoire et limites dans [le dossier V7.9.5](docs/V7_9_5_OFF_PRIORITY.md).

Le jeton de l’API de commande doit être défini dans le fichier local `include/wifi_credentials.h`; sans jeton valide, les routes de commande restent désactivées. Vérifier la marge flash CORE-WIFI après compilation avant flash matériel.

## Architecture

~~~text
Application mobile (V7.8.0; retrait API V7.7.2)
             │ HTTP local /api/v1
             ▼
   C6-WIFI — registre CORE, IDs, Wi-Fi, API
             │ UART
   C6-ZIGBEE — transport Zigbee, annonces et états
             │ un PAN Zigbee
             ▼
 MAIN_LIGHTING ─── Zigbee V5 ─── LAMP_C6
~~~

Le CORE garde l’ID racine 1. L’application ne communique pas directement avec Zigbee. Depuis V7.7.3, elle utilise l’API de retrait V7.7.2 avec suivi réconciliable. L’accès distant sécurisé et le support multi-MAIN matériel restent à réaliser.

## Profils PlatformIO maintenus

- **core_wifi_c6**
- **core_zigbee_c6**
- **main_light_c6**
- **lamp_c6** — GPIO18 avec PWM LEDC pour le prototype LED basse tension, ON/OFF et luminosité; ne pas utiliser pour une charge secteur ou de puissance
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

L’[index des documents](docs/README.md) mène aux références V5 et V7, dont le [dossier de l’application mobile](docs/V7_5_MOBILE_APP.md). Les anciens documents de versions sont conservés dans [docs/archive](docs/archive/README.md); ils décrivent l’historique et ne sont pas les instructions courantes.
