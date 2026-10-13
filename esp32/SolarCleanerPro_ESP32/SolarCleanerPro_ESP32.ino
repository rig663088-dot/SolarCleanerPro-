/*
  SolarCleanerPro v2 · ESP32-S3
  Autores: Leon Mena, Ángel Joel · Gallegos Valencia, Cristian Augusto
  ---------------------------------------------------------------
  El ESP32 crea su PROPIA red WiFi (punto de acceso). No usa router
  ni internet ni nube. La app se conecta a http://192.168.4.1

  Placa: ESP32S3 Dev Module (ESP32-S3-DevKitC-1) · Núcleo Arduino-ESP32 3.x
  Librerías: solo las incluidas en el núcleo (WiFi, WebServer).

  ¡OJO! Revisa la sección PINES y RELÉS contra tu montaje antes de
  conectar motores y bomba. Prueba primero con la fuente ATX apagada.
*/
#include <WiFi.h>
#include <WebServer.h>

// ===================== RED PROPIA =====================
const char* AP_SSID  = "SolarCleanerPro";
const char* AP_PASS  = "solar1234";   // CÁMBIALA (mínimo 8 caracteres)
const int   AP_CANAL = 6;
const int   AP_MAX_CLIENTES = 2;

// ===================== PINES (según diagrama) =====================
const int PIN_FIN_DER   = 6;    // final de carrera derecho  (NO a GND)
const int PIN_FIN_IZQ   = 7;    // final de carrera izquierdo (NO a GND)
const int PIN_K1_BOMBA  = 18;   // relé K1: bomba
const int PIN_K2_FUENTE = 5;    // relé K2: fuente ATX
const int PIN_INICIO    = 10;   // pulsador de la caja: inicia el ciclo (NO a GND)
const int PIN_PARO      = 11;   // hongo de emergencia: contacto NC a GND (cerrado = normal)
// Si todavía no conectaste el hongo, pon un puente entre GPIO11 y GND
// (o USAR_HONGO = false SOLO para pruebas sobre la mesa).
const bool USAR_HONGO   = true;
// BTS7960 #1 (motor 1) y #2 (motor 2). SUPUESTO: confirma cuál es cuál.
const int PIN_RPWM1 = 4;
const int PIN_LPWM1 = 21;
const int PIN_RPWM2 = 15;
const int PIN_LPWM2 = 16;

// Módulo de relés: la mayoría es de activación en BAJO (IN=LOW enciende).
const bool RELE_ACTIVO_BAJO = true;

// ===================== PARÁMETROS DEL CICLO =====================
// Son los valores del programa. La app los lee al conectarse y puede cambiarlos
// mientras el sistema está en espera. Al reiniciar el ESP32 vuelven a estos valores:
// cuando encuentres los definitivos, cópialos aquí (la app los genera con
// «Copiar parámetros para el código»). Mantén los nombres para que la app los reconozca.
float T_FUENTE_S          = 1.0;      // espera tras encender la fuente
float T_PRELAVADO_S       = 5.0;      // bomba antes de mover
float T_PAUSA_S           = 0.4;      // pausa antes de invertir (puente H). Mínimo 0.2
float T_FIN_S             = 1.5;      // espera final antes de apagar
float T_RAMPA_S           = 0.4;      // arranque suave de los motores
float T_MAX_MOV_S         = 120.0;    // PROTECCIÓN: máx. movimiento continuo
float T_MAX_BOMBA_S       = 600.0;    // PROTECCIÓN: máx. bomba encendida
int   pwmVel              = 200;      // 80-255 velocidad de los motores
bool  BOMBA_EN_RECORRIDO  = true;     // true: bomba hasta el final · false: solo prelavado
int   VUELTAS_POR_DEFECTO = 3;        // vueltas del pulsador de la caja

const uint32_t PWM_HZ      = 20000;
const uint32_t T_ENLACE_MS = 3000;    // sin app: se detiene el modo manual
const uint32_t DEBOUNCE_MS = 20;

// ===================== ESTADO =====================
enum Fase { F_REPOSO, F_FUENTE, F_PRELAVADO, F_DER, F_PAUSA, F_IZQ, F_FIN, F_ERROR, F_EMERG };
Fase  fase = F_REPOSO, proxFase = F_REPOSO;
int   proxDir = 0;
uint32_t tFase = 0;

bool  k1 = false, k2 = false;
int   dirObj = 0, dirApl = 0;
float duty = 0;
uint32_t tParo = 0, tMovIni = 0, tK1On = 0;
float pos = 0;                 // 0 = izquierda, 100 = derecha (estimada por tiempo)
float tPasada = 45.0;          // s de un extremo al otro (se calibra sola)
bool  desdeExtremo = false;
bool  manual = false;
bool  escPend = false;         // escobilla encendida, esperando a la fuente
int   dirEsc = 0;
uint32_t tEsc = 0;
int   laps = 0, tot = VUELTAS_POR_DEFECTO;
String err = "";
uint32_t lastSeen = 0;

struct Fin { int pin; bool est; bool crudo; uint32_t t; };
Fin finD = { PIN_FIN_DER, false, false, 0 };
Fin finI = { PIN_FIN_IZQ, false, false, 0 };
Fin btnIni = { PIN_INICIO, false, false, 0 };    // est = presionado
Fin hongo  = { PIN_PARO,   false, false, 0 };    // est = circuito cerrado (normal)
bool prevInicio = false;

// Registro circular de eventos
struct Ev { uint32_t id; char m[80]; };
Ev evs[8];
uint32_t evN = 0;

WebServer server(80);

// ===================== UTILIDADES =====================
void evento(const char* m) {
  evN++;
  Ev &e = evs[evN % 8];
  e.id = evN;
  strncpy(e.m, m, sizeof(e.m) - 1);
  e.m[sizeof(e.m) - 1] = 0;
  Serial.println(m);
}

void rele(int pin, bool on) {
  digitalWrite(pin, RELE_ACTIVO_BAJO ? !on : on);
}

const int PINES_PWM[4] = { PIN_RPWM1, PIN_LPWM1, PIN_RPWM2, PIN_LPWM2 };

void pwmInit() {
  for (int i = 0; i < 4; i++) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(PINES_PWM[i], PWM_HZ, 8);
    ledcWrite(PINES_PWM[i], 0);
#else
    ledcSetup(i, PWM_HZ, 8);
    ledcAttachPin(PINES_PWM[i], i);
    ledcWrite(i, 0);
#endif
  }
}

void pwmEsc(int i, int d) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PINES_PWM[i], d);
#else
  ledcWrite(i, d);
#endif
}

void salidaMotores(int dir, int d) {
  // dir > 0: derecha (RPWM) · dir < 0: izquierda (LPWM). Nunca los dos a la vez.
  pwmEsc(0, dir > 0 ? d : 0);  pwmEsc(1, dir < 0 ? d : 0);
  pwmEsc(2, dir > 0 ? d : 0);  pwmEsc(3, dir < 0 ? d : 0);
}

void setK1(bool v) {
  if (v && !k1) tK1On = millis();
  k1 = v; rele(PIN_K1_BOMBA, v);
}
void setK2(bool v) { k2 = v; rele(PIN_K2_FUENTE, v); }

void salidasOff() {
  dirObj = 0; dirApl = 0; duty = 0; manual = false; escPend = false;
  salidaMotores(0, 0);
  setK1(false);
  setK2(false);
}

void apagarTodo() {
  salidasOff();
  fase = F_REPOSO;
}

void fallo(const char* msg) {
  apagarTodo();
  fase = F_ERROR;
  err = msg;
  evento(msg);
}

void leerFin(Fin &f) {
  bool lectura = (digitalRead(f.pin) == LOW);   // presionado = LOW
  if (lectura != f.crudo) { f.crudo = lectura; f.t = millis(); }
  if (millis() - f.t >= DEBOUNCE_MS) f.est = f.crudo;
}

// ===================== MOTORES =====================
void motorLoop(float dt) {
  int obj = (k2 && dirObj != 0) ? dirObj : 0;
  if (obj > 0 && finD.est) obj = 0;
  if (obj < 0 && finI.est) obj = 0;

  if (obj != dirApl) {
    if (dirApl != 0) {                       // primero parar
      dirApl = 0; duty = 0; tParo = millis();
      salidaMotores(0, 0);
    } else if (millis() - tParo >= (uint32_t)(T_PAUSA_S * 1000)) {
      dirApl = obj; duty = 0;
      tMovIni = millis();
      desdeExtremo = (obj > 0 && finI.est) || (obj < 0 && finD.est);
    }
  }
  if (dirApl != 0) {
    float paso = pwmVel * dt / T_RAMPA_S;
    duty = min(duty + paso, (float)pwmVel);
    salidaMotores(dirApl, (int)duty);
    pos += dirApl * (100.0 / tPasada) * dt * (duty / pwmVel);
    if (pos < 0) pos = 0;
    if (pos > 100) pos = 100;
  }
}

// Calibra el tiempo de recorrido al llegar a un extremo
void llegadaExtremo(int lado) {      // lado: 1 derecha, -1 izquierda
  if (desdeExtremo && dirApl != 0) {
    float medido = (millis() - tMovIni) / 1000.0;
    if (medido > 5 && medido < 600) tPasada = 0.6 * tPasada + 0.4 * medido;
  }
  pos = (lado > 0) ? 100 : 0;
}

// ===================== CICLO AUTOMÁTICO =====================
float tEnFase() { return (millis() - tFase) / 1000.0; }
void irAFase(Fase f) { fase = f; tFase = millis(); }

void pausaYPasar(int dir, Fase f) {
  dirObj = 0; proxDir = dir; proxFase = f;
  irAFase(F_PAUSA);
  evento("Pausa de 0,4 s antes de invertir");
}

void iniciarCiclo(int n) {
  laps = 0; tot = n; err = ""; manual = false;
  setK2(true);
  irAFase(F_FUENTE);
  evento("Ciclo iniciado: K2 enciende la fuente ATX");
}

void cicloLoop() {
  switch (fase) {
    case F_FUENTE:
      if (tEnFase() >= T_FUENTE_S) { setK1(true); irAFase(F_PRELAVADO); evento("K1 enciende la bomba (prelavado 5 s)"); }
      break;
    case F_PRELAVADO:
      if (tEnFase() >= T_PRELAVADO_S) {
        dirObj = 1; irAFase(F_DER); evento("Motores hacia la derecha");
        if (!BOMBA_EN_RECORRIDO) { setK1(false); evento("K1 apaga la bomba (solo prelavado)"); }
      }
      break;
    case F_DER:
      if (finD.est) { llegadaExtremo(1); evento("Final derecho presionado (GPIO6)"); pausaYPasar(-1, F_IZQ); }
      break;
    case F_IZQ:
      if (finI.est) {
        llegadaExtremo(-1);
        laps++;
        char b[60]; snprintf(b, sizeof(b), "Vuelta %d de %d (GPIO7 presionado)", laps, tot); evento(b);
        if (laps >= tot) { dirObj = 0; irAFase(F_FIN); }
        else pausaYPasar(1, F_DER);
      }
      break;
    case F_PAUSA:
      if (tEnFase() >= T_PAUSA_S) { dirObj = proxDir; irAFase(proxFase); }
      break;
    case F_FIN:
      if (tEnFase() >= T_FIN_S) { apagarTodo(); evento("Ciclo terminado: bomba y fuente apagadas (STANDBY)"); }
      break;
    default: break;
  }
}

// ===================== HONGO Y PULSADOR DE LA CAJA =====================
void emergenciaLoop() {
  bool emerg = USAR_HONGO && !hongo.est;       // abierto o cable cortado = emergencia
  if (emerg) {
    salidasOff();                              // todo apagado, siempre
    if (fase != F_EMERG) {
      fase = F_EMERG; err = "PARO DE EMERGENCIA";
      evento("PARO DE EMERGENCIA: todo apagado");
    }
  } else if (fase == F_EMERG) {
    fase = F_REPOSO; err = "";                 // no arranca solo: queda en STANDBY
    evento("Hongo liberado: sistema en STANDBY");
  }
}

void pulsadorInicioLoop() {
  if (btnIni.est && !prevInicio) {             // flanco: recién presionado
    if (fase == F_REPOSO) { iniciarCiclo(tot); evento("Pulsador de la caja: ciclo iniciado"); }
    else if (fase == F_EMERG) evento("Pulsador ignorado: paro de emergencia activo");
    else evento("Pulsador ignorado: ciclo en marcha o con error");
  }
  prevInicio = btnIni.est;
}

void protecciones() {
  if (escPend && millis() >= tEsc) { escPend = false; dirObj = dirEsc; }
  if (finD.est && finI.est) { if (fase != F_ERROR) fallo("Ambos finales de carrera activos"); return; }
  if (dirApl != 0 && (millis() - tMovIni) > (uint32_t)(T_MAX_MOV_S * 1000)) { fallo("Tiempo máximo de recorrido excedido"); return; }
  if (k1 && (millis() - tK1On) > (uint32_t)(T_MAX_BOMBA_S * 1000)) { setK1(false); evento("Bomba apagada por tiempo máximo"); }
  if (manual && dirObj != 0 && (millis() - lastSeen) > T_ENLACE_MS) {
    dirObj = 0; manual = false; evento("Enlace perdido: motores detenidos");
  }
}

// ===================== SERVIDOR WEB =====================
const char* nombreFase() {
  switch (fase) {
    case F_REPOSO: return "reposo";   case F_FUENTE: return "fuente";
    case F_PRELAVADO: return "prelavado"; case F_DER: return "der";
    case F_PAUSA: return "pausa";     case F_IZQ: return "izq";
    case F_FIN: return "fin";         case F_EMERG: return "emergencia";
    default: return "error";
  }
}

void cors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
}

void hEstado() {
  lastSeen = millis();
  bool pedir = server.hasArg("ev");
  uint32_t desde = pedir ? (uint32_t)server.arg("ev").toInt() : evN;
  String s; s.reserve(600);
  s += "{\"pos\":";  s += String(pos, 1);
  s += ",\"dir\":";  s += dirApl;
  s += ",\"k1\":";   s += (k1 ? 1 : 0);
  s += ",\"k2\":";   s += (k2 ? 1 : 0);
  s += ",\"run\":";  s += ((fase != F_REPOSO && fase != F_ERROR && fase != F_EMERG) ? 1 : 0);
  s += ",\"es\":";   s += (fase == F_EMERG ? 1 : 0);
  s += ",\"ph\":\""; s += nombreFase(); s += "\"";
  s += ",\"laps\":"; s += laps;
  s += ",\"tot\":";  s += tot;
  s += ",\"lR\":";   s += (finD.est ? 1 : 0);
  s += ",\"lL\":";   s += (finI.est ? 1 : 0);
  s += ",\"tp\":";   s += String(tPasada, 1);
  s += ",\"vel\":";  s += pwmVel;
  s += ",\"err\":\""; s += err; s += "\"";
  s += ",\"up\":";   s += (millis() / 1000);
  s += ",\"evn\":";  s += evN;
  s += ",\"ev\":[";
  bool primero = true;
  uint32_t ini = (evN > 8) ? evN - 7 : 1;
  if (desde + 1 > ini) ini = desde + 1;
  for (uint32_t i = ini; i <= evN; i++) {
    Ev &e = evs[i % 8];
    if (e.id != i) continue;
    if (!primero) s += ",";
    primero = false;
    s += "{\"i\":"; s += i; s += ",\"m\":\""; s += e.m; s += "\"}";
  }
  s += "]}";
  cors();
  server.send(200, "application/json; charset=utf-8", s);
}

void hParams() {
  lastSeen = millis();
  String s; s.reserve(300);
  s += "{\"tFuente\":";    s += String(T_FUENTE_S, 2);
  s += ",\"tPre\":";       s += String(T_PRELAVADO_S, 2);
  s += ",\"tPausa\":";     s += String(T_PAUSA_S, 2);
  s += ",\"tFin\":";       s += String(T_FIN_S, 2);
  s += ",\"tRampa\":";     s += String(T_RAMPA_S, 2);
  s += ",\"tMaxMov\":";    s += String(T_MAX_MOV_S, 1);
  s += ",\"tMaxBomba\":";  s += String(T_MAX_BOMBA_S, 1);
  s += ",\"pwm\":";        s += pwmVel;
  s += ",\"bombaRec\":";   s += (BOMBA_EN_RECORRIDO ? 1 : 0);
  s += ",\"laps\":";       s += VUELTAS_POR_DEFECTO;
  s += "}";
  cors();
  server.send(200, "application/json; charset=utf-8", s);
}

void responder(bool ok, const char* m) {
  cors();
  String s = String("{\"ok\":") + (ok ? 1 : 0);
  if (m) { s += ",\"m\":\""; s += m; s += "\""; }
  s += "}";
  server.send(200, "application/json; charset=utf-8", s);
}

void cancelarCiclo() { if (fase != F_REPOSO && fase != F_ERROR) { fase = F_REPOSO; evento("Modo manual: ciclo automático cancelado"); } }

void hCmd() {
  lastSeen = millis();
  if (fase == F_EMERG) { responder(false, "Paro de emergencia activo: libera el hongo"); return; }
  String c = server.arg("c");
  int v = server.arg("v").toInt();
  if (c == "start") {
    if (fase != F_REPOSO) { responder(false, fase == F_ERROR ? "Hay un error: usa Detener todo" : "El ciclo ya está en marcha"); return; }
    int n = server.arg("n").toInt();
    if (n < 1) n = 3;
    if (n > 10) n = 10;
    iniciarCiclo(n);
  } else if (c == "stop") {
    apagarTodo(); err = ""; evento("Todo detenido");
  } else if (c == "izq" || c == "der" || c == "parar") {
    if (fase == F_ERROR) { responder(false, "Hay un error: usa Detener todo"); return; }
    cancelarCiclo();
    escPend = false;
    manual = true;
    dirObj = (c == "der") ? 1 : (c == "izq") ? -1 : 0;
    evento(c == "der" ? "Manual: motores a la derecha" : c == "izq" ? "Manual: motores a la izquierda" : "Motores parados");
  } else if (c == "k1") {
    if (fase == F_ERROR) { responder(false, "Hay un error: usa Detener todo"); return; }
    cancelarCiclo(); setK1(v != 0); evento(v ? "K1 bomba ON" : "K1 bomba OFF");
  } else if (c == "k2") {
    if (fase == F_ERROR) { responder(false, "Hay un error: usa Detener todo"); return; }
    cancelarCiclo(); setK2(v != 0); evento(v ? "K2 fuente ON" : "K2 fuente OFF");
  } else if (c == "escobilla") {
    if (fase == F_ERROR) { responder(false, "Hay un error: usa Detener todo"); return; }
    cancelarCiclo();
    if (v) {
      // Enciende la fuente y, tras 1 s, mueve la escobilla hacia el extremo más lejano
      setK2(true); manual = true;
      dirEsc = (pos < 50) ? 1 : -1; tEsc = millis() + (uint32_t)(T_FUENTE_S * 1000); escPend = true;
      evento("Escobilla encendida");
    } else {
      escPend = false; dirObj = 0; manual = false; setK2(false);
      evento("Escobilla apagada");
    }
  } else if (c == "param") {
    if (fase != F_REPOSO) { responder(false, "Solo se cambian con el sistema en espera"); return; }
    String k = server.arg("k");
    float x = server.arg("v").toFloat();
    bool ok = true;
    if      (k == "tFuente"   && x >= 0    && x <= 10)   T_FUENTE_S = x;
    else if (k == "tPre"      && x >= 0    && x <= 120)  T_PRELAVADO_S = x;
    else if (k == "tPausa"    && x >= 0.2  && x <= 5)    T_PAUSA_S = x;
    else if (k == "tFin"      && x >= 0    && x <= 60)   T_FIN_S = x;
    else if (k == "tRampa"    && x >= 0.05 && x <= 5)    T_RAMPA_S = x;
    else if (k == "tMaxMov"   && x >= 10   && x <= 600)  T_MAX_MOV_S = x;
    else if (k == "tMaxBomba" && x >= 10   && x <= 3600) T_MAX_BOMBA_S = x;
    else if (k == "pwm"       && x >= 80   && x <= 255)  { tPasada = tPasada * pwmVel / (int)x; pwmVel = (int)x; }
    else if (k == "bombaRec"  && (x == 0 || x == 1))     BOMBA_EN_RECORRIDO = (x == 1);
    else if (k == "laps"      && x >= 1    && x <= 10)   { VUELTAS_POR_DEFECTO = (int)x; tot = (int)x; }
    else ok = false;
    if (!ok) { responder(false, "Valor fuera de rango o parámetro desconocido"); return; }
  } else if (c == "vel") {
    if (v < 80) v = 80;
    if (v > 255) v = 255;
    tPasada = tPasada * pwmVel / v;     // mantiene la estimación de posición
    pwmVel = v;
  } else {
    responder(false, "Comando desconocido"); return;
  }
  responder(true, nullptr);
}

void hRaiz() {
  cors();
  server.send(200, "text/plain; charset=utf-8",
    "SolarCleanerPro v2 - ESP32-S3\nUsa la app y conectate a esta red.\nEstado en /estado");
}

// ===================== SETUP / LOOP =====================
void setup() {
  // Salidas seguras al arrancar: todo apagado
  pinMode(PIN_K1_BOMBA, OUTPUT);  rele(PIN_K1_BOMBA, false);
  pinMode(PIN_K2_FUENTE, OUTPUT); rele(PIN_K2_FUENTE, false);
  pwmInit();
  pinMode(PIN_FIN_DER, INPUT_PULLUP);
  pinMode(PIN_FIN_IZQ, INPUT_PULLUP);
  pinMode(PIN_INICIO, INPUT_PULLUP);
  pinMode(PIN_PARO, INPUT_PULLUP);
  delay(50);
  finD.crudo = finD.est = (digitalRead(PIN_FIN_DER) == LOW);
  finI.crudo = finI.est = (digitalRead(PIN_FIN_IZQ) == LOW);
  btnIni.crudo = btnIni.est = (digitalRead(PIN_INICIO) == LOW);
  hongo.crudo = hongo.est = (digitalRead(PIN_PARO) == LOW);
  prevInicio = btnIni.est;                       // no arranca si el botón estaba presionado

  Serial.begin(115200);
  delay(200);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS, AP_CANAL, 0, AP_MAX_CLIENTES);

  server.on("/", hRaiz);
  server.on("/estado", hEstado);
  server.on("/cmd", hCmd);
  server.on("/params", hParams);
  server.begin();

  lastSeen = millis();
  Serial.print("Red WiFi: "); Serial.println(AP_SSID);
  Serial.print("IP: "); Serial.println(WiFi.softAPIP());
  evento("Sistema listo (STANDBY)");
}

void loop() {
  static uint32_t anterior = micros();
  uint32_t ahora = micros();
  float dt = (ahora - anterior) / 1000000.0;
  anterior = ahora;
  if (dt > 0.1) dt = 0.1;

  server.handleClient();
  leerFin(finD);
  leerFin(finI);
  leerFin(btnIni);
  leerFin(hongo);
  emergenciaLoop();
  pulsadorInicioLoop();
  cicloLoop();
  motorLoop(dt);
  protecciones();
  delay(1);
}
