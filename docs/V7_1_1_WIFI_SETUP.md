# V7.1.1 — Connexion Wi-Fi de CORE-WIFI

V7.1.1 ajoute la connexion de CORE-WIFI au réseau local par DHCP. Le serveur HTTP démarre même si les credentials manquent ou si le point d'accès est momentanément indisponible. Le firmware retente automatiquement la connexion toutes les cinq secondes.

## Configurer SSID et mot de passe

Depuis PowerShell, à la racine du dépôt :

```powershell
Copy-Item include/wifi_credentials.example.h include/wifi_credentials.h
notepad include/wifi_credentials.h
```

Remplace ensuite les deux valeurs par celles du réseau local :

```cpp
#define SMART_LIGHTING_WIFI_SSID "NomDuReseau"
#define SMART_LIGHTING_WIFI_PASSWORD "MotDePasseDuReseau"
```

Enregistre le fichier. `include/wifi_credentials.h` est ignoré par Git. Ne colle pas les credentials dans le fichier `.example.h`, dans `platformio.ini`, ou dans un fichier source versionné. Le mot de passe n'est jamais écrit dans les logs série.

Le mot de passe peut être vide pour un réseau ouvert. L'ESP32-C6 utilise le Wi-Fi 2,4 GHz. Assure-toi que le réseau ne bloque pas les clients Wi-Fi entre eux.

## Compiler et flasher

Remplace `COMx` par le port qui correspond au C6-WIFI. Repère-le en débranchant/rebranchant la carte puis en exécutant `pio device list`.

```powershell
pio run -e core_wifi_c6
pio run -e core_wifi_c6 -t upload --upload-port COMx
pio device monitor -p COMx -b 115200
```

Par exemple, si le CORE-WIFI apparaît sur `COM9`, utilise `COM9` dans les deux dernières commandes. Ferme le moniteur série avant l'upload si PlatformIO indique que le port est déjà ouvert.

Pour modifier les credentials plus tard, édite `include/wifi_credentials.h`, puis recompile et reflashe avec les mêmes commandes.

## Logs série attendus

La ligne de démarrage de l'API doit apparaître même si aucun réseau ne répond :

```text
[CORE-WIFI] HTTP API: ready on port 80
```

Avec un fichier de credentials valide, le cycle normal ressemble à ceci :

```text
[CORE-WIFI] Wi-Fi station ready; waiting for credentials
[CORE-WIFI] Wi-Fi connection attempt, SSID: NomDuReseau
[CORE-WIFI] Wi-Fi connected, SSID: NomDuReseau
[CORE-WIFI] IP address: 192.168.1.42
[CORE-WIFI] HTTP API: ready on port 80
```

L'ordre exact des lignes API et connexion peut varier. Si le réseau disparaît, la console indique `Wi-Fi connection lost (reason=...)`, puis `Wi-Fi reconnect attempt` avec le SSID. Le mot de passe n'est jamais affiché.

Sans credentials, la console indique `Wi-Fi credentials not configured`; le serveur est démarré mais aucun client ne peut encore le joindre par Wi-Fi.

## Retrouver l'adresse IP

Lis `IP address` dans le moniteur série après `Wi-Fi connected`. L'adresse provient de DHCP. Tu peux aussi la retrouver dans la page des clients DHCP de ton routeur, en repérant l'ESP32-C6 ou son adresse MAC.

Depuis PowerShell, définis l'adresse obtenue, sans `http://` :

```powershell
$CoreIp = "192.168.1.42"
```

Puis interroge l'API avec `curl.exe` :

```powershell
curl.exe -i "http://$CoreIp/api/v1/health"
curl.exe -i "http://$CoreIp/api/v1/core"
curl.exe -i "http://$CoreIp/api/v1/modules"
curl.exe -i "http://$CoreIp/api/v1/devices"
```

Les réponses HTTP doivent commencer par `200 OK` et avoir `Content-Type: application/json`. Une liste vide est valide si aucun MAIN ou appareil n'a encore été inscrit dans le registre.

L'API reste en HTTP local sans authentification. Ne redirige pas le port 80 du routeur vers Internet et n'utilise pas de réseau Wi-Fi invité qui isole les clients.
