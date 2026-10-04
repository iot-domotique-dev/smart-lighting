# V7 — CORE à deux ESP32-C6

Cette étape sépare le CORE logique sur deux cartes. Elle prépare les transports
et l'attribution d'identité V7 ; elle n'inclut pas le serveur de l'application.

```text
Téléphone / réseau IP
        │
   C6-WIFI (CORE = 1, registre, IDs)
        │ UART point-à-point
   C6-ZIGBEE (routeur Zigbee, sans registre global)
        │ un seul PAN Zigbee
   MAIN_LIGHTING ─── LAMP_C6
```

Le VPN relie le téléphone au réseau domestique. Il ne transporte pas les
messages CORE ↔ MAIN. Le lien Zigbee V5 MAIN_LIGHTING ↔ LAMP_C6 et ses codecs
restent inchangés.

## Profils PlatformIO

- `core_wifi_c6` : station Wi-Fi, racine du registre (`CORE_LOGICAL_ID = 1`),
  attribution des identifiants V7 et liaison UART. Il ne démarre ni Zigbee ni
  le provisioning V5.
- `core_zigbee_c6` : transport Zigbee et relais des annonces/attributions sur
  UART ; aucun registre global. Il rejoint le PAN existant en rôle routeur.
- `main_light_c6` et `lamp_c6` : profils V5 conservés.

Le protocole UART utilise des trames v1 avec synchronisation, type, numéro de
séquence, longueur explicite, payload sérialisé champ par champ et CRC-16
CCITT-FALSE. Les types comprennent HELLO, MODULE_ANNOUNCEMENT, ID_ASSIGNMENT,
ACK, STATE et ERROR. Aucune structure C++ n'est copiée directement sur le fil.

## Câblage prototype

Relier les broches TX et RX en croisé et partager la masse :

| C6-WIFI | C6-ZIGBEE |
| --- | --- |
| GPIO4 TX | GPIO5 RX |
| GPIO5 RX | GPIO4 TX |
| GND | GND |

Les valeurs par défaut sont `UART1`, GPIO4/5 et 115200 bauds. Elles sont
configurables par les drapeaux de compilation PlatformIO :
`CORE_LINK_UART_PORT`, `CORE_LINK_UART_TX_PIN`, `CORE_LINK_UART_RX_PIN` et
`CORE_LINK_UART_BAUD`. Adapter le câblage si ces valeurs changent. Ne pas relier
les sorties d'alimentation 3,3 V des deux cartes entre elles.

## Identité et flux

1. MAIN_LIGHTING diffuse son annonce locale sur Zigbee avec `parentId = 1`.
2. C6-ZIGBEE traduit l'annonce en trame UART.
3. C6-WIFI valide l'identité et met à jour le registre racine ; une répétition
   met à jour la même entrée.
4. C6-WIFI renvoie l'ID déterministe issu de `{parentId, localId}` ou une erreur.
5. C6-ZIGBEE relaie l'ID à MAIN_LIGHTING ; MAIN accuse réception. Les annonces
   V5 des lampes ne sont relayées vers le registre CORE qu'après cette
   attribution.

Les IDs sont stables dans un CORE, pas globalement uniques entre foyers. Une
collision ou une identité incohérente est rejetée. Cette version suppose un
seul PAN Zigbee.

## Limites de cette étape

Le transport Wi-Fi initialise une station, mais ne reçoit pas encore de
credentials et ne démarre aucune API. Les tests natifs couvrent le codec UART,
le flux d'attribution d'ID, les annonces répétées et les identités incohérentes.

Essais matériels provisoires : C6-WIFI initialise le Wi-Fi et l'UART;
C6-ZIGBEE reçoit son ACK UART; MAIN_LIGHTING rejoint le PAN et ses annonces
V7 sont reçues par C6-ZIGBEE. Le retour de l'ID calculé par C6-WIFI jusqu'au
MAIN, les quatre modules liés, et la validation V5 de bout en bout avec le CORE
restent à vérifier quand les cartes supplémentaires seront disponibles.
