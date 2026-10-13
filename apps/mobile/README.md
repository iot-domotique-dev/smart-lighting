# Smart Lighting Mobile — V7.6.4

Application Expo SDK 57 / React Native / TypeScript pour consulter CORE-WIFI, piloter les lampes individuellement en ON/OFF et les ajouter depuis Android/iOS à l’aide de l’API de pairing V7.6.3 existante.

Le bilan V7.6.4, y compris le résultat matériel rapporté pour l’ajout d’une lampe depuis le téléphone, est dans le [dossier V7.6](../../docs/V7_6_PAIRING.md). Le dossier [V7.5](../../docs/V7_5_MOBILE_APP.md) décrit la consultation et les commandes ON/OFF.

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

`last_confirmed_state` décrit la dernière commande ON/OFF confirmée par ACK : `unknown`, `confirmed` ou `stale`. Il ne mesure pas l’état électrique de la lampe. Une lampe hors ligne peut conserver un historique `stale`. Le champ API existant `state:null` reste inchangé.

## Piloter les lampes

Chaque carte propose **Allumer** et **Éteindre**. Un seul POST est envoyé par appui à `/devices/{id}/commands/power`; en cas de perte de réponse, l’application affiche un résultat incertain et ne renvoie jamais automatiquement ce POST. Elle conserve l’ID, la lampe, la demande et les états de suivi en mémoire pendant l’exécution.

Après HTTP `202`, le client suit `/commands/{id}` avec un délai progressif, jusqu’à `executed`, `failed`, `expired` ou un délai mobile maximal de 30 secondes. `sent` et `accepted` restent en cours. Après un état terminal, la fiche lampe est relue depuis `/devices/{id}`. L’affichage ON/OFF reste fondé sur `last_confirmed_state` et n’est jamais modifié optimistement par l’appui.

Une commande en cours désactive les deux boutons de cette lampe; une autre lampe reste pilotable en parallèle. Les boutons sont également désactivés si la lampe, son MAIN ou la fraîcheur de l’inventaire ne permettent pas de confirmer sa disponibilité. Modifier l’adresse ou le jeton CORE attend la fin du suivi. Le retour au premier plan reprend les lectures de statut. Après fermeture complète de l’application ou redémarrage du CORE, le résultat peut rester inconnu; aucune nouvelle commande n’est rejouée automatiquement.

`executed` confirme l’exécution logicielle par ACK, pas un état électrique mesuré. `expired`, `command_not_found`, une réponse POST perdue et un délai de suivi sont présentés comme incertains; l’application relit l’état connu, sans prétendre prouver quelle commande l’a produit.

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

Les scénarios de découverte nécessitent une lampe réellement non appairée. Une candidate ne figure pas dans l’inventaire opérationnel avant le succès final. Les limites firmware restent 10 lampes, 10 candidates simultanées et 10 routes Zigbee au total. L’API ne donne pas de suppression/désappairage et le contrat `/devices` ne fournit pas l’identité matérielle d’une lampe déjà connue; l’écran ne peut donc que s’appuyer sur la liste des candidates filtrée par le MAIN.

### Résultat matériel rapporté — 13 octobre 2026

L’utilisateur a confirmé la réussite de l’ajout d’une lampe depuis l’application sur son téléphone, avec un seul MAIN et une LAMP_C6. Le compte rendu ne précise pas l’OS du téléphone et ne contient ni ID d’opération, ni ID local, ni capture ou journal série. Cette validation couvre le parcours nominal d’ajout sur ce montage; elle ne valide pas la suppression depuis l’application, le multi-MAIN ou plusieurs lampes physiques.

## Structure

- `App.tsx` : écran unique, configuration ou inventaire.
- `src/api/` : types et client HTTP de lecture, commande et suivi.
- `src/settings/` et `src/security/` : adresse et jeton conservés via SecureStore.
- `src/features/core/` : configuration et test du CORE.
- `src/features/lighting/` : liste, détail, commandes d’éclairage et suivi progressif.
- `src/features/pairing/` : parcours d’ajout, suivi prudent des sessions et opérations.
- `src/components/` : indicateur visuel partagé.
- `test/` : tests du client API et du workflow pairing avec réponses simulées.

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

Pour V7.6.4, `npm run typecheck`, `npm run lint`, les **34/34 tests simulés**, `npx expo install --check` et `npx expo export --platform android` ont réussi après alignement des patchs `expo` et `expo-dev-client` sur SDK 57. L’environnement de vérification utilisait Node 20.19 et npm a signalé `EBADENGINE`; le projet et Expo SDK 57 demandent Node 22.13 ou plus récent. Le parcours nominal d’ajout a depuis été confirmé sur le téléphone de l’utilisateur; refaire les vérifications logicielles sous Node 22.13+ reste souhaitable pour éliminer cet avertissement d’environnement. L’export Android seul ne constitue pas une validation matérielle.
