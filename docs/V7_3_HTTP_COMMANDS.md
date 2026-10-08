# V7.3 — Commande HTTP locale

## Portée

V7.3 ajoute une interface HTTP sur CORE-WIFI pour soumettre `SET_POWER` à une lampe et consulter le suivi V7.2 existant. La commande traverse ensuite CORE-WIFI, UART, CORE-ZIGBEE, Zigbee, MAIN et LAMP comme la commande console déjà validée.

L’implémentation compile avec le profil `core_wifi_c6`. La mesure de cette révision V7.3 était de **994 816 octets** dans une partition de **1 048 576 octets**, soit **53 760 octets libres** (94,87 % utilisés); ce chiffre est historique et ne décrit pas l’image V7.4.2. La validation fonctionnelle V7.3 sur carte est confirmée par l’utilisateur le **8 octobre 2026**. Les traces détaillées disponibles couvrent ON/OFF, le suivi et les principaux refus HTTP; les autres scénarios sont confirmés globalement par l’utilisateur.

Le serveur HTTP n’ajoute pas de tâche d’exécution de commande : la boucle CORE-WIFI conserve la responsabilité des ACK, retries et expirations. L’accès HTTP et la boucle partagent le mutex du registre afin de protéger aussi le tracker et le générateur d’IDs.

## Configuration d’accès

Les routes de lecture (`GET /api/v1/health`, `core`, `modules` et `devices`) restent accessibles comme avant. Les routes de commande exigent `Authorization: Bearer <jeton>`.

Dans le fichier local ignoré par Git `include/wifi_credentials.h`, ajouter un jeton aléatoire d’au moins 32 caractères :

```powershell
$bytes = New-Object byte[] 32
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
$rng.GetBytes($bytes)
$ApiToken = -join ($bytes | ForEach-Object { $_.ToString("x2") })
$rng.Dispose()
$ApiToken
```

Copier la valeur affichée dans la macro ci-dessous, sans partager ni committer le fichier :

```cpp
#define SMART_LIGHTING_API_TOKEN "REMPLACER_PAR_UN_JETON_ALEATOIRE_D_AU_MOINS_32_CARACTERES"
```

Le fichier [wifi_credentials.example.h](../include/wifi_credentials.example.h) laisse ce jeton vide par défaut. Tant qu’il est vide ou trop court, les routes de commande répondent `503 command_api_disabled`. Le jeton est compilé dans le firmware CORE-WIFI : ne pas le committer ni le publier. L’API utilise HTTP sans chiffrement; garder CORE-WIFI sur un réseau de confiance et ne pas rediriger le port 80 vers Internet. Un jeton Bearer protège l’accès, mais ne chiffre pas les échanges.

## Soumettre une commande

### `POST /api/v1/devices/{id}/commands/power`

`id` est l’ID V7 global de la lampe. Le corps JSON accepte `on` ou `off` :

```json
{"state":"on"}
```

Exemple PowerShell :

```powershell
$CoreIp = "IP_AFFICHEE_PAR_CORE_WIFI"
$LampId = "ID_V7_DE_LA_LAMPE"
$ApiToken = "JETON_CONFIGURE_DANS_wifi_credentials.h"
$Headers = @{ Authorization = "Bearer $ApiToken" }

$request = Invoke-RestMethod `
    -Method Post `
    -Uri "http://$CoreIp/api/v1/devices/$LampId/commands/power" `
    -Headers $Headers `
    -ContentType "application/json" `
    -Body '{"state":"on"}'

$request.command
```

Une commande acceptée renvoie HTTP `202` et une URL de suivi :

```json
{"command":{"id":1364364413,"state":"sent"}}
```

L’acceptation HTTP confirme la création locale et l’envoi UART, pas l’exécution de la lampe. Un ID inconnu renvoie `404`; une lampe ou un MAIN offline renvoie `409`; un transport ou tracker indisponible renvoie `503`. Les autres erreurs ont une réponse JSON avec un code stable.

## Lire l’état

### `GET /api/v1/commands/{id}`

La route exige le même Bearer token :

```powershell
$commandId = $request.command.id
Invoke-RestMethod `
    -Method Get `
    -Uri "http://$CoreIp/api/v1/commands/$commandId" `
    -Headers $Headers
```

Réponse :

```json
{"command":{"id":1364364413,"state":"executed","retries":0}}
```

Les états sont `sent`, `accepted`, `executed`, `failed` et `expired`. Pour `failed` ou `expired`, la réponse contient `error`. L’expiration signifie que CORE n’a pas reçu de confirmation finale dans le délai V7.2; l’exécution réelle peut être inconnue. L’historique est en RAM, borné par le tracker V7.2 et perdu au redémarrage; un ID absent ou évincé renvoie `404 command_not_found`.

## Erreurs d’accès et de requête

- `401 unauthorized` : jeton absent ou incorrect.
- `400 bad_request` : chemin, ID ou corps JSON invalide.
- `405 method_not_allowed` : méthode HTTP inadaptée; l’en-tête `Allow` indique la méthode attendue.
- `413 payload_too_large` : le corps dépasse 64 octets.
- `415 unsupported_media_type` : le corps n’est pas envoyé en `application/json`.
- `503 command_api_disabled` : aucun jeton assez long n’est compilé dans CORE-WIFI.

## Validation

Cette procédure reste reproductible en suivant les essais V7.2 décrits dans [le dossier de commande](V7_2_COMMANDS.md), en soumettant les ordres depuis PowerShell à la place de la console série. Elle couvre ON/OFF, état `executed`, token absent/incorrect, ID inconnu et cible offline. Les essais d’expiration et de perte d’ACK sont décrits dans le dossier V7.2 et la clôture V7.4. La mesure de flash courante après V7.4.2 est documentée dans [V7.4](V7_4_RELIABILITY.md).

### Résultat matériel — 8 octobre 2026

Le parcours `POST` avec `{"state":"on"}` a renvoyé l’ID `1978340518` et l’état initial `sent`. `GET /api/v1/commands/1978340518` a ensuite renvoyé `executed`, `retries=0`. Le journal LAMP contient la commande destinée à l’ID V7 `3559985816` puis `Power -> ON`; CORE-WIFI a journalisé l’acceptation MAIN et l’ACK final exécuté.

Le parcours `POST` avec `{"state":"off"}` a renvoyé l’ID `2817180636` et l’état initial `sent`. CORE-WIFI a journalisé `accepted`, puis `executed retries=0`; LAMP a reçu la commande pour `3559985816` et affiché `Power -> OFF`. L’état terminal OFF provient du tracker CORE; la réponse HTTP GET de suivi pour cet ID n’a pas été fournie.

ON et OFF confirment le parcours nominal HTTP jusqu’à LAMP. Le rejet d’un jeton incorrect, la lecture d’un état terminal, le jeton absent, les corps invalides, l’ID inconnu et le refus d’une cible offline sont également consignés ci-dessous. Les traces V7.3 de retry/expiration via HTTP n’ont pas toutes été archivées; les essais V7.4.2 disposent d’IDs distincts dans [leur dossier](V7_4_RELIABILITY.md).

Le 8 octobre, une requête avec un Bearer token incorrect a reçu `401 unauthorized`. Avec le jeton correct, le `POST` OFF suivant a créé la commande `2817180637`, puis `GET /api/v1/commands/2817180637` a renvoyé `executed`, `retries=0`. Cela valide le contrôle d’accès par jeton invalide et la lecture HTTP du résultat terminal. La tentative rejetée n’a pas remplacé la variable PowerShell `$request`; l’ID affiché après l’erreur était donc celui de la requête précédente.

Deux requêtes sans en-tête `Authorization` ont ensuite reçu `401 Unauthorized` : le `POST /api/v1/devices/3559985816/commands/power` et le `GET /api/v1/commands/2817180637`. Les deux réponses contenaient `WWW-Authenticate: Bearer` et `Cache-Control: no-store`. Cela confirme que les routes de soumission et de suivi refusent l’accès sans jeton.

Un `POST` authentifié avec `{"state":"maybe"}` a reçu `400 Bad Request`, code `bad_request`, avec le message indiquant que seules les valeurs `on` et `off` sont admises. Aucun ID de commande n’a été créé par cette réponse.

Un `POST` authentifié avec un corps valide `{"state":"off"}` vers l’ID absent `999999` a reçu `404`, code `unknown_destination`. La réponse ne contient pas d’ID de commande; le rejet est fait avant création et envoi.

Après débranchement de LAMP_C6, `GET /api/v1/devices` a indiqué `online=false` et `status=offline` pour l’ID `3559985816`. Un `POST` OFF authentifié à cette cible a reçu `409 Conflict`, code `offline`, sans ID de commande.

L’utilisateur confirme que l’ensemble des essais fonctionne, y compris la reprise, les retries et l’expiration via HTTP. Les traces de ces derniers scénarios n’ont pas été fournies dans la conversation; la validation fonctionnelle est donc confirmée, mais leur détail n’est pas archivé ici.
