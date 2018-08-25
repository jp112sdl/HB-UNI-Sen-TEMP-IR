//- -----------------------------------------------------------------------------------------------------------------------
// AskSin++
// 2016-10-31 papa Creative Commons - http://creativecommons.org/licenses/by-nc-sa/3.0/de/
//- -----------------------------------------------------------------------------------------------------------------------

// define this to read the device id, serial and device type from bootloader section
// #define USE_OTA_BOOTLOADER

#define EI_NOTEXTERNAL
#include <EnableInterrupt.h>
#include <AskSinPP.h>
#include <LowPower.h>

#include <Register.h>
#include <MultiChannelDevice.h>
#include <sensors/MLX90614.h>

// Arduino Pro mini 8 Mhz
// Arduino pin for the config button
#define CONFIG_BUTTON_PIN 8

// number of available peers per channel
#define PEERS_PER_CHANNEL 6

#define SENSOR_EN_PIN     6

// all library classes are placed in the namespace 'as'
using namespace as;

enum tParams {INACTIVE, T1, T2, T1sT2, T2sT1};

// define all device properties
const struct DeviceInfo PROGMEM devinfo = {
  {0xf3, 0x08, 0x01},     // Device ID
  "JPTHIR0001",           // Device Serial
  {0xf3, 0x08},           // Device Model Indoor
  0x10,                   // Firmware Version
  as::DeviceType::THSensor, // Device Type
  {0x01, 0x00}            // Info Bytes
};

/**
   Configure the used hardware
*/
typedef AvrSPI<10, 11, 12, 13> SPIType;
typedef Radio<SPIType, 2> RadioType;
typedef StatusLed<4> LedType;
typedef AskSin<LedType, BatterySensor, RadioType> BaseHal;
class Hal : public BaseHal {
  public:
    void init (const HMID& id) {
      BaseHal::init(id);

      battery.init(seconds2ticks(60UL * 60), sysclock); //battery measure once an hour
      battery.low(22);
      battery.critical(18);
    }

    bool runready () {
      return sysclock.runready() || BaseHal::runready();
    }
} hal;


DEFREGISTER(Reg0, MASTERID_REGS, DREG_TPARAMS, DREG_LOCALRESETDISABLE, 0x20, 0x21)
class UList0 : public RegList0<Reg0> {
  public:
    UList0(uint16_t addr) : RegList0<Reg0>(addr) {}
    bool updIntervall (uint16_t value) const {
      return this->writeRegister(0x20, (value >> 8) & 0xff) && this->writeRegister(0x21, value & 0xff);
    }
    uint16_t updIntervall () const {
      return (this->readRegister(0x20, 0) << 8) + this->readRegister(0x21, 0);
    }

    void defaults () {
      clear();
      localResetDisable(false);
      tParamSelect(3);
      updIntervall(180);
    }
};

class MeasureEventMsg : public Message {
  public:
    void init(uint8_t msgcnt, int tempValues[4], bool batlow) {
      Message::init(0x1a, msgcnt, 0x53, (msgcnt % 20 == 1) ? BIDI : BCAST, batlow ? 0x80 : 0x00, 0x41);
      for (int i = 0; i < 4; i++) {
        pload[i * 3] = (tempValues[i] >> 8) & 0xff;
        pload[(i * 3) + 1] = (tempValues[i]) & 0xff;
        pload[(i * 3) + 2] = 0x42 + i;
      }
    }
};

class WeatherChannel : public Channel<Hal, List1, EmptyList, List4, PEERS_PER_CHANNEL, UList0> {
  public:
    WeatherChannel () : Channel() {}
    virtual ~WeatherChannel () {}

    void configChanged() {
      //DPRINTLN("Config changed List1");
    }

    uint8_t status () const {
      return 0;
    }

    uint8_t flags () const {
      return 0;
    }
};

class UType : public MultiChannelDevice<Hal, WeatherChannel, 5, UList0> {

    class SensorArray : public Alarm {
        UType& dev;
        MLX90614<>      mlx90614;
      public:
        int16_t tempValues[4] = {0, 0, 0, 0};
        SensorArray (UType& d) : Alarm(0), dev(d) { }

        virtual void trigger (__attribute__ ((unused)) AlarmClock& clock) {
          uint16_t txCycle = dev.getList0().updIntervall();
          tick = seconds2ticks(txCycle);
          sysclock.add(*this);

          digitalWrite(SENSOR_EN_PIN, HIGH);
          
          delay(250);
          
          mlx90614.measure();

          tempValues[0] = mlx90614.temperatureAmb();
          tempValues[1] = mlx90614.temperatureObj();

          digitalWrite(SENSOR_EN_PIN, LOW);


          tempValues[2] = tempValues[0] - tempValues[1];
          tempValues[3] = tempValues[1] - tempValues[0];

          DPRINT("T:"); DDEC(tempValues[0]); DPRINT(";"); DDEC(tempValues[1]); DPRINT(";"); DDEC(tempValues[2]); DPRINT(";"); DDECLN(tempValues[3]);

          MeasureEventMsg& msg = (MeasureEventMsg&)dev.message();

          msg.init(dev.nextcount(), tempValues, dev.battery().low());
          dev.send(msg, dev.getMasterID());
        }

        void initSensor() {
          digitalWrite(SENSOR_EN_PIN, HIGH);
          delay(500);
          mlx90614.init();
          digitalWrite(SENSOR_EN_PIN, LOW);
        }

    } sensarray;

  public:
    typedef MultiChannelDevice<Hal, WeatherChannel, 5, UList0> TSDevice;
    UType(const DeviceInfo& info, uint16_t addr) : TSDevice(info, addr), sensarray(*this) {}
    virtual ~UType () {}

    virtual void configChanged () {
      TSDevice::configChanged();
      DPRINTLN("Config Changed List0");
      DPRINT("tParamSelect: "); DDECLN(this->getList0().tParamSelect());
      DPRINT("localResetDisable: "); DDECLN(this->getList0().localResetDisable());
      DPRINT("updIntervall: "); DDECLN(this->getList0().updIntervall());
    }

    bool init (Hal& hal) {
      TSDevice::init(hal);
      pinMode(SENSOR_EN_PIN, OUTPUT);
      sensarray.set(seconds2ticks(2));
      sensarray.initSensor();
      sysclock.add(sensarray);
    }
};

UType sdev(devinfo, 0x20);
ConfigButton<UType> cfgBtn(sdev);

void setup () {
  DINIT(57600, ASKSIN_PLUS_PLUS_IDENTIFIER);
  sdev.init(hal);
  buttonISR(cfgBtn, CONFIG_BUTTON_PIN);
  sdev.initDone();
}

void loop() {
  bool worked = hal.runready();
  bool poll = sdev.pollRadio();
  if ( worked == false && poll == false ) {
    if ( hal.battery.critical() ) {
      hal.activity.sleepForever(hal);
    }
    hal.activity.savePower<Sleep<>>(hal);
  }
}

