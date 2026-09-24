/*
  ============================================================
  CORDIALEMENT - portage Minitel / ESP32 (Minitel1B_Hard)
  ============================================================
  Portage du mini-jeu web "Cordialement." (jeu de frappe admin)
  pour Minitel 1B via iodeo Minitel-ESP32.

  Jeu original (navigateur) : https://www.play-fool.net/cordialement
  Cree par Thomas François et Gildas Paubert.
  Ce portage Minitel en reprend le principe et les regles de score,
  adaptes aux contraintes du terminal (40 colonnes, pas de son,
  clavier Minitel). Tout le code ci-dessous est reecrit pour l'ESP32 /
  Minitel1B_Hard, il ne reutilise pas le code source du jeu original.

  Principe : taper le mot "cordialement" (12 lettres) le plus
  vite possible, sur N manches, avec un nombre de vies limité.
  Highscores conservés en RAM le temps de la session (pas
  d'EEPROM/SD, pas de réseau) -> repartent à zéro au reboot.

  IMPORTANT - vérifié contre la vraie API eserandour/Minitel1B_Hard
  (header recupere le 09/07/2026) :
  - Constructeur : Minitel minitel(Serial2); (pas d'entier, une reference
    HardwareSerial). Pour repositionner RX/TX sur ESP32, il existe aussi
    Minitel(HardwareSerial& serial, int8_t rxPin, int8_t txPin).
  - print() n'accepte QUE un String -> pour un caractere seul on utilise
    printChar(char) (deja fait dans ce sketch, ne pas remettre print('x')).
  - Pas de minitel.available() : on teste Serial2.available() directement
    (Serial2 est l'objet HardwareSerial passe au constructeur), puis on
    appelle minitel.getKeyCode() seulement si des octets sont arrives.
  - Curseur : cursor() = visible, noCursor() = invisible (pas de bool en
    parametre).
  - Bip : minitel.bip() existe nativement dans la lib (envoie BEL 0x07),
    utilise a la place d'un Serial2.write(0x07) manuel.
  - Utilise ta copie patchée de Minitel1B_Hard.cpp (timeouts sur
    workingAiguillage/workingMode/workingKeyboard) pour éviter les
    blocages si le Minitel n'est pas branché : getKeyCode() appelle ces
    fonctions en interne et peut bloquer sans cette patch.
  - echo(false) est appelé dans setup() (sinon le Minitel réaffiche
    les lettres tapées par-dessus notre propre rendu).
  - Pas d'accents dans les textes (jeu de caractères Minitel de base) ;
    la lib gère les diacritiques via printSpecialChar()/accent grid si
    tu veux les ajouter plus tard.
  ============================================================
*/

#include <Minitel1B_Hard.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <Preferences.h>

Minitel minitel(Serial2); // Serial2 (RX2/TX2)

// ---------------- CONFIG ----------------
#define ENABLE_BEEP true

const uint8_t  NB_MANCHES        = 10;
const uint8_t  NB_VIES           = 6;
const unsigned long MANCHE_DURATION  = 6000UL; // ms pour taper les 12 lettres
const unsigned long PERFECT_DURATION = 2500UL; // sous ce temps -> score max
const unsigned long ERROR_MALUS      = 6000UL; // temps "forfait" si erreur/timeout
const unsigned long RESULT_DURATION  = 3200UL; // durée affichage résultat de manche
const unsigned long INTRO_DURATION   = 2600UL; // durée écran "pret ? go !"
const unsigned long ENDGAME_DURATION = 9000UL; // avant retour auto a l'accueil
const unsigned long HIGHSCORE_DURATION = 12000UL;
const unsigned long HOME_IDLE_DURATION = 12000UL; // avant de basculer vers l'ecran highscores (mode "attract")
const uint8_t  MAX_HIGHSCORES    = 8;
const uint8_t  WORD_LEN          = 12;
const char WORD[WORD_LEN + 1]    = "cordialement";

const unsigned long TOUCHE_ENVOI = 0x1341; // confirmé sur les 2 projets precedents
const unsigned long TOUCHE_CORRECTION = 0x1347; // touche "Correction" du Minitel = backspace

// ---------------- ETATS ----------------
enum EtatJeu : uint8_t {
  ETAT_HOME,
  ETAT_INTRO,
  ETAT_MANCHE_PLAY,
  ETAT_MANCHE_RESULT,
  ETAT_YOUWIN_STATS,
  ETAT_ENTER_NAME,   // saisie du pseudo, utilisee apres victoire ET apres defaite
  ETAT_YOULOOSE,
  ETAT_HIGHSCORES
};
EtatJeu etat = ETAT_HOME;
unsigned long etatSince = 0;

// ---------------- STATS PARTIE ----------------
struct RoundStat {
  unsigned long temps;
  uint8_t erreurs;
  float points;
};
struct GameStat {
  uint8_t manche;
  float score;
  unsigned long bestTime;
  unsigned long averageTime;
  uint8_t vies;
  RoundStat rounds[NB_MANCHES];
};
GameStat gameStat;

// ---------------- HIGHSCORES (session, RAM) ----------------
struct HScore {
  char pseudo[9];
  float score;
  unsigned long bestTime; // meilleur temps de manche (ms) de cette partie
};
HScore highscores[MAX_HIGHSCORES];
uint8_t nbHighscores = 0;
bool persistOK = false; // reflete le succes reel du dernier acces a la NVS (Preferences)
Preferences prefs;
const char* const PREFS_NAMESPACE = "cordial";   // <= 15 caracteres, impose par Preferences
const char* const PREFS_KEY       = "hs";        // cle unique stockant tout le classement

// ---------------- VARIABLES MANCHE EN COURS ----------------
uint8_t taped = 0;
uint8_t scored = 0;
bool roundStarted = false;
unsigned long roundStart = 0;
unsigned long lastBarUpdate = 0;
const char* currentEmail[6];
uint8_t currentEmailLines = 0;

// résultat de manche (pour affichage)
bool lastErrored = false;
unsigned long lastElapsed = 0;
float lastPoints = 0;

// saisie du pseudo (youwin)
char pseudoBuf[9];
uint8_t pseudoLen = 0;

// ---------------- EMAILS (bibliotheque courte, sans accents) ----------------
// Chaque ligne fait au maximum 37 caracteres : l'affichage demarre en
// colonne 2 sur un ecran de 40 colonnes, donc au-dela de 37 caracteres le
// Minitel wrap tout seul sur la ligne suivante et vient percuter le texte
// qu'on y avait prevu -> lignes qui semblent "collees" les unes aux autres.
// Toujours verifier la longueur de chaque ligne ajoutee ici.
const char* const EMAIL_1[] = {
  "Bonjour,",
  "",
  "Offre LAST MINUTE : encore 2 places",
  "pour notre tournage du 12 septembre.",
  "Repondez avant demain 18h pour",
  "beneficier du tarif.",
  "Interesse(e) ? Rappelez-moi vite !"
};
const char* const EMAIL_2[] = {
  "Bonjour,",
  "",
  "Suite a notre echange telephonique,",
  "voici le recapitulatif de votre",
  "commande. Merci de confirmer par",
  "retour de mail avant vendredi.",
  "Bien a vous,"
};
const char* const EMAIL_3[] = {
  "Madame, Monsieur,",
  "",
  "Nous accusons reception de votre",
  "dossier. Un conseiller reviendra vers",
  "vous sous 48h pour finaliser les",
  "modalites du contrat."
};
const char* const EMAIL_4[] = {
  "Salut,",
  "",
  "Petit rappel amical pour le paiement",
  "de la facture n. 4471, en retard de",
  "12 jours. Peux-tu regulariser",
  "rapidement stp ?",
  "Merci d'avance,"
};
const char* const* const EMAILS[] = { EMAIL_1, EMAIL_2, EMAIL_3, EMAIL_4 };
const uint8_t EMAILS_LINES[] = { 7, 7, 6, 7 }; // doit correspondre exactement au nb de lignes de chaque tableau ci-dessus
const uint8_t NB_EMAILS = 4;

// ============================================================
//  UTILITAIRES AFFICHAGE
// ============================================================
void gotoxy(uint8_t x, uint8_t y) {
  minitel.moveCursorXY(x, y); // ADAPTER SI BESOIN (colonne, ligne)
}
void clrLine(uint8_t y) {
  gotoxy(1, y);
  minitel.attributs(FOND_NORMAL); // ADAPTER SI BESOIN
  for (uint8_t i = 0; i < 40; i++) minitel.printChar(' ');
}
void clrZone(uint8_t y1, uint8_t y2) {
  for (uint8_t y = y1; y <= y2; y++) clrLine(y);
}
void centerText(uint8_t y, const char* txt) {
  uint8_t len = strlen(txt);
  uint8_t x = (len < 40) ? (40 - len) / 2 + 1 : 1;
  gotoxy(x, y);
  minitel.print(txt);
}
void beep() {
#if ENABLE_BEEP
  minitel.bip(); // fonction native de la lib (BEL 0x07 + gestion timing)
#endif
}

// ============================================================
//  CLAVIER
// ============================================================
long pollKey() {
  if (Serial2.available() > 0) {
    return (long)minitel.getKeyCode(); // ADAPTER SI BESOIN
  }
  return -1;
}

// ============================================================
//  OUTILS SCORE
// ============================================================
char rankLetter(unsigned long avgMs) {
  if (avgMs < 800)  return 'S'; // "A+"
  if (avgMs < 1400) return 'A';
  if (avgMs < 1800) return 'B';
  if (avgMs < 2200) return 'C';
  if (avgMs < 2800) return 'D';
  if (avgMs < 3600) return 'E';
  return 'F';
}

void dig3(unsigned long ms, char* out) {
  // affiche "1.234"
  unsigned long s = ms / 1000;
  unsigned long r = ms % 1000;
  sprintf(out, "%lu.%03lu", s, r);
}

void insertHighscore(const char* pseudo, float score, unsigned long bestTime) {
  if (nbHighscores < MAX_HIGHSCORES) {
    strncpy(highscores[nbHighscores].pseudo, pseudo, 8);
    highscores[nbHighscores].pseudo[8] = 0;
    highscores[nbHighscores].score = score;
    highscores[nbHighscores].bestTime = bestTime;
    nbHighscores++;
  } else {
    // remplace le plus petit si on est meilleur
    uint8_t worst = 0;
    for (uint8_t i = 1; i < MAX_HIGHSCORES; i++) {
      if (highscores[i].score < highscores[worst].score) worst = i;
    }
    if (score > highscores[worst].score) {
      strncpy(highscores[worst].pseudo, pseudo, 8);
      highscores[worst].pseudo[8] = 0;
      highscores[worst].score = score;
      highscores[worst].bestTime = bestTime;
    }
  }
  // tri desc (petit tableau -> tri a bulles suffit largement)
  for (uint8_t i = 0; i < nbHighscores; i++) {
    for (uint8_t j = i + 1; j < nbHighscores; j++) {
      if (highscores[j].score > highscores[i].score) {
        HScore tmp = highscores[i];
        highscores[i] = highscores[j];
        highscores[j] = tmp;
      }
    }
  }

  saveHighscoresToFlash(); // persiste immediatement, comme les presets de Telnet_Pro
}

// ---------------- PERSISTANCE (Preferences / NVS) ----------------
// Meme objectif que les presets de Minitel1B_Telnet_Pro (survivre aux
// reboots), mais via Preferences (stockage cle/valeur NVS integre a
// l'ESP32) plutot que SPIFFS : ca evite le bug de link connu sur le core
// esp32 2.0.17 ("dangerous relocation ... crosses 1GB boundary" dans
// FS/vfs_api.cpp quand on appelle SPIFFS.begin()). Preferences ne passe
// pas par cette couche VFS/POSIX, donc le probleme ne se pose pas.
// Bonus : pas besoin de choisir un Partition Scheme particulier, la
// partition NVS existe par defaut dans (quasi) tous les schemas.
//
// Format stocke sous la cle PREFS_KEY :
//   [1 octet : nbHighscores] [N x struct HScore (pseudo[9] + float score)]

void loadHighscoresFromFlash() {
  nbHighscores = 0;

  // readOnly=false ici (pas true) : sur le tout premier boot, le namespace
  // n'existe pas encore, et begin(..., true) echouerait alors qu'il n'y a
  // rien d'anormal. begin(..., false) cree le namespace si besoin et donne
  // un vrai signal de succes/echec de la NVS elle-meme.
  persistOK = prefs.begin(PREFS_NAMESPACE, false);
  if (persistOK) {
    size_t len = prefs.getBytesLength(PREFS_KEY);

    if (len >= 1) {
      uint8_t buf[1 + MAX_HIGHSCORES * sizeof(HScore)];
      size_t toRead = min(len, sizeof(buf));
      prefs.getBytes(PREFS_KEY, buf, toRead);

      uint8_t n = buf[0];
      if (n > MAX_HIGHSCORES) n = MAX_HIGHSCORES; // securite si donnees corrompues

      size_t available = (toRead > 1) ? (toRead - 1) : 0;
      size_t maxEntries = available / sizeof(HScore);
      if (n > maxEntries) n = maxEntries; // securite si lecture tronquee

      memcpy(highscores, buf + 1, n * sizeof(HScore));
      nbHighscores = n;
    }
    prefs.end();
  }
}

void saveHighscoresToFlash() {
  uint8_t buf[1 + MAX_HIGHSCORES * sizeof(HScore)];
  buf[0] = nbHighscores;
  memcpy(buf + 1, highscores, nbHighscores * sizeof(HScore));

  persistOK = prefs.begin(PREFS_NAMESPACE, false); // false = lecture/ecriture
  if (persistOK) {
    prefs.putBytes(PREFS_KEY, buf, 1 + nbHighscores * sizeof(HScore));
    prefs.end();
  }
}

void clearHighscoresFlash() {
  nbHighscores = 0;
  persistOK = prefs.begin(PREFS_NAMESPACE, false);
  if (persistOK) {
    prefs.remove(PREFS_KEY);
    prefs.end();
  }
}

// ============================================================
//  CHANGEMENT D'ETAT
// ============================================================
void changeEtat(EtatJeu nouveau);

// ============================================================
//  SETUP / LOOP
// ============================================================
void setup() {
  Serial.begin(115200); // pour les messages de debug eventuels, sans lien avec le Minitel

  minitel.changeSpeed(4800); // ADAPTER SI BESOIN selon ta lib
  // NB : le Minitel 1B ne repond a la commande de changement de vitesse
  // (PRO2 + code vitesse) que s'il la supporte reellement. Certains 1B
  // ne montent pas au-dessus de 1200 bauds -> si l'ecran reste vide ou
  // affiche du charabia apres ce changement, repasse a 1200 :
  //   minitel.changeSpeed(1200);
  // Verifie aussi que la ligne Serial2.begin(...) de ta lib est bien
  // reconfiguree a 4800 7E1 en interne par changeSpeed(), sinon il faut
  // le faire a la main juste apres l'appel.
  minitel.echo(false);       // essentiel, sinon double affichage clavier
  minitel.newScreen();
  minitel.noCursor();
  randomSeed(analogRead(A0));

  loadHighscoresFromFlash(); // NVS s'initialise tout seul, pas de mount a gerer

  changeEtat(ETAT_HOME);
}

void loop() {
  long touche = pollKey();

  switch (etat) {
    case ETAT_HOME:          loopHome(touche); break;
    case ETAT_INTRO:         loopIntro(touche); break;
    case ETAT_MANCHE_PLAY:   loopManchePlay(touche); break;
    case ETAT_MANCHE_RESULT: loopMancheResult(touche); break;
    case ETAT_YOUWIN_STATS:  loopYouWinStats(touche); break;
    case ETAT_ENTER_NAME:    loopEnterName(touche); break;
    case ETAT_YOULOOSE:      loopYouLoose(touche); break;
    case ETAT_HIGHSCORES:    loopHighscores(touche); break;
  }
}

// ============================================================
//  ETAT : HOME
// ============================================================
void drawHome() {
  minitel.newScreen();
  minitel.attributs(DOUBLE_HAUTEUR); // ADAPTER SI BESOIN
  minitel.attributs(CARACTERE_MAGENTA);
  centerText(4, "* * *  C O R D I A L E M E N T  * * *");
  minitel.attributs(GRANDEUR_NORMALE);

  minitel.attributs(CARACTERE_BLANC);
  centerText(8,  "Le celebre jeu de petite frappe");
  centerText(9,  "administrative.");

  minitel.attributs(CARACTERE_JAUNE);
  centerText(12, "Tapez le mot ci-dessous, vite et bien,");
  centerText(13, "avant la fin du chrono.");

  minitel.attributs(CARACTERE_VERT);
  char buf[32];
  sprintf(buf, "%d manches - %d vies", NB_MANCHES, NB_VIES);
  centerText(16, buf);

  minitel.attributs(CARACTERE_CYAN);
  centerText(18, "Appuyez sur ENVOI pour jouer");

  // ligne 19 laissee vide volontairement (respiration avant le credit)

  minitel.attributs(CARACTERE_BLEU);
  centerText(23, "Orig. web : play-fool.net/cordialement");
  centerText(24, "par Thomas Francois & Gildas Paubert");
}

void loopHome(long touche) {
  if (touche == TOUCHE_ENVOI) {
    changeEtat(ETAT_INTRO);
    return;
  }
  if (touche == TOUCHE_CORRECTION) {
    // reset du classement stocke en flash (utile entre 2 events/sessions)
    clearHighscoresFlash();
    drawHome();
    minitel.attributs(CARACTERE_ROUGE);
    centerText(21, "Classement efface."); // lignes 23-24 = credit, on ne les ecrase pas
    return;
  }
  if (millis() - etatSince >= HOME_IDLE_DURATION) {
    // mode "attract" : personne ne joue -> on montre le classement,
    // ca alterne indefiniment avec loopHighscores() ci-dessous.
    changeEtat(ETAT_HIGHSCORES);
  }
}

// ============================================================
//  ETAT : INTRO ("Pret ? Go !")
// ============================================================
void initGame() {
  gameStat.manche = 0;
  gameStat.score = 0;
  gameStat.bestTime = 0;
  gameStat.averageTime = 0;
  gameStat.vies = NB_VIES;
  for (uint8_t i = 0; i < NB_MANCHES; i++) {
    gameStat.rounds[i].temps = 0;
    gameStat.rounds[i].erreurs = 0;
    gameStat.rounds[i].points = 0;
  }
}

void drawIntro() {
  initGame();
  minitel.newScreen();
  minitel.attributs(DOUBLE_GRANDEUR);
  minitel.attributs(CARACTERE_ROUGE);
  centerText(10, "PRET ?");
  minitel.attributs(GRANDEUR_NORMALE);
  minitel.attributs(CARACTERE_BLANC);
  centerText(14, "Cordialement, vite !");
  beep();
}

void loopIntro(long touche) {
  unsigned long dt = millis() - etatSince;
  if (dt > INTRO_DURATION / 2 && dt < INTRO_DURATION / 2 + 50) {
    minitel.attributs(DOUBLE_GRANDEUR);
    minitel.attributs(CARACTERE_JAUNE);
    centerText(10, "GO !  ");
    minitel.attributs(GRANDEUR_NORMALE);
    beep();
  }
  if (dt >= INTRO_DURATION) {
    changeEtat(ETAT_MANCHE_PLAY);
  }
}

// ============================================================
//  ETAT : MANCHE - PLAY
// ============================================================
void drawGameFrame() {
  minitel.newScreen();

  // HUD ligne 1
  minitel.attributs(CARACTERE_BLANC);
  char buf[40];
  sprintf(buf, "Manche %d/%d", gameStat.manche, NB_MANCHES);
  gotoxy(1, 1); minitel.print(buf);

  minitel.attributs(CARACTERE_MAGENTA);
  gotoxy(20, 1);
  minitel.print("Vies:");
  for (uint8_t i = 0; i < NB_VIES; i++) {
    minitel.attributs(i < gameStat.vies ? CARACTERE_ROUGE : CARACTERE_NOIR);
    minitel.printChar(i < gameStat.vies ? '#' : '.'); // '#' visible en G0, (char)3 etait un code de controle invisible
  }

  minitel.attributs(CARACTERE_JAUNE);
  gotoxy(1, 2);
  char sbuf[24];
  dtostrf(gameStat.score, 0, 1, sbuf);
  sprintf(buf, "Score: %s pts", sbuf);
  minitel.print(buf);

  // email
  uint8_t idx = random(0, NB_EMAILS);
  currentEmailLines = EMAILS_LINES[idx];
  minitel.attributs(CARACTERE_BLANC);
  for (uint8_t i = 0; i < currentEmailLines; i++) {
    gotoxy(2, 4 + i);
    minitel.print(EMAILS[idx][i]);
  }

  minitel.attributs(CARACTERE_CYAN);
  gotoxy(2, 12);
  minitel.print("Signature du mail :");

  // ligne de saisie
  minitel.attributs(CARACTERE_BLANC);
  gotoxy(2, 20);
  minitel.print("Tapez : cordialement");

  drawTimerBar(1.0);
  redrawTyped();
}

void drawTimerBar(float ratio) {
  if (ratio < 0) ratio = 0;
  uint8_t total = 38;
  uint8_t filled = (uint8_t)(ratio * total);
  gotoxy(1, 22);
  minitel.attributs(FOND_JAUNE); // ADAPTER SI BESOIN
  for (uint8_t i = 0; i < filled; i++) minitel.printChar(' ');
  minitel.attributs(FOND_NORMAL);
  for (uint8_t i = filled; i < total; i++) minitel.printChar(' ');
}

void redrawTyped() {
  gotoxy(2, 21);
  minitel.attributs(CARACTERE_BLANC);
  for (uint8_t i = 0; i < WORD_LEN; i++) {
    minitel.printChar('.');
  }
}

void printTypedChar(uint8_t pos, char c, bool ok) {
  gotoxy(2 + pos, 21);
  minitel.attributs(ok ? CARACTERE_VERT : CARACTERE_MAGENTA);
  minitel.printChar((char)toupper(c));
}

void startManche() {
  gameStat.manche++;
  taped = 0;
  scored = 0;
  roundStarted = false;
  lastBarUpdate = 0;
  drawGameFrame();
}

void loopManchePlay(long touche) {
  // timeout dur (rien tape a temps)
  if (roundStarted) {
    unsigned long elapsed = millis() - roundStart;
    if (elapsed >= MANCHE_DURATION) {
      validateRound(true, MANCHE_DURATION);
      return;
    }
    if (millis() - lastBarUpdate > 150) {
      lastBarUpdate = millis();
      float ratio = 1.0f - (float)elapsed / (float)MANCHE_DURATION;
      drawTimerBar(ratio);
    }
  }

  if (touche < 0) return;

  if (touche == TOUCHE_ENVOI) {
    // validation manuelle (autorisee a tout moment, comme dans l'original)
    unsigned long elapsed = roundStarted ? (millis() - roundStart) : 0;
    validateRound(scored != WORD_LEN, elapsed);
    return;
  }

  char c = (char)touche;
  if (c >= 'A' && c <= 'Z') c = tolower(c);
  if (c < 'a' || c > 'z') return; // touche non pertinente

  if (taped >= WORD_LEN) return; // deja tout tape, on attend ENVOI/timeout

  if (taped == 0) {
    roundStarted = true;
    roundStart = millis();
  }

  bool ok = (c == WORD[taped]);
  if (ok) {
    scored++;
  } else {
    beep();
  }
  printTypedChar(taped, c, ok);
  taped++;

  if (taped >= WORD_LEN) {
    unsigned long elapsed = millis() - roundStart;
    validateRound(scored != WORD_LEN, elapsed);
  }
}

void validateRound(bool errored, unsigned long elapsedIn) {
  unsigned long elapsed = errored ? ERROR_MALUS : elapsedIn;

  if (gameStat.bestTime == 0) gameStat.bestTime = elapsed;
  else gameStat.bestTime = min(gameStat.bestTime, elapsed);

  unsigned long sum = elapsed;
  uint8_t n = 1;
  for (uint8_t i = 0; i < gameStat.manche - 1; i++) {
    sum += gameStat.rounds[i].temps;
    n++;
  }
  gameStat.averageTime = sum / n;

  float points = 0;
  if (!errored) {
    long restant = (long)MANCHE_DURATION - (long)elapsed;
    float base = (float)restant / (float)(MANCHE_DURATION - PERFECT_DURATION) * 10.0f;
    if (base > 10) base = 10;
    points = base - (WORD_LEN - scored);
    if (points < 0) points = 0;
    points = round(points * 2) / 2.0f; // pas de 0.5
    gameStat.score += points;
  } else {
    gameStat.vies--;
  }

  if (gameStat.manche - 1 < NB_MANCHES) {
    gameStat.rounds[gameStat.manche - 1].temps = elapsed;
    gameStat.rounds[gameStat.manche - 1].erreurs = WORD_LEN - scored;
    gameStat.rounds[gameStat.manche - 1].points = points;
  }

  lastErrored = errored;
  lastElapsed = elapsed;
  lastPoints = points;

  changeEtat(ETAT_MANCHE_RESULT);
}

// ============================================================
//  ETAT : RESULTAT DE MANCHE
// ============================================================
void drawResult() {
  clrZone(9, 16);
  minitel.attributs(FOND_BLEU); // ADAPTER SI BESOIN : re-appliquer par ligne si mosaique
  for (uint8_t y = 9; y <= 16; y++) { gotoxy(2, y); for (uint8_t i=0;i<36;i++) minitel.printChar(' '); }

  char tbuf[16];
  dig3(lastElapsed, tbuf);

  if (!lastErrored) {
    minitel.attributs(CARACTERE_JAUNE);
    if (scored == WORD_LEN) {
      centerText(11, "PARFAIT !");
    }
    minitel.attributs(CARACTERE_BLANC);
    char buf[32];
    sprintf(buf, "%s sec", tbuf);
    centerText(13, buf);
    char pbuf[24];
    char psbuf[16];
    dtostrf(lastPoints, 0, 1, psbuf);
    sprintf(pbuf, "%s points !", psbuf);
    centerText(14, pbuf);
    beep();
  } else {
    minitel.attributs(CARACTERE_MAGENTA);
    if (gameStat.vies <= 0) {
      centerText(11, "GAME OVER");
    } else {
      centerText(11, "RATE !");
    }
    minitel.attributs(CARACTERE_BLANC);
    char buf[40];
    sprintf(buf, "%d erreurs / %d lettres", WORD_LEN - scored, taped);
    centerText(13, buf);
    centerText(14, "0 point.");
    beep();
  }

  minitel.attributs(FOND_NORMAL);
  minitel.attributs(CARACTERE_CYAN);
  centerText(18, "(ENVOI pour continuer)");
}

void loopMancheResult(long touche) {
  bool go = (touche == TOUCHE_ENVOI) || (millis() - etatSince >= RESULT_DURATION);
  if (!go) return;

  if (gameStat.vies <= 0) {
    changeEtat(ETAT_YOULOOSE);
  } else if (gameStat.manche >= NB_MANCHES) {
    changeEtat(ETAT_YOUWIN_STATS);
  } else {
    changeEtat(ETAT_MANCHE_PLAY); // -> startManche() via changeEtat
  }
}

// ============================================================
//  ETAT : YOUWIN
// ============================================================
void drawYouWinStats() {
  minitel.newScreen();
  minitel.attributs(DOUBLE_HAUTEUR);
  minitel.attributs(CARACTERE_MAGENTA);
  centerText(2, "YOU WIN !");
  minitel.attributs(GRANDEUR_NORMALE);

  char rk = rankLetter(gameStat.averageTime);
  minitel.attributs(CARACTERE_JAUNE);
  char buf[32];
  sprintf(buf, "Rang : %c", rk);
  centerText(5, buf);

  char tbuf1[16], tbuf2[16];
  dig3(gameStat.bestTime, tbuf1);
  dig3(gameStat.averageTime, tbuf2);

  minitel.attributs(CARACTERE_BLANC);
  sprintf(buf, "Meilleur temps  : %s sec", tbuf1);
  centerText(8, buf);
  sprintf(buf, "Temps moyen     : %s sec", tbuf2);
  centerText(9, buf);
  char scbuf[16];
  dtostrf(gameStat.score, 0, 1, scbuf);
  sprintf(buf, "Score final     : %s pts", scbuf);
  centerText(10, buf);

  // detail manches (compact, 2 colonnes)
  minitel.attributs(CARACTERE_CYAN);
  for (uint8_t i = 0; i < NB_MANCHES; i++) {
    uint8_t col = (i < 5) ? 2 : 21;
    uint8_t row = 13 + (i % 5);
    gotoxy(col, row);
    char lbuf[24];
    char t3[16];
    dig3(gameStat.rounds[i].temps, t3);
    sprintf(lbuf, "%2d %5ss %2de", i + 1, t3, gameStat.rounds[i].erreurs);
    minitel.print(lbuf);
  }

  minitel.attributs(CARACTERE_JAUNE);
  centerText(20, "(ENVOI pour continuer)");
}

void loopYouWinStats(long touche) {
  if (touche == TOUCHE_ENVOI || (millis() - etatSince >= ENDGAME_DURATION)) {
    changeEtat(ETAT_ENTER_NAME);
  }
}

void drawEnterName() {
  pseudoLen = 0;
  pseudoBuf[0] = 0;
  minitel.newScreen();
  minitel.attributs(CARACTERE_BLANC);
  centerText(8, "Entrez votre nom pour le");
  centerText(9, "classement de la session :");

  minitel.attributs(CARACTERE_JAUNE);
  gotoxy(15, 12);
  minitel.print("________");

  minitel.attributs(CARACTERE_CYAN);
  centerText(16, "(ENVOI pour valider)");
  centerText(17, "(CORRECTION pour effacer)");
}

void loopEnterName(long touche) {
  if (touche < 0) return;

  if (touche == TOUCHE_ENVOI) {
    if (pseudoLen == 0) strcpy(pseudoBuf, "ANONYME");
    insertHighscore(pseudoBuf, gameStat.score, gameStat.bestTime);
    changeEtat(ETAT_HIGHSCORES);
    return;
  }

  if (touche == TOUCHE_CORRECTION) {
    if (pseudoLen > 0) {
      pseudoLen--;
      pseudoBuf[pseudoLen] = 0;
      gotoxy(15 + pseudoLen, 12);
      minitel.attributs(CARACTERE_JAUNE);
      minitel.printChar('_');
    }
    return;
  }

  char c = (char)touche;
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-') {
    if (pseudoLen < 8) {
      pseudoBuf[pseudoLen++] = c;
      pseudoBuf[pseudoLen] = 0;
      gotoxy(15 + pseudoLen - 1, 12);
      minitel.attributs(CARACTERE_JAUNE);
      minitel.printChar(c);
    }
  }
}

// ============================================================
//  ETAT : YOULOOSE
// ============================================================
void drawYouLoose() {
  minitel.newScreen();
  minitel.attributs(DOUBLE_HAUTEUR);
  minitel.attributs(CARACTERE_ROUGE);
  centerText(3, "GAME OVER");
  minitel.attributs(GRANDEUR_NORMALE);
  minitel.attributs(CARACTERE_BLANC);
  centerText(6, "Vous avez perdu.");

  char buf[32], t1[16], t2[16], sc[16];
  dig3(gameStat.bestTime, t1);
  dig3(gameStat.averageTime, t2);
  dtostrf(gameStat.score, 0, 1, sc);

  minitel.attributs(CARACTERE_JAUNE);
  sprintf(buf, "Meilleur temps : %s sec", t1);
  centerText(10, buf);
  sprintf(buf, "Temps moyen    : %s sec", t2);
  centerText(11, buf);
  sprintf(buf, "Score          : %s pts", sc);
  centerText(12, buf);

  minitel.attributs(CARACTERE_CYAN);
  centerText(18, "(ENVOI pour continuer)");
  beep();
}

void loopYouLoose(long touche) {
  if (touche == TOUCHE_ENVOI || (millis() - etatSince >= ENDGAME_DURATION)) {
    changeEtat(ETAT_ENTER_NAME);
  }
}

// ============================================================
//  ETAT : HIGHSCORES (session)
// ============================================================
void drawHighscores() {
  minitel.newScreen();
  minitel.attributs(DOUBLE_HAUTEUR);
  minitel.attributs(CARACTERE_MAGENTA);
  centerText(2, "HIGHSCORES");
  minitel.attributs(GRANDEUR_NORMALE);
  minitel.attributs(CARACTERE_CYAN);
  if (persistOK) {
    centerText(4, "(classement sauvegarde en memoire)");
  } else {
    centerText(4, "(non sauvegarde - session seulement)");
  }

  minitel.attributs(CARACTERE_BLANC);
  if (nbHighscores == 0) {
    centerText(10, "Aucun score pour l'instant.");
  } else {
    minitel.attributs(CARACTERE_VERT);
    // Alignement calque sur le format de sprintf() des lignes de donnees
    // ("%-4s%-8s %13ss %6s" a partir de la colonne 6) :
    //   rang  -> colonnes 6-9   (chiffre(s)+"." aligne a gauche, dans un
    //                            champ de 4 caracteres -> le chiffre est
    //                            toujours en colonne 6, meme a 1 chiffre)
    //   nom   -> colonnes 10-18 (8 chars + " ")
    //   temps -> colonnes 19-33 (13 chars + "s " -> assez large pour
    //                            "Meilleur temps" en entete, 14 chars)
    //   score -> colonnes 34-39 (6 chars, pas de suffixe "pts" : l'entete
    //                            "Score" suffit, ca degage la place
    //                            necessaire pour l'entete du temps)
    gotoxy(6, 6);  minitel.print("#");
    gotoxy(10, 6); minitel.print("Nom");
    gotoxy(19, 6); minitel.print("Meilleur temps");
    gotoxy(34, 6); minitel.print("Score");

    for (uint8_t i = 0; i < nbHighscores; i++) {
      char buf[40];
      char sc[16];
      char tbuf[16];
      char rankbuf[6];
      dtostrf(highscores[i].score, 0, 1, sc);
      dig3(highscores[i].bestTime, tbuf);
      sprintf(rankbuf, "%d.", i + 1); // "1." .. "10." (pas de padding a gauche)
      sprintf(buf, "%-4s%-8s %13ss %6s", rankbuf, highscores[i].pseudo, tbuf, sc);
      gotoxy(6, 7 + i);
      minitel.attributs((i == 0) ? CARACTERE_JAUNE : CARACTERE_BLANC);
      minitel.print(buf);
    }
  }

  minitel.attributs(CARACTERE_CYAN);
  centerText(20, "(ENVOI pour rejouer)");
}

void loopHighscores(long touche) {
  if (touche == TOUCHE_ENVOI) {
    // "appuyez sur start" depuis n'importe quel ecran d'accueil/attract
    changeEtat(ETAT_INTRO);
    return;
  }
  if (millis() - etatSince >= HIGHSCORE_DURATION) {
    changeEtat(ETAT_HOME); // referme la boucle attract (home <-> highscores)
  }
}

// ============================================================
//  DISPATCH changeEtat
// ============================================================
void changeEtat(EtatJeu nouveau) {
  etat = nouveau;
  etatSince = millis();
  switch (etat) {
    case ETAT_HOME:          drawHome(); break;
    case ETAT_INTRO:         drawIntro(); break;
    case ETAT_MANCHE_PLAY:   startManche(); break;
    case ETAT_MANCHE_RESULT: drawResult(); break;
    case ETAT_YOUWIN_STATS:  drawYouWinStats(); break;
    case ETAT_ENTER_NAME:    drawEnterName(); break;
    case ETAT_YOULOOSE:      drawYouLoose(); break;
    case ETAT_HIGHSCORES:    drawHighscores(); break;
  }
}
