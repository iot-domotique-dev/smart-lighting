# Smart Lighting Mobile — V7.8.0

Application Expo SDK 57 / React Native / TypeScript pour consulter CORE-WIFI, piloter les lampes individuellement en ON/OFF et en luminosité, les ajouter et les retirer depuis Android/iOS avec les API V7 existantes.

Le parcours d’ajout V7.6.4 et son résultat matériel rapporté sont dans le [dossier V7.6](../../docs/V7_6_PAIRING.md). Le protocole de retrait du firmware V7.7.2 et son état de validation sont dans le [dossier V7.7](../../docs/V7_7_UNPAIRING.md). Le dossier [V7.5](../../docs/V7_5_MOBILE_APP.md) décrit la consultation et les commandes ON/OFF.

## Préparer et démarrer

Prérequis : Node.js 22.13 ou plus récent, npm et un téléphone Android relié au même réseau local que le PC et CORE-WIFI. Expo SDK 57 utilise React Native 0.86 et exige cette version minimale de Node.js.

Depuis la racine du dépôt, dans PowerShell :

```powershell
cd apps/mobile
npm ci
npx expo start --go --lan
```

Expo Go SDK 57 est maintenant disponible dans les magasins officiels. Mets Expo Go a jour sur le telephone. Le projet et Expo Go doivent utiliser le meme SDK.
Sur Android, si le magasin ne propose pas encore cette version sur ton appareil, telecharge-la avec :
```powershell
npx expo-go download android 57
```
Sur iPhone, Expo Go SDK 57 est disponible dans l'App Store. Pour ouvrir un projet en developpement, connecte Expo Go et le terminal au meme compte Expo (commande npx expo login). Le sideload d'une ancienne version d'Expo Go n'est pas pris en charge sur iPhone.
Pour appliquer de façon fiable la configuration native du trafic HTTP local, utiliser un build de développement. Après installation de l’Android SDK et activation du débogage USB sur le téléphone :

```powershell
cd apps/mobile
npm ci
npx expo run:android --device
npx expo start --dev-client --lan
```

Le premier build crée les dossiers natifs ignorés par Git. Le réglage Android `usesCleartextTraffic` et l’exception iOS pour le réseau local sont déclarés dans `app.json`; Expo Go utilise sa propre configuration native. Aucun build de développement n’a été installé sur téléphone pendant V7.5.1.

## Connexion au CORE-WIFI

1. Sur le téléphone, rester sur le Wi-Fi local qui peut joindre CORE-WIFI. Éviter les réseaux invités qui isolent les appareils.
2. Saisir **uniquement l’IPv4 privée** du CORE-WIFI, sans `http://` ni port. L’adresse apparaît dans le moniteur série CORE-WIFI après connexion.
3. Saisir le jeton Bearer configuré localement pour CORE-WIFI. Il est conservé dans `expo-secure-store` sur le téléphone et n’est jamais inclus dans le dépôt ni dans la configuration distribuée.
4. Toucher **Tester la connexion** pour vérifier `/health` et `/core`, puis **Enregistrer et continuer**. Le test HTTP ne vérifie pas le jeton ; les routes de lecture sont publiques. Le jeton stocké dans SecureStore autorise les commandes et leur suivi.
5. La liste charge `/health`, `/modules` et `/devices`. Toucher une lampe pour obtenir `/devices/{id}`. **Actualiser** ou tirer la liste vers le bas pour recharger immédiatement.

L’actualisation automatique se fait toutes les 20 secondes pendant que l’application est active. Chaque requête a un délai de 5 secondes. En cas de perte réseau, la dernière liste reste visible avec un avertissement ; elle n’est plus présentée comme une disponibilité actuelle.

`last_confirmed_state` décrit les dernières consignes ON/OFF et brightness confirmées par ACK : `unknown`, `confirmed` ou `stale`. La propriété brightness peut être absente sur un ancien CORE, ou être `null` avec le statut `unknown`. Ces informations ne mesurent pas l’état électrique ni la lumière réellement émise. Une lampe hors ligne peut conserver un historique `stale`. Le champ API existant `state:null` reste inchangé.

## Piloter les lampes

Chaque carte propose **Allumer** et **Éteindre**. Un seul POST est envoyé par appui à `/devices/{id}/commands/power`; en cas de perte de réponse, l’application affiche un résultat incertain et ne renvoie jamais automatiquement ce POST. Elle conserve l’ID, la lampe, la demande et les états de suivi en mémoire pendant l’exécution.

Les lampes qui annoncent la capacité `brightness` affichent aussi **Luminosité**, avec un curseur entier de 0 à 100. Il reste visible hors ligne, mais devient inactif si la lampe ou son MAIN est indisponible, si les données sont anciennes, si un retrait bloque la lampe ou si une commande de sortie est active. Les lampes sans cette capacité gardent les commandes ON/OFF sans curseur. Le curseur affiche la sélection locale séparément du dernier niveau confirmé; `unknown` reste inconnu au lieu d’être remplacé par une valeur supposée. `stale` est affiché comme historique à vérifier.

Pendant le glissement, seule la sélection locale change. Un seul POST à `/devices/{id}/commands/brightness` avec `{"percent": n}` est envoyé au relâchement, si la valeur a changé et qu’elle n’est pas déjà la consigne confirmée. Aucun POST n’est envoyé à chaque mouvement. `0 %` applique la sémantique OFF du firmware; `1–100 %` applique une consigne ON. L’application n’envoie jamais un ON/OFF additionnel avec brightness.

Après HTTP `202`, le client suit `/commands/{id}` avec un délai progressif, jusqu’à `executed`, `failed`, `expired` ou un délai mobile maximal de 30 secondes. `sent` et `accepted` restent en cours. Après un état terminal, la fiche lampe est relue depuis `/devices/{id}`. L’affichage ON/OFF reste fondé sur `last_confirmed_state` et n’est jamais modifié optimistement par l’appui.

Les commandes ON/OFF et brightness partagent le même verrou par lampe : aucune seconde commande de sortie ne peut être envoyée pendant un suivi ou tant qu’un résultat incertain doit être relu. Une autre lampe reste pilotable en parallèle. Modifier l’adresse ou le jeton CORE attend la fin du suivi. Le retour au premier plan reprend les lectures de statut. Après fermeture complète de l’application ou redémarrage du CORE, le résultat peut rester inconnu; aucune nouvelle commande n’est rejouée automatiquement.

`sent` et `accepted` restent en cours; seul `executed` confirme l’exécution logicielle par ACK. Après cet ACK, l’application relit le détail de la lampe et son `last_confirmed_state`. `failed`, `expired`, `command_not_found`, une réponse POST perdue et un délai de suivi ne sont jamais présentés comme une réussite. Le client relit l’état par GET, puis permet une nouvelle action explicite seulement après cette réconciliation; il ne répète jamais le POST perdu. `executed` confirme une consigne firmware, pas l’état électrique ni l’intensité lumineuse réellement mesurée.

La consigne brightness n’est conservée qu’en RAM par le firmware LAMP. Après un redémarrage à froid de LAMP, l’application ne suppose pas que le niveau antérieur a été restauré; elle affiche les valeurs et statuts réellement fournis par CORE.

Le Slider natif est fourni par `@react-native-community/slider` 5.2.0, version alignée sur Expo SDK 57. Expo Go SDK 57 inclut ce module; un client de développement ou une application native déjà construite doit être rebâti pour intégrer ce nouveau code natif. L’installation d’une bibliothèque native ne peut pas être mise à jour par un simple rechargement JavaScript.

## Retirer une lampe — V7.7.3

Dans le détail d’une lampe en ligne, **Retirer cette lampe** ouvre une confirmation destructive avec son nom et son ID V7. Le texte précise que l’action retire l’association de Smart Lighting et n’éteint pas la sortie électrique. **Annuler** ne lance aucune requête. Le retrait est désactivé si la lampe ou son MAIN est hors ligne, si l’inventaire n’est plus frais, si une commande SET_POWER a un résultat en cours ou incertain, si un retrait est déjà actif ou si son état local ne peut pas être sauvegardé.

Le parcours utilise uniquement les routes firmware V7.7.2 existantes :

- `POST /api/v1/devices/{id}/removal-operations` avec `{"operation_id": <uint32 non nul>}`;
- `GET /api/v1/removal-operations/{operation_id}` pour le suivi.

Le client génère l’identifiant avec `expo-crypto`, enregistre d’abord l’opération dans `expo-secure-store`, puis envoie un seul POST. Le Bearer token n’est pas copié dans cet enregistrement. HTTP `202` et `processing` signifient que le CORE suit l’opération, pas que la lampe a déjà disparu. L’application interroge le même identifiant par GET. Une perte de réponse HTTP, une interruption de l’application ou un redémarrage du téléphone reprennent ce suivi; aucun second POST n’est envoyé automatiquement.

Les états `processing`, `succeeded`, `failed`, `uncertain` et `not_found` restent distincts. Après `succeeded`, l’application relit `/api/v1/devices` et ne retourne à la liste qu’après avoir vérifié que la lampe a disparu. Si elle est toujours listée, l’écran propose seulement d’actualiser l’inventaire. `not_found` n’établit pas que le POST n’a jamais été reçu : l’opération reste à vérifier et ne peut pas être renvoyée depuis cette reprise. Une erreur réseau/serveur de suivi peut être retentée en GET seulement.

Le blocage est global pour un second retrait et cible les commandes ON/OFF de la lampe qui est en cours de retrait. L’ajout/pairing reste une action séparée; l’application ne le démarre pas automatiquement après le retrait. La confirmation de retrait ne mesure pas l’état électrique et n’envoie pas une commande power OFF.

### Vérification Android V7.7.3

Avec CORE-WIFI V7.7.2 accessible sur le réseau local, un MAIN et une lampe appairée :

1. Ouvrir le détail de la lampe, toucher **Retirer cette lampe**, vérifier le nom, l’ID et le texte sur la sortie électrique, puis toucher **Annuler**. Vérifier que la lampe est toujours dans la liste.
2. Recommencer et toucher **Confirmer le retrait**. Vérifier que l’opération atteint `succeeded`, puis que la lampe disparaît de la liste après la relecture de l’inventaire.
3. Vérifier dans le moniteur MAIN/LAMP les messages de retrait normaux et contrôler que la lampe retirée n’est plus commandable depuis l’application.
4. Refaire un pairing depuis **Ajouter une lampe** si l’on veut vérifier le cycle complet; il s’agit d’une action distincte.
5. Pour une vérification de reprise, interrompre l’application après le POST, la relancer, puis vérifier qu’elle reprend le même `operation_id` en GET. Ne pas provoquer cette interruption sur une installation à préserver sans pouvoir récupérer l’ID/la configuration.

Les tests simulés et l’export Android ne remplacent pas l’essai sur téléphone et cartes. Le compte rendu matériel du 15 octobre 2026 est consigné ci-dessous; les cas de panne et de concurrence restent à tester.

### Résultat matériel rapporté — 15 octobre 2026

L’utilisateur confirme sur Android et matériel réel la découverte, l’ajout avec nom, l’appairage, ON/OFF, le retrait depuis l’application, le réappairage avec un nouvel ID V7 et la persistance après redémarrage. La validation utilise un MAIN et une LAMP. Aucun ID d’opération ou journal série complet n’a été communiqué. Les scénarios de plusieurs lampes actives, de concurrence et d’interruption d’une transaction restent hors de cette validation nominale.

## Ajouter une lampe — V7.6.4

Depuis **Mes lampes**, toucher **Ajouter une lampe** ouvre l’écran de pairing. L’écran vérifie `/health`, lit la session existante et reprend une session active sans en ouvrir une autre. Consulter l’écran ne démarre pas la radio : toucher **Démarrer la recherche** est nécessaire pour envoyer le POST d’ouverture de 60 secondes.

Quand la session est confirmée `active`, l’application lit les candidates toutes les 3 secondes. Chaque ligne est identifiée par son `hardware_id`; aucune candidate n’est sélectionnée automatiquement. L’utilisateur choisit une candidate, saisit un nom de 1 à 31 caractères ASCII parmi lettres, chiffres, espace, `_`, `-`, `.`, apostrophe, puis confirme explicitement l’appairage.

Le parcours utilise exclusivement ces routes existantes :

- `POST`, `GET` et `DELETE /api/v1/pairing/session`;
- `GET /api/v1/pairing/candidates`;
- `POST /api/v1/pairing/operations`;
- `GET /api/v1/pairing/operations/{id}`.

Toutes les requêtes de pairing portent le Bearer token de SecureStore. Les états de session `opening`, `active`, `closing`, `closed`, `expired`, `failed` et `uncertain` sont distingués. Les états d’opération `pending`, `processing`, `succeeded`, `failed`, `rejected`, `expired` et `uncertain` sont également affichés distinctement. Seul `succeeded` confirme l’appairage. Après ce résultat, l’inventaire `/api/v1/devices` est relu pendant quelques secondes; si l’annonce de la lampe tarde, l’application affiche que la synchronisation est en cours.

Chaque confirmation produit un `operation_id` non nul sur 32 bits. Une seule requête POST est envoyée. Si sa réponse est perdue, l’application consulte l’opération par ID et ne répète jamais le POST. Une ouverture de session ou une fermeture à réponse perdue est réconciliée par GET. La fermeture est asynchrone : l’écran reste affiché jusqu’à confirmation du CORE. Le retour au premier plan relit la session, les candidates et l’opération connue. Un résultat incertain doit être vérifié avant une nouvelle opération.

L’identifiant d’opération reste en mémoire dans l’application pendant le parcours; il n’est pas persisté sur le téléphone. Si Android tue complètement le processus avant la fin, l’écran peut reprendre la session existante, mais ne peut plus interroger l’ancienne opération sans son ID. CORE-WIFI ne conserve lui-même les sessions et opérations qu’en RAM; après son redémarrage, il faut contrôler l’inventaire avant de lancer une nouvelle demande.

## Procédure de validation sur téléphone

Utiliser un CORE-WIFI avec le firmware V7.6.3, un MAIN et au moins une LAMP_C6 non appairée. Le téléphone et CORE-WIFI doivent être sur le même réseau local; l’application doit déjà avoir une adresse IP et un jeton Bearer valides.

1. Ouvrir **Ajouter une lampe**, vérifier que le CORE est joignable et toucher **Démarrer la recherche**.
2. Attendre la session `active`, alimenter la lampe non appairée et actualiser les candidates.
3. Vérifier son `hardware_id`, la sélectionner, saisir un nom valide et toucher **Continuer**, puis **Confirmer et appairer**.
4. Vérifier que l’opération atteint `succeeded` et qu’un ID local est affiché. La lampe doit ensuite apparaître dans **Mes lampes**; si l’inventaire tarde, attendre le message de synchronisation puis actualiser.
5. Toucher **Terminer** et attendre la confirmation de fermeture de session avant de quitter l’écran.

Les scénarios de découverte nécessitent une lampe réellement non appairée. Une candidate ne figure pas dans l’inventaire opérationnel avant le succès final. Les limites firmware restent 10 lampes, 10 candidates simultanées et 10 routes Zigbee au total. Au jalon V7.6.4, le retrait applicatif n’était pas encore disponible; il est ajouté en V7.7.3 dans le [dossier de retrait](../../docs/V7_7_UNPAIRING.md). Le contrat `/devices` ne fournit pas l’identité matérielle d’une lampe déjà connue, et l’écran d’ajout s’appuie donc sur la liste des candidates filtrée par le MAIN.

### Résultat matériel rapporté — 13 octobre 2026

L’utilisateur a confirmé la réussite de l’ajout d’une lampe depuis l’application sur son téléphone, avec un seul MAIN et une LAMP_C6. Le compte rendu ne précise pas l’OS du téléphone et ne contient ni ID d’opération, ni ID local, ni capture ou journal série. Cette validation couvre le parcours nominal d’ajout sur ce montage; elle ne valide pas le nouveau parcours de retrait V7.7.3, le multi-MAIN ou plusieurs lampes physiques.

## Structure

- `App.tsx` : écran unique, configuration ou inventaire.
- `src/api/` : types et client HTTP de lecture, commande et suivi.
- `src/settings/` et `src/security/` : adresse et jeton conservés via SecureStore.
- `src/features/core/` : configuration et test du CORE.
- `src/features/lighting/` : liste, détail, commandes d’éclairage, retrait et suivis réconciliables.
- `src/features/pairing/` : parcours d’ajout, suivi prudent des sessions et opérations.
- `src/components/` : indicateur visuel partagé.
- `test/` : tests du client API, du workflow pairing et du workflow de retrait avec réponses simulées.

## Vérifications locales

```powershell
npm run lint
npm run typecheck
npm run test:api
npx expo install --check
npx expo config --type public
npx expo export --platform android
```

Pour V7.5.2, le typage, le lint, les 15/15 tests simulés, la vérification des dépendances SDK 57 et l’export Android ont réussi. Le premier essai nominal ON/OFF depuis Android a aussi été confirmé sur une LED de test raccordée à GPIO18; les scénarios matériels restant à vérifier sont listés dans le [dossier V7.5](../../docs/V7_5_MOBILE_APP.md). L’API actuelle utilise HTTP sans TLS : le port 80 du CORE ne doit pas être publié sur Internet.

La migration SDK 57 réduit les avis rapportés par npm, mais `npm audit fix --force` n’est pas appliqué automatiquement : ses propositions peuvent changer les versions majeures. La mise à niveau vers un SDK ultérieur fera l’objet d’une validation séparée.

Pour V7.6.4, `npm run typecheck`, `npm run lint`, les **34/34 tests simulés**, `npx expo install --check` et `npx expo export --platform android` ont réussi après alignement des patchs `expo` et `expo-dev-client` sur SDK 57. L’environnement de vérification utilisait Node 20.19 et npm a signalé `EBADENGINE`; le projet et Expo SDK 57 demandent Node 22.13 ou plus récent.

Pour V7.7.3, `npm run lint`, `npm run typecheck`, les **56/56 tests simulés** et `npx expo export --platform android` avaient réussi. `npx expo install --check` signalait que `expo@57.0.28` était un patch en dessous de `~57.0.29`; cette dépendance n’a pas été mise à jour dans V7.8.0. Node 20.19.4 est utilisé ici alors que le projet demande Node 22.13 ou plus récent. L’export Android vérifie le bundling JavaScript, pas l’installation native, le comportement sur téléphone ou le firmware.

Pour le candidat V7.8.0, les résultats consignés sont : `npm run test:api` (**69/69**), `npm run typecheck`, `npm run lint`, `npx expo install --check` et `npx expo export --platform android` réussis. Le contrôle Expo a utilisé la table locale faute de réseau; la dépendance déclarée reste `~57.0.28`, donc l’écart déjà relevé avec `~57.0.29` n’a pas été tranché avec le registre en ligne. Node 20.19.4 a affiché `EBADENGINE`; le projet demande Node 22.13 ou plus récent. Le prébuild Android a échoué dans l’environnement de travail lors de la création du cache d’icônes; aucun SDK Android local n’était installé, donc aucun APK Gradle natif n’a été compilé. Ces vérifications logicielles ne remplacent pas l’essai matériel.

### Validation matérielle rapportée — 16 octobre 2026

L’utilisateur confirme le pilotage de la luminosité depuis Android jusqu’à la LED physique de test sur GPIO18. Le trajet a utilisé l’API HTTP locale, UART, Zigbee et le profil unique `lamp_c6` avec LEDC. Le montage est une LED basse tension avec résistance série de 220 Ω; cette sortie ne convient pas à une lampe secteur ou à une charge de puissance. L’essai couvre un MAIN et une LAMP.

La validation V7.7 antérieure, le 15 octobre, couvre l’ajout/appairage, ON/OFF, le retrait depuis l’application, le réappairage et la persistance. Ces parcours n’ont pas été répétés pendant l’essai PWM V7.8.0. Plusieurs lampes pilotées simultanément, les scénarios de panne/récupération et la persistance de la luminosité après redémarrage à froid de LAMP ne sont pas validés. La luminosité reste en RAM et n’est pas persistée en NVS.
