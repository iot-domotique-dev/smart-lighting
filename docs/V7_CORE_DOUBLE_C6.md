# V7 — CORE à deux ESP32-C6

## Répartition des rôles

Le CORE est une seule entité logique répartie sur deux cartes :

~~~text
Appareil client ── Wi-Fi/IP ── C6-WIFI
                                  │ UART local
                                  ▼
MAIN_LIGHTING ── Zigbee ── C6-ZIGBEE
      │
      └────────── Zigbee V5 ── LAMP_C6
~~~

- **C6-WIFI** possède le registre global, la racine CORE ID 1, l’attribution des IDs, la station Wi-Fi et l’API HTTP.
- **C6-ZIGBEE** participe à un seul PAN, relaie les annonces et les réponses d’identité par UART. Il ne possède pas le registre global et ne lance pas HTTP.
- **MAIN_LIGHTING ↔ LAMP_C6** conserve le chemin fonctionnel V5.
- Le VPN éventuel reliera un téléphone au réseau du domicile; il ne transporte pas CORE ↔ MAIN.

## Identités et annonces

Le contrat V7 est conservé :

- La racine CORE a l’ID logique 1.
- Le CORE calcule les IDs depuis {parentId, localId}. Ils sont stables au sein d’un CORE, mais ne sont pas globaux entre foyers.
- Une annonce répétée met à jour l’entrée existante.
- Une identité incohérente ou une collision est rejetée.
- Un MAIN reçoit son ID CORE avant d’annoncer ses propres modules avec cet ID comme parent.

C6-ZIGBEE reçoit l’annonce Zigbee du MAIN et la transmet par UART à C6-WIFI. C6-WIFI valide l’identité, actualise le registre et renvoie l’attribution ou une erreur. C6-ZIGBEE transmet la réponse au MAIN.

## UART entre les C6

Le protocole utilise des trames versionnées avec synchronisation, type, séquence, longueur explicite, payload sérialisé champ par champ et CRC-16 CCITT-FALSE. Il ne transmet aucune structure C++ en mémoire. Les types couvrent notamment HELLO, MODULE_ANNOUNCEMENT, ID_ASSIGNMENT, ACK, STATE et ERROR.

Brochage prototype par défaut, TX/RX croisés et masse commune :

| C6-WIFI | C6-ZIGBEE |
| --- | --- |
| GPIO4 TX | GPIO5 RX |
| GPIO5 RX | GPIO4 TX |
| GND | GND |

Les paramètres par défaut sont UART1 et 115200 bauds. Les broches, le port et le débit sont configurables via CORE_LINK_UART_PORT, CORE_LINK_UART_TX_PIN, CORE_LINK_UART_RX_PIN et CORE_LINK_UART_BAUD dans PlatformIO. Ne pas relier les alimentations 3,3 V des cartes entre elles.

## Profils

- core_wifi_c6
- core_zigbee_c6
- main_light_c6
- lamp_c6

C6-WIFI n’active pas le provisioning Zigbee V5. Les deux profils CORE et les profils V5 restent compilés séparément.

## État des essais matériels

CORE-WIFI a rejoint le réseau local par DHCP et son API HTTP a été interrogée avec succès depuis un PC du même Wi-Fi. MAIN_LIGHTING et CORE-ZIGBEE ont également été essayés séparément sur Zigbee. Ces essais partiels ne démontrent pas encore le retour de l’ID attribué jusqu’au MAIN ni le remplissage du registre par la chaîne complète.

La validation V7 attend l’essai simultané des quatre cartes : C6-WIFI, C6-ZIGBEE, MAIN_LIGHTING et LAMP_C6. Il faudra vérifier l’attribution et le retour d’ID, une annonce répétée sans doublon, le rejet d’une identité incohérente, les listes de l’API et la conservation du lien V5. Voir les résultats HTTP détaillés dans [API CORE V7.1](V7_1_CORE_API.md).
