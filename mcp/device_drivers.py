"""Concrete, inspectable Lua device drivers; no firmware changes per installation.

Implemented protocol/decoding is distinct from physical compatibility testing.
The catalog never claims an unconnected device was detected or hardware-tested.
"""

from dataclasses import dataclass
import json
import math
from pathlib import Path
import re

from native_scripts import save_script, validate_source, valid_name


@dataclass(frozen=True)
class Driver:
    name: str
    bus: str
    address: int | None
    addresses: tuple[int, ...]
    source: str
    body: str
    outputs: str


COMMON = r"""
local function u16(s,i) return s:byte(i)*256+s:byte(i+1) end
local function le16(s,i) return s:byte(i)+s:byte(i+1)*256 end
local function signed(v) if v>=32768 then return v-65536 end return v end
local function crc(s,n,init) local c=init or 255;for i=1,n do c=c~s:byte(i);for j=1,8 do if c&128~=0 then c=((c<<1)~49)&255 else c=(c<<1)&255 end end end;return c end
local initialized=false
""".strip()

I2C = r"""
local function raw(tx,n) local v=device.i2c_transfer(sda,scl,address,tx,n);assert(#v==n,"short I2C read");return v end
local function read(r,n) return raw(string.char(r),n) end
local function write(r,...) raw(string.char(r,...),0) end
""".strip()

DRIVERS: dict[str, Driver] = {}


def add(name, address, body, outputs, source, addresses=None, bus="i2c"):
    DRIVERS[name] = Driver(
        name,
        bus,
        address,
        tuple(addresses or (() if address is None else (address,))),
        source,
        body.strip(),
        outputs,
    )


add(
    "bh1750",
    0x23,
    r"""
raw(string.char(1),0);raw(string.char(0x20),0);device.sleep_ms(180)
return {lux=u16(raw("",2),1)/1.2}
""",
    "illuminance lux",
    "https://www.rohm.com/products/sensors-mems/ambient-light-sensor-ics/bh1750fvi-product",
    (0x23, 0x5C),
)
add(
    "veml7700",
    0x10,
    r"""
write(0,0,0);device.sleep_ms(150);local n=le16(read(4,2),1)
return {als_raw=n,lux=n*0.0576}
""",
    "ALS count, low-light lux (gain1/100ms; no high-light correction)",
    "https://www.vishay.com/docs/84286/veml7700.pdf",
)
add(
    "tsl2561",
    0x39,
    r"""
write(0x80,3);write(0x81,2);device.sleep_ms(410)
return {broadband_raw=le16(read(0xAC,2),1),infrared_raw=le16(read(0xAE,2),1)}
""",
    "raw broadband/IR channels; lux conversion not provided",
    "https://github.com/adafruit/Adafruit_TSL2561",
    (0x29, 0x39, 0x49),
)
add(
    "apds9960",
    0x39,
    r"""
write(0x80,3);write(0x81,219);device.sleep_ms(120);local s=read(0x94,8)
return {clear=le16(s,1),red=le16(s,3),green=le16(s,5),blue=le16(s,7)}
""",
    "raw RGBC channels; gesture processing not provided",
    "https://github.com/adafruit/Adafruit_APDS9960",
)
add(
    "tcs34725",
    0x29,
    r"""
write(0x80,1);device.sleep_ms(3);write(0x81,0xD5);write(0x80,3);device.sleep_ms(110);local s=read(0xB4,8)
return {clear=le16(s,1),red=le16(s,3),green=le16(s,5),blue=le16(s,7)}
""",
    "raw RGBC channels; no color-temperature inference",
    "https://github.com/adafruit/Adafruit_TCS34725",
)
for name, scale, source in (
    ("lm75", 1 / 256, "https://www.ti.com/lit/ds/symlink/lm75.pdf"),
    ("tmp102", 1 / 256, "https://www.ti.com/lit/ds/symlink/tmp102.pdf"),
    ("tmp117", 0.0078125, "https://www.ti.com/lit/ds/symlink/tmp117.pdf"),
):
    add(
        name,
        0x48,
        f"return {{temperature_c=signed(u16(read(0,2),1))*{scale}}}",
        "temperature Celsius (default normal register mode)",
        source,
        tuple(range(0x48, 0x4C)),
    )
add(
    "mcp9808",
    0x18,
    r"""
assert(u16(read(6,2),1)==0x54,"MCP9808 manufacturer mismatch")
local n=u16(read(5,2),1);local t=(n&4095)/16;if n&4096~=0 then t=t-256 end
return {temperature_c=t}
""",
    "temperature Celsius",
    "https://www.microchip.com/en-us/product/MCP9808",
    tuple(range(0x18, 0x20)),
)
add(
    "sht31",
    0x44,
    r"""
raw(string.char(0x24,0),0);device.sleep_ms(20);local s=raw("",6)
assert(crc(s:sub(1,2),2)==s:byte(3) and crc(s:sub(4,5),2)==s:byte(6),"SHT31 CRC mismatch")
return {temperature_c=-45+175*u16(s,1)/65535,humidity_percent=100*u16(s,4)/65535}
""",
    "temperature/humidity with CRC",
    "https://sensirion.com/products/catalog/SHT31-DIS-B",
    (0x44, 0x45),
)
add(
    "sht4x",
    0x44,
    r"""
raw(string.char(0xFD),0);device.sleep_ms(12);local s=raw("",6)
assert(crc(s:sub(1,2),2)==s:byte(3) and crc(s:sub(4,5),2)==s:byte(6),"SHT4x CRC mismatch")
return {temperature_c=-45+175*u16(s,1)/65535,humidity_percent=math.max(0,math.min(100,-6+125*u16(s,4)/65535))}
""",
    "temperature/humidity with CRC",
    "https://sensirion.com/resource/datasheet/sht4x",
    (0x44, 0x45, 0x46),
)
add(
    "aht20",
    0x38,
    r"""
if not initialized then raw(string.char(0xBE,8,0),0);device.sleep_ms(10);initialized=true end
raw(string.char(0xAC,0x33,0),0);device.sleep_ms(85);local s=raw("",7)
assert(s:byte(1)&128==0,"AHT20 busy");assert(crc(s,6)==s:byte(7),"AHT20 CRC mismatch")
local h=s:byte(2)*4096+s:byte(3)*16+(s:byte(4)>>4)
local t=(s:byte(4)&15)*65536+s:byte(5)*256+s:byte(6)
return {temperature_c=t/1048576*200-50,humidity_percent=h/1048576*100}
""",
    "temperature/humidity with busy and CRC checks",
    "https://github.com/adafruit/Adafruit_AHTX0",
)
add(
    "hdc1080",
    0x40,
    r"""
assert(u16(read(0xFE,2),1)==0x5449,"HDC1080 manufacturer mismatch")
write(2,0x10,0);raw(string.char(0),0);device.sleep_ms(20);local s=raw("",4)
return {temperature_c=u16(s,1)/65536*165-40,humidity_percent=u16(s,3)/65536*100}
""",
    "temperature/humidity",
    "https://www.ti.com/lit/ds/symlink/hdc1080.pdf",
)
for name, source in (
    ("htu21d", "https://github.com/adafruit/Adafruit_HTU21DF_Library"),
    ("si7021", "https://www.silabs.com/documents/public/data-sheets/Si7021-A20.pdf"),
):
    add(
        name,
        0x40,
        r"""
raw(string.char(0xF3),0);device.sleep_ms(55);local t=raw("",3)
raw(string.char(0xF5),0);device.sleep_ms(30);local h=raw("",3)
assert(crc(t,2,0)==t:byte(3) and crc(h,2,0)==h:byte(3),"temperature/humidity CRC mismatch")
return {temperature_c=-46.85+175.72*(u16(t,1)&65532)/65536,humidity_percent=-6+125*(u16(h,1)&65532)/65536}
""",
        "temperature/humidity with CRC",
        source,
    )

BOSCH = r"""
assert(read(0xD0,1):byte()==EXPECTED,"Bosch chip ID mismatch")
HUM_START
write(0xF4,0x25);device.sleep_ms(20);local c=read(0x88,24);local s=read(0xF7,8)
local t1=le16(c,1)+0.0;local t2=signed(le16(c,3))+0.0;local t3=signed(le16(c,5))+0.0
local at=s:byte(4)*4096+s:byte(5)*16+(s:byte(6)>>4)
local v1=(at/16384-t1/1024)*t2;local v2=(at/131072-t1/8192)^2*t3;local fine=v1+v2
local p={};for i=1,9 do p[i]=le16(c,7+(i-1)*2);if i>1 then p[i]=signed(p[i]) end;p[i]=p[i]+0.0 end
v1=fine/2-64000;v2=v1*v1*p[6]/32768+v1*p[5]*2;v2=v2/4+p[4]*65536
v1=(p[3]*v1*v1/524288+p[2]*v1)/524288;v1=(1+v1/32768)*p[1];assert(v1~=0,"invalid pressure calibration")
local ap=s:byte(1)*4096+s:byte(2)*16+(s:byte(3)>>4);local pressure=(1048576-ap-v2/4096)*6250/v1
pressure=pressure+(p[9]*pressure*pressure/2147483648+pressure*p[8]/32768+p[7])/16
local out={temperature_c=fine/5120,pressure_pa=pressure}
HUM_RESULT
return out
"""
add(
    "bmp280",
    0x76,
    BOSCH.replace("EXPECTED", "0x58").replace("HUM_START", "").replace("HUM_RESULT", ""),
    "factory-compensated temperature/pressure",
    "https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmp280-ds001.pdf",
    (0x76, 0x77),
)
add(
    "bme280",
    0x76,
    BOSCH.replace("EXPECTED", "0x60")
    .replace("HUM_START", "write(0xF2,1)")
    .replace(
        "HUM_RESULT",
        r"""
local h=read(0xE1,7);local h1=read(0xA1,1):byte();local h2=signed(le16(h,1))+0.0;local h3=h:byte(3)
local h4=h:byte(4)*16+(h:byte(5)&15);if h4>=2048 then h4=h4-4096 end
local h5=h:byte(6)*16+(h:byte(5)>>4);if h5>=2048 then h5=h5-4096 end
local h6=h:byte(7);if h6>=128 then h6=h6-256 end
local rh=fine-76800;rh=(u16(s,7)-(h4*64+h5/16384*rh))*(h2/65536*(1+h6/67108864*rh*(1+h3/67108864*rh)))
out.humidity_percent=math.max(0,math.min(100,rh*(1-h1*rh/524288)))
""",
    ),
    "factory-compensated temperature/pressure/humidity",
    "https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bme280-ds002.pdf",
    (0x76, 0x77),
)
add(
    "bmp180",
    0x77,
    r"""
assert(read(0xD0,1):byte()==0x55,"BMP180 ID mismatch");local c=read(0xAA,22);local a={}
for i=1,11 do local n=u16(c,(i-1)*2+1);if i<4 or i>6 then n=signed(n) end;a[i]=n+0.0 end
write(0xF4,0x2E);device.sleep_ms(6);local ut=u16(read(0xF6,2),1)
local x1=(ut-a[6])*a[5]/32768;assert(x1+a[11]~=0,"invalid calibration");local b5=x1+a[10]*2048/(x1+a[11]);local temperature=(b5+8)/160
write(0xF4,0x34);device.sleep_ms(6);local up=u16(read(0xF6,3),1);local b6=b5-4000
x1=a[8]*(b6*b6/4096)/2048;local x2=a[2]*b6/2048;local b3=math.floor((a[1]*4+x1+x2+2)/4)
x1=a[3]*b6/8192;x2=a[7]*(b6*b6/4096)/65536;local x3=(x1+x2+2)/4;local b4=a[4]*(x3+32768)/32768;assert(b4~=0,"invalid calibration")
local p=(up-b3)*50000*2/b4;x1=(p/256)^2*3038/65536;x2=-7357*p/65536;p=p+(x1+x2+3791)/16
return {temperature_c=temperature,pressure_pa=p}
""",
    "compensated temperature/pressure at OSS0",
    "https://github.com/adafruit/Adafruit-BMP085-Library",
)
for name, delay, source in (
    ("ads1115", 12, "https://www.ti.com/lit/ds/symlink/ads1115.pdf"),
    ("ads1015", 3, "https://www.ti.com/lit/ds/symlink/ads1015.pdf"),
):
    add(
        name,
        0x48,
        f"""
local config=0x8383|((4+channel)<<12);write(1,config>>8,config&255);device.sleep_ms({delay})
local n=signed(u16(read(0,2),1));return {{channel=channel,raw=n,volts=n*0.000125}}
""",
        "single-ended ADC channel 0..3, ±4.096V PGA",
        source,
        tuple(range(0x48, 0x4C)),
    )
add(
    "ina219",
    0x40,
    r"""
write(0,0x39,0x9F);device.sleep_ms(3);local bus=u16(read(2,2),1);assert(bus&1==0,"INA219 overflow")
local volts=(bus>>3)*0.004;local current=signed(u16(read(1,2),1))*0.00001/shunt
return {volts=volts,current_a=current,power_w=volts*current}
""",
    "bus voltage, shunt-based current and power; explicit shunt resistance",
    "https://www.ti.com/lit/ds/symlink/ina219.pdf",
    tuple(range(0x40, 0x50)),
)
add(
    "ina226",
    0x40,
    r"""
assert(u16(read(0xFE,2),1)==0x5449,"INA226 manufacturer mismatch")
local volts=u16(read(2,2),1)*0.00125;local current=signed(u16(read(1,2),1))*0.0000025/shunt
return {volts=volts,current_a=current,power_w=volts*current}
""",
    "bus voltage, shunt-based current and power; explicit shunt resistance",
    "https://www.ti.com/lit/ds/symlink/ina226.pdf",
    tuple(range(0x40, 0x50)),
)
add(
    "mpu6050",
    0x68,
    r"""
assert(read(0x75,1):byte()&126==0x68,"MPU6050 ID mismatch")
write(0x6B,0);write(0x1B,0);write(0x1C,0);device.sleep_ms(20);local s=read(0x3B,14)
return {ax_g=signed(u16(s,1))/16384,ay_g=signed(u16(s,3))/16384,az_g=signed(u16(s,5))/16384,temperature_c=signed(u16(s,7))/340+36.53,gx_dps=signed(u16(s,9))/131,gy_dps=signed(u16(s,11))/131,gz_dps=signed(u16(s,13))/131}
""",
    "acceleration, angular velocity, temperature",
    "https://github.com/adafruit/Adafruit_MPU6050",
    (0x68, 0x69),
)
add(
    "adxl345",
    0x53,
    r"""
assert(read(0,1):byte()==0xE5,"ADXL345 ID mismatch");write(0x2D,8);write(0x31,8);device.sleep_ms(20);local s=read(0x32,6)
return {ax_g=signed(le16(s,1))*0.0039,ay_g=signed(le16(s,3))*0.0039,az_g=signed(le16(s,5))*0.0039}
""",
    "3-axis acceleration at full resolution",
    "https://www.analog.com/media/en/technical-documentation/data-sheets/adxl345.pdf",
    (0x1D, 0x53),
)
add(
    "lis3dh",
    0x18,
    r"""
assert(read(0x0F,1):byte()==0x33,"LIS3DH ID mismatch");write(0x20,0x57);write(0x23,0x88);device.sleep_ms(20);local s=read(0xA8,6)
return {ax_g=signed(le16(s,1))/16000,ay_g=signed(le16(s,3))/16000,az_g=signed(le16(s,5))/16000}
""",
    "3-axis acceleration, high resolution ±2g",
    "https://www.st.com/resource/en/datasheet/lis3dh.pdf",
    (0x18, 0x19),
)
add(
    "lsm6ds3trc",
    0x6A,
    r"""
assert(read(0x0F,1):byte()==0x6A,"LSM6DS3TR-C ID mismatch");write(0x12,0x44);write(0x10,0x40);write(0x11,0x40);device.sleep_ms(20);local s=read(0x20,14)
return {temperature_c=signed(le16(s,1))/256+25,gx_dps=signed(le16(s,3))*0.00875,gy_dps=signed(le16(s,5))*0.00875,gz_dps=signed(le16(s,7))*0.00875,ax_g=signed(le16(s,9))*0.000061,ay_g=signed(le16(s,11))*0.000061,az_g=signed(le16(s,13))*0.000061}
""",
    "acceleration/angular velocity/temperature; TR-C variant only",
    "https://www.st.com/resource/en/datasheet/lsm6ds3tr-c.pdf",
    (0x6A, 0x6B),
)
add(
    "ds3231",
    0x68,
    r"""
local function bcd(n) return (n>>4)*10+(n&15) end
local s=read(0,7);local hour=s:byte(3);if hour&64~=0 then hour=bcd(hour&31)%12+((hour&32~=0) and 12 or 0) else hour=bcd(hour&63) end
local t=read(0x11,2);local upper=t:byte(1);if upper>=128 then upper=upper-256 end
return {year=2000+bcd(s:byte(7))+((s:byte(6)&128~=0) and 100 or 0),month=bcd(s:byte(6)&31),day=bcd(s:byte(5)),hour=hour,minute=bcd(s:byte(2)),second=bcd(s:byte(1)&127),temperature_c=upper+(t:byte(2)>>6)/4,clock_valid=read(0x0F,1):byte()&128==0}
""",
    "RTC date/time/temperature and oscillator validity",
    "https://www.analog.com/media/en/technical-documentation/data-sheets/DS3231.pdf",
)
add(
    "mcp23017",
    0x20,
    r"""
if outputs>=0 then write(0,((~outputs)&255),((~outputs)>>8)&255);write(0x14,value&255,(value>>8)&255) end
return {gpio=le16(read(0x12,2),1)}
""",
    "16-bit GPIO; optional explicit output mask/value, BANK0 required",
    "https://www.microchip.com/en-us/product/MCP23017",
    tuple(range(0x20, 0x28)),
)
add(
    "pca9685",
    0x40,
    r"""
if not initialized then local pre=math.floor(25000000/(4096*frequency)+0.5)-1;write(0,0x10);write(0xFE,pre);write(0,0x20);device.sleep_ms(1);write(0,0xA0);initialized=true end
write(6+4*channel,0,0,value&255,(value==0) and 16 or ((value>>8)&15))
return {channel=channel,duty=value,frequency_hz=frequency}
""",
    "16-channel PWM; explicit output value; outputs persist after script exit",
    "https://www.nxp.com/docs/en/data-sheet/PCA9685.pdf",
    tuple(range(0x40, 0x78)),
)
add(
    "ds18b20",
    None,
    r"""
local function crc8(s,n) local c=0;for i=1,n do local b=s:byte(i);for j=1,8 do local mix=(c~b)&1;c=c>>1;if mix~=0 then c=c~0x8C end;b=b>>1 end end;return c end
assert(device.onewire_reset(pin),"no 1-Wire device");device.onewire_write(pin,string.char(0xCC,0x44));device.sleep_ms(750)
assert(device.onewire_reset(pin),"1-Wire device disappeared");device.onewire_write(pin,string.char(0xCC,0xBE));local s=device.onewire_read(pin,9)
assert(crc8(s,8)==s:byte(9),"DS18B20 CRC mismatch");return {temperature_c=signed(le16(s,1))/16}
""",
    "temperature with CRC; one externally powered device per bus",
    "https://www.analog.com/media/en/technical-documentation/data-sheets/ds18b20.pdf",
    bus="onewire",
)
add(
    "mcp4725",
    0x60,
    r"""
raw(string.char((value>>8)&15,value&255),0);return {dac_code=value}
""",
    "12-bit DAC fast write without EEPROM writes; output persists",
    "https://www.microchip.com/en-us/product/MCP4725",
    tuple(range(0x60, 0x68)),
)


add(
    "nrf24l01",
    None,
    r"""
local function command(c,data) return spi(string.char(c)..(data or "")):sub(2) end
local function reg(r) return command(r&31,string.char(0)):byte() end
local function put(r,...) command(0x20|r,string.char(...)) end
if not initialized then
 ce(0);put(0,12);put(1,0);put(2,1);put(3,3);put(4,0);put(5,channel);put(6,rf_setup)
 put(7,0x70);command(0x30,address);command(0x2A,address);put(0x11,width);put(0x1C,0);put(0x1D,0)
 command(0xE1);command(0xE2);put(0,14);device.sleep_ms(5)
 assert(reg(5)==channel and reg(6)==rf_setup,"NRF24L01 SPI readback failed");initialized=true
end
if #payload>0 then
 ce(0);put(0,14);put(7,0x70);command(0xE1);command(0xA0,payload..string.rep(string.char(0),width-#payload))
 ce(1);device.sleep_ms(1);ce(0)
 for i=1,100 do local s=reg(7);if s&0x30~=0 then put(7,0x30);return {tx_complete=s&0x20~=0,remote_ack=false} end;device.sleep_ms(1) end
 error("NRF24L01 TX timeout")
end
put(0,15);ce(1);device.sleep_ms(1)
local out={received=false,channel=channel}
if reg(0x17)&1==0 then out.received=true;out.payload_hex=device.hex(command(0x61,string.rep(string.char(0),width)));put(7,0x40) end
if not continuous then ce(0) end
return out
""",
    "fixed-width packet TX/RX; no auto-ACK; TX completion does not confirm delivery",
    "https://devzone.nordicsemi.com/cfs-file/__key/communityserver-discussions-components-files/4/content.pdf",
    bus="spi",
)


def radio_prefix(config, background):
    pins = [number(config, key, None, 0, 63) for key in ("sck", "miso", "mosi")]
    controls = []
    logical = {"P3", "P4", "P6", "P8", "P9", "P10", "P12", "P13", "P14", "P15", "P16"}
    for key in ("cs", "ce"):
        pin = config.get(key)
        if isinstance(pin, str):
            if pin not in logical:
                raise ValueError("Expected an external K10 logical GPIO name")
        else:
            pin = number(config, key, None, 0, 63)
        controls.append(pin)
    if len(set(pins + controls)) != 5:
        raise ValueError("Radio SPI/CS/CE pins must be distinct")

    def output(name, pin, idle):
        if isinstance(pin, str):
            return f'local function {name}(v) device.k10_gpio_write("{pin}",v,{idle}) end\n'
        return f'local function {name}(v) device.pin_mode({pin},"output");device.digital_write({pin},v) end\n'

    address = config.get("address_hex", "e7e7e7e7e7")
    payload = config.get("payload_hex", "")
    if not isinstance(address, str) or not re.fullmatch(r"[0-9a-fA-F]{10}", address):
        raise ValueError("address_hex must contain exactly five hex bytes")
    width = number(config, "width", 32, 1, 32)
    if (
        not isinstance(payload, str)
        or not re.fullmatch(r"(?:[0-9a-fA-F]{2})*", payload)
        or len(payload) > width * 2
    ):
        raise ValueError("payload_hex must contain complete hex bytes fitting width")
    rate = number(config, "rate_kbps", 1000, 250, 2000)
    if rate not in (250, 1000, 2000):
        raise ValueError("rate_kbps must be 250, 1000 or 2000")
    power = number(config, "power", 0, 0, 3)
    channel = number(config, "channel", 40, 0, 125)
    setup = {250: 32, 1000: 0, 2000: 8}[rate] | (power << 1)

    def byte_string(value):
        return (
            "string.char(" + ",".join(str(n) for n in bytes.fromhex(value)) + ")" if value else '""'
        )

    sck, miso, mosi = pins
    return (
        output("cs", controls[0], 1)
        + output("ce", controls[1], 0)
        + f'local function spi(s) cs(0);local r=device.spi_transfer({sck},{miso},{mosi},-1,s,4000000,0);cs(1);assert(#r==#s,"short SPI transfer");return r end\n'
        + f"cs(1);ce(0)\nlocal channel,width,rf_setup={channel},{width},{setup}\n"
        + f"local address,payload={byte_string(address)},{byte_string(payload)}\n"
        + f"local continuous={str(background).lower()}\n"
    )


def number(config, name, default, low, high, integer=True):
    value = config.get(name, default)
    if (
        type(value) not in (int, float)
        or isinstance(value, bool)
        or not math.isfinite(value)
        or not low <= value <= high
        or (integer and type(value) is not int)
    ):
        raise ValueError(
            f"{name} must be {'an integer' if integer else 'a number'} in {low}..{high}"
        )
    return value


def render_driver(driver_id, config, background=False):
    if driver_id not in DRIVERS:
        raise ValueError("Unknown device driver")
    if not isinstance(config, dict) or type(background) is not bool:
        raise ValueError("Expected configuration object and boolean background")
    driver = DRIVERS[driver_id]
    allowed = (
        {
            "sck",
            "miso",
            "mosi",
            "cs",
            "ce",
            "channel",
            "width",
            "rate_kbps",
            "power",
            "address_hex",
            "payload_hex",
            "interval_ms",
        }
        if driver.bus == "spi"
        else {
            "sda",
            "scl",
            "pin",
            "address",
            "channel",
            "value",
            "outputs",
            "frequency",
            "shunt",
            "interval_ms",
        }
    )
    if set(config) - allowed:
        raise ValueError("Unknown driver configuration fields")
    prefix = COMMON + "\n"
    if driver.bus == "i2c":
        sda = number(config, "sda", None, 0, 63)
        scl = number(config, "scl", None, 0, 63)
        if sda == scl:
            raise ValueError("SDA/SCL must be distinct GPIO numbers")
        address = number(config, "address", driver.address, 8, 119)
        if address not in driver.addresses:
            raise ValueError("Address does not match this driver's documented range")
        prefix += f"local sda,scl,address={sda},{scl},{address}\n" + I2C + "\n"
    elif driver.bus == "spi":
        prefix += radio_prefix(config, background)
    else:
        prefix += f"local pin={number(config, 'pin', None, 0, 63)}\n"
    channel = number(
        config,
        "channel",
        0,
        0,
        125 if driver.bus == "spi" else (15 if driver_id == "pca9685" else 3),
    )
    value = number(config, "value", 0, 0, 65535 if driver_id == "mcp23017" else 4095)
    outputs = number(config, "outputs", -1, -1, 65535)
    frequency = number(config, "frequency", 50, 40, 1600)
    shunt = number(config, "shunt", 0.1, 0.0001, 100, integer=False)
    if driver.bus != "spi":
        prefix += f"local channel,value,outputs,frequency,shunt={channel},{value},{outputs},{frequency},{shunt}\n"
    source = prefix + "local function sample()\n" + driver.body + "\nend\n"
    if background:
        interval = number(config, "interval_ms", 1000, 20, 5000)
        source += f"while true do device.publish(sample());device.sleep_ms({interval}) end\n"
    else:
        source += "return sample()\n"
    return validate_source(source)


def catalog():
    return [
        dict(
            id=d.name,
            bus=d.bus,
            default_address=d.address,
            addresses=list(d.addresses),
            outputs=d.outputs,
            source=d.source,
            implementation="implemented",
            physical_validation="not tested with attached module",
        )
        for d in DRIVERS.values()
    ]


def register_driver_tools(bridge, root):
    root = Path(root)

    @bridge.mcp.tool()
    def list_compatible_devices() -> str:
        """List implemented device drivers and explicit hardware-test status; not auto-detection."""
        return json.dumps({"count": len(DRIVERS), "drivers": catalog()}, ensure_ascii=False)

    @bridge.mcp.tool()
    def create_device_driver(
        driver_id: str,
        config_json: str,
        name: str,
        install: bool = True,
        run: bool = False,
        background: bool = False,
    ) -> str:
        """Generate/save a concrete Lua driver with short aliases; optionally install/run.

        Supply wiring explicitly (sda/scl or pin). Does not grant GPIO automatically.
        Background runs indefinitely with snapshots, cooperative scheduling and MCP stop.
        External device outputs can persist after the VM exits; program shutdown explicitly.
        """
        valid_name(name)
        if any(type(flag) is not bool for flag in (install, run, background)):
            raise ValueError("install/run/background must be boolean")
        if not isinstance(config_json, str) or len(config_json.encode()) > 1024:
            raise ValueError("Driver configuration limit is 1024 UTF-8 bytes")
        config = json.loads(config_json)
        code = render_driver(driver_id, config, background)
        item = save_script(
            root / "skills", name, code, "sensor " + driver_id, [driver_id], background
        )
        result = {
            "saved_locally": True,
            "driver": driver_id,
            "skill": item,
            "source": code,
            "physical_validation": "not tested with attached module",
        }
        if install:
            raw = bridge.request_to(
                bridge.BOARD_URL,
                "/runtime/skills/save",
                "POST",
                {
                    "name": name,
                    "source": code,
                    "voice_phrase": item["voice_phrase"],
                    "aliases": item["aliases"],
                    "background": background,
                },
            )
            result["installation"] = json.loads(raw)
        if run:
            if install:
                raw = bridge.request_to(
                    bridge.BOARD_URL, "/runtime/skills/run", "POST", {"name": name}
                )
            else:
                raw = bridge.request_to(
                    bridge.BOARD_URL,
                    "/runtime/execute",
                    "POST",
                    {"source": code, "background": background, "timeout_ms": 10000},
                )
            result["execution"] = json.loads(raw)
        return json.dumps(result, ensure_ascii=False)
