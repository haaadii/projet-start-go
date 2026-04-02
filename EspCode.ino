#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <HardwareSerial.h>
#include <DFRobotDFPlayerMini.h>

// Identifiants du service Bluetooth Low Energy
// Ces trois chaînes sont des UUID, c'est-à-dire des identifiants
// uniques qui permettent au téléphone de reconnaître le bon appareil
// et de savoir comment lui parler
#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CMD_UUID     "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define EVT_UUID     "1c95d5e3-d8f7-413a-bf3d-7a2e5d7be87e"

// Brochages : D12 reçoit les données du DFPlayer, D11 lui en envoie
// La résistance de 1kΩ est obligatoire sur D11 pour ne pas griller le module
#define BROCHE_RX  D12
#define BROCHE_TX  D11

// Numéros de pistes sur la carte SD
// Le DFPlayer lit les fichiers dans le dossier /mp3
// 0001.mp3, 0002.mp3, 0003.mp3, 0004.mp3
#define SON_DEMARRAGE   1
#define SON_CONNEXION   2
#define SON_BIP_COURSE  3
#define SON_TEST        4

// Volumes : les sons système sont fixes et discrets (10/30)
// Le son de course est réglable depuis l'application
#define VOLUME_SYSTEME  10
#define VOLUME_DEFAUT   25

// La séquence de départ passe par quatre états distincts
// IDLE = en attente, CONCENTRATION = l'athlète se prépare
// DELAI_ALEA = on attend un moment imprévisible, TERMINE = le bip a sonné
enum Etat { IDLE, CONCENTRATION, DELAI_ALEA, TERMINE };
volatile Etat etatActuel = IDLE;

// Variables pour mesurer le temps de chaque phase
unsigned long debutPhase    = 0;
unsigned long dureeConc     = 3000;
unsigned long delaiMin      = 1000;
unsigned long delaiMax      = 3000;
unsigned long delaiTire     = 0;

// Objets Bluetooth
BLEServer*          serveurBLE  = nullptr;
BLECharacteristic*  caracterEVT = nullptr;
bool telephoneConnecte = false;

// Objet pour parler au DFPlayer via le port série matériel numéro 1
HardwareSerial      serialDF(1);
DFRobotDFPlayerMini dfplayer;
bool moduleSON = false;
int  volumeCourse = VOLUME_DEFAUT;

// File d'attente pour les commandes son
// Le DFPlayer ne peut pas recevoir deux ordres trop rapprochés
// On stocke donc les sons à jouer et on les envoie un par un
struct CommandeSon { int piste; int volume; };
CommandeSon fileSons[4];
int fileTete = 0, fileQueue = 0;
unsigned long derniereSon = 0;
#define INTERVALLE_MIN_SON 300

void ajouterSon(int piste, int volume) {
  int suivant = (fileQueue + 1) % 4;
  if (suivant == fileTete) return; // file pleine, on laisse tomber
  fileSons[fileQueue] = {piste, volume};
  fileQueue = suivant;
}

void traiterFileSons() {
  if (fileTete == fileQueue) return; // rien à jouer
  if (!moduleSON) { fileTete = fileQueue; return; }
  if (millis() - derniereSon < INTERVALLE_MIN_SON) return; // trop tôt

  CommandeSon c = fileSons[fileTete];
  fileTete = (fileTete + 1) % 4;

  dfplayer.volume(c.volume);
  delay(20); // petit délai indispensable entre volume et play
  dfplayer.play(c.piste);
  derniereSon = millis();
}

// Jouer un son système (démarrage, connexion) toujours au même volume faible
void jouerSonSysteme(int piste) {
  ajouterSon(piste, VOLUME_SYSTEME);
}

// Jouer un son de course au volume choisi par l'utilisateur
void jouerSonCourse(int piste) {
  ajouterSon(piste, volumeCourse);
}

// Envoyer un message au téléphone via Bluetooth
void envoyerMessage(const String& msg) {
  if (!telephoneConnecte || !caracterEVT) return;
  caracterEVT->setValue(msg.c_str());
  caracterEVT->notify();
  Serial.println(">> " + msg);
}

// Analyse et exécution des commandes reçues depuis l'application
void traiterCommande(const String& brut) {
  String cmd = brut;
  cmd.trim();
  Serial.println("<< " + cmd);

  // Lancement d'une séquence de départ
  // Format : START:dureeConc:delaiMin:delaiMax (en millisecondes)
  if (cmd.startsWith("START:")) {
    if (etatActuel != IDLE) { envoyerMessage("ERR:BUSY"); return; }

    int a = cmd.indexOf(':', 6);
    int b = (a > 0) ? cmd.indexOf(':', a + 1) : -1;

    if (a > 0 && b > 0) {
      dureeConc  = (unsigned long)cmd.substring(6, a).toInt();
      delaiMin   = (unsigned long)cmd.substring(a + 1, b).toInt();
      delaiMax   = (unsigned long)cmd.substring(b + 1).toInt();
    } else {
      dureeConc = 3000; delaiMin = 1000; delaiMax = 3000;
    }

    // Sécurités pour éviter des valeurs absurdes
    if (dureeConc < 500)       dureeConc = 500;
    if (delaiMin  < 300)       delaiMin  = 300;
    if (delaiMax  <= delaiMin) delaiMax  = delaiMin + 500;

    // On tire au sort le délai aléatoire maintenant
    // pour qu'il soit imprévisible même pour le programme
    delaiTire  = (unsigned long)random((long)delaiMin, (long)delaiMax + 1);
    debutPhase = millis();
    etatActuel = CONCENTRATION;

    envoyerMessage("PHASE:WAIT");
    Serial.println("[SEQ] depart conc=" + String(dureeConc) + "ms delai=" + String(delaiTire) + "ms");
    return;
  }

  // Changement de volume pour les sons de course
  if (cmd.startsWith("VOL:")) {
    volumeCourse = constrain(cmd.substring(4).toInt(), 0, 30);
    envoyerMessage("VOL_OK:" + String(volumeCourse));
    return;
  }

  // Annulation en cours de séquence (faux départ détecté côté app)
  if (cmd == "ABORT") {
    etatActuel = IDLE;
    envoyerMessage("ABORTED");
    return;
  }

  // Fin de session, on remet le système en attente
  if (cmd == "FINISH") {
    etatActuel = IDLE;
    envoyerMessage("FINISH_ACK");
    return;
  }

  // Test du son depuis les paramètres de l'application
  if (cmd == "TEST_BIP") {
    if (etatActuel != IDLE) { envoyerMessage("ERR:BUSY"); return; }
    jouerSonCourse(SON_TEST);
    envoyerMessage("TEST_BIP_OK");
    return;
  }

  // Vérification de connexion simple
  if (cmd == "PING") {
    envoyerMessage("PONG:vol=" + String(volumeCourse) + ":son=" + String(moduleSON ? 1 : 0));
    return;
  }
}

// Ce qui se passe quand le téléphone se connecte
class GestionnaireConnexion : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    telephoneConnecte = true;
    Serial.println("[BLE] telephone connecte");
    delay(200); // on laisse le temps à la connexion de se stabiliser
    envoyerMessage("CONNECTED:vol=" + String(volumeCourse));
    jouerSonSysteme(SON_CONNEXION);
  }
  void onDisconnect(BLEServer* s) override {
    telephoneConnecte = false;
    etatActuel = IDLE;
    Serial.println("[BLE] telephone deconnecte, relance...");
    delay(300);
    s->startAdvertising(); // on redevient visible pour se reconnecter
  }
};

// Ce qui se passe quand l'application envoie une commande
class GestionnaireCommandes : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    traiterCommande(String(c->getValue().c_str()));
  }
};

// Initialisation du module son DFPlayer
bool demarrerDFPlayer() {
  Serial.print("[SON] initialisation... ");
  serialDF.begin(9600, SERIAL_8N1, BROCHE_RX, BROCHE_TX);
  delay(1000); // le DFPlayer est lent à démarrer, surtout avec une carte SD

  if (!dfplayer.begin(serialDF, true, true)) {
    Serial.println("echec");
    Serial.println("  -> verifier le cablage, la carte SD (FAT32)");
    Serial.println("  -> dossier /mp3 avec 0001.mp3 a 0004.mp3");
    return false;
  }

  dfplayer.setTimeOut(500);
  dfplayer.EQ(DFPLAYER_EQ_NORMAL);
  dfplayer.outputDevice(DFPLAYER_DEVICE_SD);
  dfplayer.volume(VOLUME_SYSTEME);

  delay(200);
  int nbFichiers = dfplayer.readFileCounts(DFPLAYER_DEVICE_SD);
  Serial.println("ok - " + String(nbFichiers) + " fichier(s) detecte(s)");

  return true;
}

// =============================================================
// SETUP : s'exécute une seule fois au démarrage
// =============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== StartGo Pro ===");

  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_RED, HIGH);

  // Graine aléatoire basée sur une valeur analogique flottante
  // pour que le délai soit vraiment imprévisible à chaque fois
  randomSeed(analogRead(A0) ^ (unsigned long)micros());

  // Démarrage du module son
  moduleSON = demarrerDFPlayer();
  if (moduleSON) {
    // Son de démarrage joué directement, sans passer par la file
    // pour s'assurer qu'il parte bien dès l'allumage
    dfplayer.volume(VOLUME_SYSTEME);
    delay(20);
    dfplayer.play(SON_DEMARRAGE);
    derniereSon = millis();
    Serial.println("[SON] son de demarrage lance");
  }

  // Initialisation du Bluetooth Low Energy
  Serial.print("[BLE] initialisation... ");
  BLEDevice::init("StartGo-Pro");
  serveurBLE = BLEDevice::createServer();
  serveurBLE->setCallbacks(new GestionnaireConnexion());

  BLEService* service = serveurBLE->createService(SERVICE_UUID);

  // Caractéristique pour recevoir les commandes du téléphone
  BLECharacteristic* caracterCMD = service->createCharacteristic(
    CMD_UUID,
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_WRITE_NR
  );
  caracterCMD->setCallbacks(new GestionnaireCommandes());

  // Caractéristique pour envoyer des événements vers le téléphone
  caracterEVT = service->createCharacteristic(
    EVT_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  caracterEVT->addDescriptor(new BLE2902());

  service->start();

  BLEAdvertising* pub = BLEDevice::getAdvertising();
  pub->addServiceUUID(SERVICE_UUID);
  pub->setScanResponse(true);
  pub->setMinPreferred(0x06);
  BLEDevice::startAdvertising();
  Serial.println("ok - visible sous le nom StartGo-Pro");

  // Clignotement de confirmation : tout est prêt
  for (int i = 0; i < 5; i++) {
    digitalWrite(LED_RED, LOW);  delay(80);
    digitalWrite(LED_RED, HIGH); delay(80);
  }

  Serial.println("[PRET] en attente de connexion Bluetooth");
}

// =============================================================
// LOOP : tourne en boucle en permanence
// =============================================================
void loop() {
  unsigned long maintenant = millis();

  // On vérifie à chaque tour si un son est en attente d'être joué
  traiterFileSons();

  // Machine à états : gestion de la séquence de départ
  switch (etatActuel) {

    case CONCENTRATION:
      // On attend que le temps de concentration soit écoulé
      if (maintenant - debutPhase >= dureeConc) {
        debutPhase = maintenant;
        etatActuel = DELAI_ALEA;
        envoyerMessage("PHASE:ALEA");
        Serial.println("[SEQ] phase aleatoire " + String(delaiTire) + "ms");
      }
      break;

    case DELAI_ALEA:
      // On attend le délai tiré au sort, puis on lance le bip
      if (maintenant - debutPhase >= delaiTire) {
        etatActuel = TERMINE;
        jouerSonCourse(SON_BIP_COURSE);
        envoyerMessage("BIP:" + String(maintenant));
        Serial.println("[SEQ] BIP !");
        // Flash LED pour indiquer visuellement le départ
        digitalWrite(LED_RED, LOW);
        delay(100);
        digitalWrite(LED_RED, HIGH);
      }
      break;

    case TERMINE:
      // On attend que l'application envoie FINISH ou ABORT
      break;

    default:
      break;
  }

  delay(5);
}
