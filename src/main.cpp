#include <Arduino.h>
#include <BluetoothSerial.h>

// ---------------------------------------------------------------------------
// Robot diferencial 4x4: 2 RoboClaw 2x15A en modo Packet Serial (sin librería)
//
//   RoboClaw DERECHO   (0x80): M1 = derecha adelante, M2 = derecha atrás
//   RoboClaw IZQUIERDO (0x81): M1 = izquierda adelante, M2 = izquierda atrás
//
// Cableado (bus compartido):
//   ESP32 GPIO17 (TX2) ---> S1 de ambos RoboClaw
//   ESP32 GND ------------> GND (señal) de ambos RoboClaw
//   (RX2/GPIO16 no se usa: solo enviamos comandos)
// ---------------------------------------------------------------------------

constexpr int PIN_RX = 16;
constexpr int PIN_TX = 17;
constexpr uint32_t BAUD = 9600;  // debe coincidir con el configurado en Motion Studio

constexpr uint8_t ADDR_RIGHT = 0x80;
constexpr uint8_t ADDR_LEFT  = 0x81;

// Ajusta a true si un lado gira al revés (los motores izquierdos van espejados)
constexpr bool INVERT_RIGHT = false;
constexpr bool INVERT_LEFT  = false;

// Comando 34 de RoboClaw: duty M1 y M2 con signo (-32767..32767), CRC16.
// Trama: [addr][34][M1 hi][M1 lo][M2 hi][M2 lo][CRC hi][CRC lo]
constexpr uint8_t CMD_DUTY_M1M2 = 34;

uint16_t crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
  }
  return crc;
}

void sendDuty(uint8_t addr, int16_t m1, int16_t m2) {
  uint8_t pkt[8] = {addr, CMD_DUTY_M1M2,
                    (uint8_t)(m1 >> 8), (uint8_t)m1,
                    (uint8_t)(m2 >> 8), (uint8_t)m2, 0, 0};
  uint16_t crc = crc16(pkt, 6);
  pkt[6] = crc >> 8;
  pkt[7] = crc & 0xFF;
  Serial2.write(pkt, sizeof(pkt));
}

// speed: -127..127 (negativo = reversa). Mueve ambos canales del RoboClaw igual.
void driveSide(uint8_t addr, int speed, bool invert) {
  speed = constrain(speed, -127, 127);
  if (invert) speed = -speed;
  int16_t duty = (int32_t)speed * 32767 / 127;
  sendDuty(addr, duty, duty);
}

// left/right: -127..127
void drive(int left, int right) {
  driveSide(ADDR_LEFT, left, INVERT_LEFT);
  driveSide(ADDR_RIGHT, right, INVERT_RIGHT);
}

void stopAll() { drive(0, 0); }

// ---------------------------------------------------------------------------
// Control por Bluetooth Classic (app "BT Car Controller", protocolo estándar)
//   F/B = adelante/atrás   L/R = izquierda/derecha   S = parar
//   G/I/H/J = diagonales (adelante-izq, adelante-der, atrás-izq, atrás-der)
//   0-9 = velocidad (0 = 0%, 9 = 90%)   q = velocidad 100%   D = parar todo
// El estado se guarda y se reenvía a los RoboClaw cada 50 ms, así no salta
// el timeout de seguridad serie y el robot sigue la última orden recibida.
// ---------------------------------------------------------------------------
BluetoothSerial SerialBT;

constexpr const char *BT_NAME = "ROBOT_SP32";
constexpr int MAX_SPEED = 127;  // tope absoluto (0..127)

int throttle = 0;  // -1 atrás, 0 parado, +1 adelante
int steer = 0;     // -1 izquierda, 0 recto, +1 derecha
int speedPct = 20; // 0..100, velocidad inicial hasta que la app mande 0-9 o q

void applyCommand(char c) {
  switch (c) {
    case 'F': throttle = 1;  steer = 0;  break;
    case 'B': throttle = -1; steer = 0;  break;
    case 'L': throttle = 0;  steer = -1; break;
    case 'R': throttle = 0;  steer = 1;  break;
    case 'G': throttle = 1;  steer = -1; break;
    case 'I': throttle = 1;  steer = 1;  break;
    case 'H': throttle = -1; steer = -1; break;
    case 'J': throttle = -1; steer = 1;  break;
    case 'S': case 'D': throttle = 0; steer = 0; break;
    case 'q': speedPct = 100; break;
    default:
      if (c >= '0' && c <= '9') speedPct = (c - '0') * 10;
      break;
  }
}

// Las 4 ruedas fijas patinan de lado al girar sobre el eje y necesitan mucho
// más par que para avanzar: en giro puro se usa al menos este % de potencia.
constexpr int TURN_MIN_PCT = 60;

// Rampa: cuánto puede cambiar la salida de cada lado por ciclo de 50 ms
// (escala 0..127). Suaviza el pico de corriente al arrancar, que puede hundir
// la alimentación del ESP32 y cortar el Bluetooth.
constexpr int RAMP_STEP = 10;

void updateDrive() {
  bool pureTurn = (throttle == 0 && steer != 0);
  int pct = pureTurn ? max(speedPct, TURN_MIN_PCT) : speedPct;
  int s = MAX_SPEED * pct / 100;
  // Mezcla diferencial: giro positivo = derecha (rueda izq. más, der. menos)
  int targetL = constrain(throttle + steer, -1, 1) * s;
  int targetR = constrain(throttle - steer, -1, 1) * s;

  static int curL = 0, curR = 0;
  curL += constrain(targetL - curL, -RAMP_STEP, RAMP_STEP);
  curR += constrain(targetR - curR, -RAMP_STEP, RAMP_STEP);
  drive(curL, curR);
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(BAUD, SERIAL_8N1, PIN_RX, PIN_TX);
  stopAll();
  SerialBT.begin(BT_NAME);
  Serial.printf("Bluetooth listo. Empareja el dispositivo \"%s\" y conecta desde la app.\n", BT_NAME);
}

void loop() {
  while (SerialBT.available()) {
    char c = SerialBT.read();
    if (c == '\r' || c == '\n') continue;
    Serial.printf("BT: '%c'\n", c);  // útil para ver qué manda tu app
    applyCommand(c);
  }

  // Seguridad: sin conexión Bluetooth, el robot se detiene
  if (!SerialBT.hasClient()) {
    throttle = 0;
    steer = 0;
  }

  static uint32_t last = 0;
  if (millis() - last >= 50) {
    last = millis();
    updateDrive();
  }
}
