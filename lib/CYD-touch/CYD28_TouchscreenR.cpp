/* CYD28 resistive touch version from Bruce. */

#include "CYD28_TouchscreenR.h"

#define ISR_PREFIX IRAM_ATTR
#define MSEC_THRESHOLD 3
#define SPI_SETTING SPISettings(2000000, MSBFIRST, SPI_MODE0)

static CYD28_TouchR *isrPinptr;
void isrPin(void);

bool CYD28_TouchR::begin() {
  pinMode(CYD28_TouchR_MOSI, OUTPUT);
  pinMode(CYD28_TouchR_MISO, INPUT);
  pinMode(CYD28_TouchR_CLK, OUTPUT);
  pinMode(CYD28_TouchR_CS, OUTPUT);
  digitalWrite(CYD28_TouchR_CLK, LOW);
  digitalWrite(CYD28_TouchR_CS, HIGH);
  if (CYD28_TouchR_IRQ != -1) {
    pinMode(CYD28_TouchR_IRQ, INPUT);
    attachInterrupt(digitalPinToInterrupt(CYD28_TouchR_IRQ), isrPin, FALLING);
    isrPinptr = this;
  } else {
    isrWake = true;
  }
  return true;
}

bool CYD28_TouchR::begin(SPIClass *wspi) {
  _pspi = wspi;
  pinMode(CYD28_TouchR_CS, OUTPUT);
  digitalWrite(CYD28_TouchR_CS, HIGH);
  if (CYD28_TouchR_IRQ != -1) {
    pinMode(CYD28_TouchR_IRQ, INPUT);
    attachInterrupt(digitalPinToInterrupt(CYD28_TouchR_IRQ), isrPin, FALLING);
    isrPinptr = this;
  } else {
    isrWake = true;
  }
  return true;
}

ISR_PREFIX
void isrPin(void) {
  CYD28_TouchR *o = isrPinptr;
  o->isrWake = true;
}

uint8_t CYD28_TouchR::transfer(uint8_t val) {
  if (_pspi == nullptr) {
    uint8_t out = 0;
    uint8_t del = _delay >> 1;
    uint8_t bval = 0;
    int sck = LOW;
    int8_t bit = 8;
    while (bit) {
      bit--;
      digitalWrite(CYD28_TouchR_MOSI, ((val & (1 << bit)) ? HIGH : LOW));
      wait(del);
      sck ^= 1u;
      digitalWrite(CYD28_TouchR_CLK, sck);
      bval = digitalRead(CYD28_TouchR_MISO);
      out <<= 1;
      out |= bval;
      wait(del);
      sck ^= 1u;
      digitalWrite(CYD28_TouchR_CLK, sck);
    }
    return out;
  }
  return _pspi->transfer(val);
}

uint16_t CYD28_TouchR::transfer16(uint16_t data) {
  union {
    uint16_t val;
    struct {
      uint8_t lsb;
      uint8_t msb;
    };
  } in, out;
  in.val = data;

  if (_pspi == nullptr) {
    out.msb = transfer(in.msb);
    out.lsb = transfer(in.lsb);
    return out.val;
  }
  out.msb = _pspi->transfer(in.msb);
  out.lsb = _pspi->transfer(in.lsb);
  return out.val;
}

void CYD28_TouchR::wait(uint_fast8_t del) {
  for (uint_fast8_t i = 0; i < del; i++) {
    asm volatile("nop");
  }
}

CYD28_TS_Point CYD28_TouchR::getPointScaled() {
  update();
  int16_t x = xraw;
  int16_t y = yraw;
  convertRawXY(&x, &y);
  return CYD28_TS_Point(x, y, zraw);
}

CYD28_TS_Point CYD28_TouchR::getPointRaw() {
  update();
  return CYD28_TS_Point(xraw, yraw, zraw);
}

bool CYD28_TouchR::touched() {
  update();
  return ((zraw >= threshold) && isrWake);
}

void CYD28_TouchR::readData(uint16_t *x, uint16_t *y, uint8_t *z) {
  update();
  *x = xraw;
  *y = yraw;
  *z = zraw;
}

static int16_t besttwoavg(int16_t x, int16_t y, int16_t z) {
  int16_t da, db, dc;
  int16_t reta = 0;
  if (x > y) da = x - y; else da = y - x;
  if (x > z) db = x - z; else db = z - x;
  if (z > y) dc = z - y; else dc = y - z;

  if (da <= db && da <= dc) reta = (x + y) >> 1;
  else if (db <= da && db <= dc) reta = (x + z) >> 1;
  else reta = (y + z) >> 1;

  return reta;
}

void CYD28_TouchR::update() {
  int16_t data[6];
  int z;
  if (!isrWake) return;
  uint32_t now = millis();
  if (now - msraw < MSEC_THRESHOLD) return;

  digitalWrite(CYD28_TouchR_CS, LOW);
  if (_pspi != nullptr) _pspi->beginTransaction(SPI_SETTING);

  transfer(0xB1);
  int16_t z1 = transfer16(0xC1) >> 3;
  z = z1 + 4095;
  int16_t z2 = transfer16(0x91) >> 3;
  z -= z2;
  if (z >= threshold) {
    transfer16(0x91);
    data[0] = transfer16(0xD1) >> 3;
    data[1] = transfer16(0x91) >> 3;
    data[2] = transfer16(0xD1) >> 3;
    data[3] = transfer16(0x91) >> 3;
  } else {
    data[0] = data[1] = data[2] = data[3] = 0;
  }
  data[4] = transfer16(0xD0) >> 3;
  data[5] = transfer16(0) >> 3;

  if (_pspi != nullptr) _pspi->endTransaction();
  digitalWrite(CYD28_TouchR_CS, HIGH);

  if (z < 0) z = 0;
  if (z < threshold) {
    zraw = 0;
    if (z < CYD28_TouchR_Z_THRES_INT && CYD28_TouchR_IRQ != -1) {
      isrWake = false;
    }
    return;
  }
  zraw = z;

  const int16_t x = besttwoavg(data[0], data[2], data[4]);
  const int16_t y = besttwoavg(data[1], data[3], data[5]);
  msraw = now;
  xraw = x;
  yraw = y;
}

void CYD28_TouchR::convertRawXY(int16_t *x, int16_t *y) {
  const int16_t x_tmp = *x;
  const int16_t y_tmp = *y;
  int16_t xx = 0;
  int16_t yy = 0;
  switch (rotation) {
    case 0:
      xx = ((y_tmp - CYD28_TouchR_CAL_YMIN) * sizeY_px) /
           (CYD28_TouchR_CAL_YMAX - CYD28_TouchR_CAL_YMIN);
      yy = ((x_tmp - CYD28_TouchR_CAL_XMIN) * sizeX_px) /
           (CYD28_TouchR_CAL_XMAX - CYD28_TouchR_CAL_XMIN);
      xx = sizeY_px - xx;
      break;
    case 1:
      xx = ((x_tmp - CYD28_TouchR_CAL_XMIN) * sizeX_px) /
           (CYD28_TouchR_CAL_XMAX - CYD28_TouchR_CAL_XMIN);
      yy = ((y_tmp - CYD28_TouchR_CAL_YMIN) * sizeY_px) /
           (CYD28_TouchR_CAL_YMAX - CYD28_TouchR_CAL_YMIN);
      break;
    case 2:
      xx = ((y_tmp - CYD28_TouchR_CAL_YMIN) * sizeY_px) /
           (CYD28_TouchR_CAL_YMAX - CYD28_TouchR_CAL_YMIN);
      yy = ((x_tmp - CYD28_TouchR_CAL_XMIN) * sizeX_px) /
           (CYD28_TouchR_CAL_XMAX - CYD28_TouchR_CAL_XMIN);
      yy = sizeX_px - yy;
      break;
    default:
      xx = ((x_tmp - CYD28_TouchR_CAL_XMIN) * sizeX_px) /
           (CYD28_TouchR_CAL_XMAX - CYD28_TouchR_CAL_XMIN);
      yy = ((y_tmp - CYD28_TouchR_CAL_YMIN) * sizeY_px) /
           (CYD28_TouchR_CAL_YMAX - CYD28_TouchR_CAL_YMIN);
      xx = sizeX_px - xx;
      yy = sizeY_px - yy;
      break;
  }
  *x = xx;
  *y = yy;
}
