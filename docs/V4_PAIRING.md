# V4 — Provisioning MAIN ↔ LAMP

## Portée

La V4 ajoute la découverte et l'affectation explicite des LAMP par un MAIN. Elle
conserve les modules d'éclairage V3.9, `Communication`, l'interface
`CommunicationTransportInterface` et `SimulationTransport`. Le transport
Zigbee réel, la caméra, la sécurité, l'application mobile, le CORE complet et
les sorties de puissance restent hors de cette étape.

```text
MAIN_LIGHT
    ├── LAMP_001
    ├── LAMP_002
    └── ...
```

Le modèle de provisioning est dans `include/provisioning.h` et
`src/provisioning.cpp`. Les messages de provisioning ont un codec vers le type
`Message` existant, afin de pouvoir les transporter avec l'abstraction actuelle.
Le codec ne met pas en œuvre une radio et n'active pas `ZigbeeTransport`.

## Identité

Chaque LAMP distingue les informations suivantes :

| Champ | Rôle |
| --- | --- |
| `hardwareId` | Identité technique permanente, utilisée pour reconnaître l'équipement et détecter les doublons. |
| `deviceId` | Identifiant logique attribué par le MAIN dans la maison. Il reste distinct du hardware ID. |
| `name` | Nom utilisateur fourni par le MAIN pendant l'affectation. Le firmware reste générique. |
| `role` | Rôle logiciel `LAMP`, séparé des deux identifiants. |
| `parentMainId` | Identifiant du MAIN auquel l'équipement est affecté. |
| `pairingState` | État `UNPAIRED`, `PAIRING` ou `PAIRED`. |

Le MAIN attribue le plus petit `deviceId` disponible dans son registre, puis
vérifie à la fois cet identifiant logique et le hardware ID lors de l'ajout. Le
registre fournit les recherches par `deviceId`, `hardwareId` et `parentMainId`,
ainsi que l'ajout et la suppression. Une identité matérielle déjà présente ne
peut pas être ajoutée une seconde fois.

## Découverte et appartenance

Une annonce indique qu'une LAMP est visible; elle ne l'ajoute pas à la liste des
LAMP affectées au MAIN. Le registre ne reçoit l'appareil qu'après un
`PAIR_CONFIRM`. Plusieurs MAIN voisins peuvent donc détecter et mémoriser la même
annonce sans prendre possession de l'appareil.

Le MAIN doit appeler `startCommissioning()` pour autoriser une nouvelle
affectation. La LAMP verrouille en mémoire le premier `parentMainId` qui lance
une demande pendant cette opération. Les demandes concurrentes venant d'un
autre MAIN sont rejetées. Une LAMP déjà `PAIRED` ne peut pas être réaffectée par
ce flux; une procédure explicite d'unpair/remplacement n'est pas encore
implémentée.

## Séquence de pairing

1. La LAMP envoie `DEVICE_ANNOUNCE` avec son `hardwareId`, son rôle, sa version
   firmware, ses capacités et son état de pairing.
2. Chaque MAIN à portée peut détecter l'annonce. La découverte seule ne crée
   aucune appartenance.
3. Un MAIN ouvre le commissioning explicitement.
4. Ce MAIN envoie `PAIR_REQUEST` avec le hardware ID et son identifiant.
5. La LAMP entre en `PAIRING` en mémoire et verrouille cette demande.
6. Le MAIN attribue le `deviceId`, le `parentMainId` et un nom initial, puis
   envoie `PAIR_ACCEPT`.
7. La LAMP sauvegarde la configuration et passe à `PAIRED`.
8. La LAMP envoie `PAIR_CONFIRM` avec l'identité acceptée.
9. Le MAIN ajoute la LAMP au registre seulement après validation de la
   confirmation.

Les types `DEVICE_ANNOUNCE`, `PAIR_REQUEST`, `PAIR_ACCEPT`, `PAIR_CONFIRM` et
`PAIR_REJECT` sont disponibles dans le protocole. La réception d'un message de
provisioning par le routeur V3.9 ne le traite pas comme une commande
d'éclairage.

## Redémarrage et persistance

`ProvisioningStore` définit `load`, `save` et `clear`. `MemoryProvisioningStore`
est une implémentation de test qui représente le stockage d'une LAMP. Le record
contient `deviceId`, `hardwareId`, `name`, `parentMainId` et `pairingState`.
Après redémarrage, la LAMP recharge son record et annonce l'identité déjà
affectée. Le MAIN peut alors reconstruire son registre à partir de cette annonce
avec le même `deviceId`, ou reconnaître l'entrée déjà présente. Aucun nouvel ID
n'est attribué. Une implémentation NVS/Preferences est à écrire pour le matériel.

## Simulation et matériel restant

Les tests natifs exercent les transitions d'état et transportent une annonce
encodée dans `SimulationTransport`. Cela valide le modèle logiciel, le codec et
la déduplication dans le registre; cela ne valide ni une liaison radio ni
Zigbee. Le test pilote directement le modèle; `src/main.cpp` n'appelle pas
encore le dispatcher de provisioning au démarrage ou dans sa boucle. Cette
intégration applicative reste à faire avec le choix de la carte. `ZigbeeTransport`
reste son abstraction préparée, non implémentée.

Les environnements PlatformIO ESP32 existants restent inchangés. Aucun
identifiant de carte ESP32-C6 précis n'est présent dans le dépôt, donc aucun
environnement `main_c6` ou `lamp_c6` n'est déclaré. Après sélection du modèle de
carte, il faudra documenter son board ID, ajouter les deux environnements,
intégrer NVS/Preferences et la stack Zigbee, puis compiler et tester les deux
cartes physiques. La compilation d'un profil ESP32 existant ne constitue pas une
validation C6.
