# Cordialement. - portage Minitel / ESP32

Portage non-officiel du mini-jeu web [Cordialement.](https://www.play-fool.net/cordialement)
(jeu de frappe administrative créé par Thomas François et Gildas Paubert) sur
Minitel 1B, via un ESP32 et la bibliothèque
[Minitel1B_Hard](https://github.com/eserandour/Minitel1B_Hard) d'iodeo/eserandour.

Ce dépôt ne réutilise pas le code source du jeu original : seuls le principe
et les règles de score sont repris, adaptés aux contraintes du terminal
(40 colonnes, pas de son, clavier Minitel).

## Principe du jeu

Un mail administratif s'affiche à l'écran. Il faut taper le mot
**« cordialement »** (12 lettres) le plus vite et le plus proprement
possible, avant la fin du chronomètre affiché en bas d'écran.

- La partie se joue en **10 manches**, avec **6 vies**.
- Une erreur de frappe ou un dépassement du temps imparti fait perdre une vie
  et rapporte 0 point sur la manche.
- Plus la manche est réussie vite, plus le score est élevé.
- À la fin de la partie (victoire ou défaite), un rang (S à F, calculé sur le
  temps moyen) et un score final sont affichés, et le joueur peut entrer un
  pseudo pour le classement de la borne.
- Le classement (highscores) est conservé en mémoire flash (NVS/`Preferences`
  de l'ESP32) et survit donc aux redémarrages, jusqu'à réinitialisation
  manuelle (touche **Correction** depuis l'écran d'accueil).
- Sans activité, l'écran d'accueil bascule automatiquement en mode
  « attract » et alterne avec l'écran des highscores.

## Matériel nécessaire

- Un Minitel 1B (ou compatible) avec sa prise DIN 5 broches / péri-informatique.
- Une carte **ESP32**.
- Le montage matériel décrit par [Minitel1B_Hard](https://github.com/eserandour/Minitel1B_Hard)
  (adaptateur niveaux TTL/Minitel, alimentation du Minitel, etc.).
- Câblage sur `Serial2` (RX2/TX2) de l'ESP32.

## Dépendances logicielles

- [Arduino IDE](https://www.arduino.cc/en/software) ou PlatformIO, avec le
  core **ESP32**.
- Bibliothèque **Minitel1B_Hard** : utiliser de préférence une copie patchée
  ajoutant des timeouts sur `workingAiguillage`/`workingMode`/`workingKeyboard`
  dans `Minitel1B_Hard.cpp`, pour éviter que `getKeyCode()` ne bloque le
  programme si le Minitel n'est pas branché ou ne répond pas.
- Bibliothèque **Preferences** (fournie avec le core ESP32) pour la
  persistance du classement en NVS.

## Installation

1. Installer le core ESP32 et la bibliothèque Minitel1B_Hard (patchée, voir
   ci-dessus) dans l'Arduino IDE.
2. Ouvrir `minitel-cordialement.ino`.
3. Vérifier le câblage RX2/TX2 vers le Minitel, et adapter si besoin le
   constructeur `Minitel minitel(Serial2);` (une variante
   `Minitel(HardwareSerial&, int8_t rxPin, int8_t txPin)` existe pour
   repositionner les broches).
4. Flasher la carte ESP32.
5. Allumer le Minitel : l'écran d'accueil doit s'afficher automatiquement.

### Vitesse de liaison

Le programme tente de passer le Minitel en **4800 bauds** au démarrage
(`minitel.changeSpeed(4800)`). Certains Minitel 1B ne supportent pas cette
vitesse : si l'écran reste vide ou affiche des caractères incohérents après
le changement, repasser en 1200 bauds dans `setup()`.

## Configuration

Les principaux réglages se trouvent en haut de `minitel-cordialement.ino` :

| Constante | Rôle |
|---|---|
| `NB_MANCHES` | Nombre de manches par partie (10 par défaut) |
| `NB_VIES` | Nombre de vies (6 par défaut) |
| `MANCHE_DURATION` | Temps alloué pour taper le mot (ms) |
| `PERFECT_DURATION` | Temps sous lequel le score est maximal |
| `ERROR_MALUS` | Temps « forfait » compté en cas d'erreur/timeout |
| `MAX_HIGHSCORES` | Taille du classement conservé en flash |
| `ENABLE_BEEP` | Active/désactive le bip du Minitel |

La bibliothèque d'e-mails affichés pendant les manches (`EMAIL_1` à
`EMAIL_4`) peut être étendue : chaque ligne doit faire au maximum
**37 caractères** pour ne pas provoquer de retour à la ligne intempestif sur
un écran Minitel de 40 colonnes.

## Commandes au clavier Minitel

| Touche | Effet |
|---|---|
| Lettres | Saisie du mot à taper / du pseudo |
| **Envoi** | Valider la manche en cours, passer l'écran, lancer une partie |
| **Correction** | Effacer un caractère (saisie du pseudo) ; depuis l'accueil, réinitialise le classement |

## Limitations connues

- Pas de gestion des accents (jeu de caractères Minitel de base).
- Pas de connexion réseau : uniquement une utilisation en mode local, en
  point à point avec un Minitel.
- Les statistiques détaillées (`GameStat`) ne sont pas persistées, seul le
  classement final (pseudo/score/meilleur temps) l'est.

## Licence et crédits

Jeu original (navigateur) : [play-fool.net/cordialement](https://www.play-fool.net/cordialement),
créé par Thomas François et Gildas Paubert.

Ce portage Minitel reprend le principe et les règles de score du jeu
original, adaptés au terminal, mais est intégralement réécrit pour
l'ESP32 / Minitel1B_Hard.
