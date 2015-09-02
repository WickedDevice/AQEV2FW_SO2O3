#!/bin/bash

ARGC=$#
version=$1

echo "Number of args: $ARGC"

if [ $ARGC -ne 1 ]
then
  echo "version string required."
  exit
fi

rm -rf WildFire-Arduino-Core
rm -rf WildFire
rm -rf WildFire_CC3000_Library
rm -rf WildFire_SPIFlash
rm -rf TinyWatchdog
rm -rf AQEV2FW
rm -rf LMP91000
rm -rf SHT25
rm -rf MCP342x
rm -rf pubsubclient
rm -rf CapacitiveSensor
rm -rf Time
rm -rf RTClib
rm package_airqualityegg_so2o3-$version\_index.json

git clone https://github.com/WickedDevice/WildFire-Arduino-Core.git
git clone https://github.com/WickedDevice/WildFire.git
git clone https://github.com/WickedDevice/WildFire_CC3000_Library.git
git clone https://github.com/WickedDevice/WildFire_SPIFlash.git
git clone https://github.com/WickedDevice/TinyWatchdog.git
git clone https://github.com/WickedDevice/AQEV2FW_SO2O3.git
git clone https://github.com/WickedDevice/LMP91000
git clone https://github.com/WickedDevice/SHT25
git clone https://github.com/stevemarple/MCP342x
git clone https://github.com/vicatcu/pubsubclient
git clone https://github.com/PaulStoffregen/CapacitiveSensor
git clone https://github.com/PaulStoffregen/Time
git clone https://github.com/mizraith/RTClib

rm -rf AirQualityEgg_SO2O3
rm -rf libraries

mkdir AirQualityEgg_SO2O3
mkdir AirQualityEgg_SO2O3/libraries
mkdir tmp 

cd WildFire-Arduino-Core
git archive master | tar -x -C ../tmp
cd ../tmp/WickedDevice
mv * ../../AirQualityEgg_SO2O3/
cd ../  

cd ../WildFire
mkdir ../AirQualityEgg_SO2O3/libraries/WildFire
git archive master | tar -x -C ../AirQualityEgg_SO2O3/libraries/WildFire

cd ../WildFire_CC3000_Library
mkdir ../AirQualityEgg_SO2O3/libraries/WildFire_CC3000_Library
git archive master | tar -x -C ../AirQualityEgg_SO2O3/libraries/WildFire_CC3000_Library

cd ../WildFire_SPIFlash
mkdir ../AirQualityEgg_SO2O3/libraries/WildFire_SPIFlash
git archive master | tar -x -C ../AirQualityEgg_SO2O3/libraries/WildFire_SPIFlash

cd ../TinyWatchdog
mkdir ../AirQualityEgg_SO2O3/libraries/TinyWatchdog
git archive master | tar -x -C ../AirQualityEgg_SO2O3/libraries/TinyWatchdog

cd ../AQEV2FW
githash=`git rev-parse HEAD`
mkdir ../AirQualityEgg_SO2O3/libraries/AQEV2FW_SO2O3
mkdir ../AirQualityEgg_SO2O3/libraries/AQEV2FW_SO2O3/examples
mkdir ../AirQualityEgg_SO2O3/libraries/AQEV2FW_SO2O3/examples/AQEV2FW
git archive master | tar -x -C ../AirQualityEgg_SO2O3/libraries/AQEV2FW_SO2O3/examples/AQEV2FW_SO2O3

cd ../LMP91000
mkdir ../AirQualityEgg_SO2O3/libraries/LMP91000
git archive master | tar -x -C ../AirQualityEgg_SO2O3/libraries/LMP91000

cd ../SHT25
mkdir ../AirQualityEgg_SO2O3/libraries/SHT25
git archive master | tar -x -C ../AirQualityEgg_SO2O3/libraries/SHT25

cd ../MCP342x
mkdir ../AirQualityEgg_SO2O3/libraries/MCP342x
git archive master | tar -x -C ../AirQualityEgg_SO2O3/libraries/MCP342x

cd ../CapacitiveSensor
mkdir ../AirQualityEgg_SO2O3/libraries/CapacitiveSensor
git archive master | tar -x -C ../AirQualityEgg_SO2O3/libraries/CapacitiveSensor

cd ../Time
mkdir ../AirQualityEgg_SO2O3/libraries/Time
git archive master | tar -x -C ../AirQualityEgg_SO2O3/libraries/Time

cd ../RTClib
mkdir ../AirQualityEgg_SO2O3/libraries/RTClib
git archive master | tar -x -C ../AirQualityEgg_SO2O3/libraries/RTClib


cd ../tmp
rm -rf *
cd ../pubsubclient
mkdir ../AirQualityEgg_SO2O3/libraries/PubSubClient
git archive master | tar -x -C ../tmp
cd PubSubClient
sed -i 's/#define MQTT_MAX_PACKET_SIZE 128/#define MQTT_MAX_PACKET_SIZE 512/g' PubSubClient.h
cp -rf * ../../AirQualityEgg_SO2O3/libraries/PubSubClient
cd ../


cd ../

rm AQEV2-SO2O3-$version-Arduino-$githash.zip
zip -r AQEV2-SO2O3-$version-Arduino-$githash.zip AirQualityEgg

filesize=`stat --printf="%s" AQEV2-SO2O3-$version-Arduino-$githash.zip`
sha256=`sha256sum AQEV2-SO2O3-$version-Arduino-$githash.zip | awk '{print $1;}'`

cp package_template.json package_airqualityegg_so2o3-$version\_index.json
replace_version="s/%version%/$version/g"
replace_githash="s/%githash%/$githash/g"
replace_sha256="s/%sha256%/$sha256/g"
replace_filesize="s/%filesize%/$filesize/g"
sed -i $replace_version package_airqualityegg_so2o3-$version\_index.json
sed -i $replace_githash package_airqualityegg_so2o3-$version\_index.json
sed -i $replace_sha256 package_airqualityegg_so2o3-$version\_index.json
sed -i $replace_filesize package_airqualityegg_so2o3-$version\_index.json

rm -rf tmp
rm -rf AirQualityEgg_SO2O3
rm -rf WildFire-Arduino-Core
rm -rf WildFire
rm -rf WildFire_CC3000_Library
rm -rf WildFire_SPIFlash
rm -rf TinyWatchdog
rm -rf AQEV2FW
rm -rf LMP91000
rm -rf SHT25
rm -rf MCP342x
rm -rf pubsubclient
rm -rf CapacitiveSensor
rm -rf Time
rm -rf RTClib
