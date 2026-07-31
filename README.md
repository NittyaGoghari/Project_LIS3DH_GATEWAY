BLE to AWS IoT Cellular Gateway (Zephyr RTOS)
An end-to-end industrial IoT solution built on Zephyr RTOS. This project consists of a Long-Range Bluetooth (BLE Coded PHY) Sensor Node that batches accelerometer data, and a Central Gateway that receives the data, buffers it into SPI Flash, saves a backup to an SD Card, and publishes it securely to AWS IoT Core via a Quectel EC200U Cellular Modem.

🏗️ System Architecture
The system is split into two primary components:

The Sensor Node (Broadcaster):
Reads X, Y, Z coordinates from an ST LIS2DH accelerometer. It batches 80 samples into a 241-byte payload and broadcasts it using BLE Extended Advertising (Long Range / Coded PHY, S=8) for maximum physical range.

The Gateway Node (Observer & Publisher):
Listens exclusively for the Sensor Node. Upon receiving data, it appends the current cellular network time, saves the raw data to a circular SPI Flash buffer, converts the batched data into JSON, saves a local backup to a FAT-formatted SD card, and publishes the payload to AWS IoT over a secure TLS/MQTT connection.

✨ Key Features
Long-Range BLE: Uses Coded PHY (S=8) Extended Advertising to maximize sensor-to-gateway distance.

Zero Data Loss Flash Buffer: Uses a custom circular buffer on SPI Flash. If the cellular network drops, data continues to spool into Flash and will automatically upload once the connection is restored.

Redundant Local Storage: Every JSON payload successfully packaged is backed up to an SD card (data.json) using FatFS.

Secure AWS IoT Integration: Directly uploads certificates to the EC200U modem and handles MQTT over TLS natively via AT commands.

Auto-Recovery & Stability: Hardware button resets on the sensor node, queue-drop protections on the BLE scanner, and self-healing connection logic for the cellular modem.

Visual Status Indicators: Active-low LED abstractions visually indicate Gateway Boot, Cellular Registration, and Cloud/AWS connection states.

📂 File Structure
Gateway Files
main.c: The core coordinator. Manages the SPI Flash read/write pointers, converts binary sensor data into JSON format, triggers the SD card backup, and initiates the MQTT upload.

observer.c: The Bluetooth scanner thread. Filters incoming BLE traffic via a Hardware Accept List, drops duplicate packets, fetches network time, and writes the final validated packet to SPI Flash.

atcommand.c / atcommand.h: The Quectel EC200U driver. Uses UART to send AT commands to the modem. Handles SIM verification, network registration, TLS certificate uploading, and AWS MQTT publishing.

sdcard.c / sdcard.h: The local storage manager. Initializes the physical SD slot, mounts the FatFS volume, and appends JSON data to /SD:/data.json.

prj.conf: Zephyr configuration file enabling multithreading, BLE Observer, Flash, SPI, SDHC, FatFS, and UART asynchronous APIs.

Sensor Node File
sensor_node.c (Name varies): Runs on the remote device. Reads the st_lis2dh accelerometer every 500ms, batches 80 readings, and broadcasts them via BLE.

🛠️ Hardware Requirements
Microcontroller: Nordic nRF52/nRF53 series (or similar Zephyr-supported boards).

Cellular Modem: Quectel EC200U (connected via UART).

Storage: External SPI Flash chip & SD Card Slot (SPI or SDHC).

Sensor: STMicroelectronics LIS2DH Accelerometer (I2C/SPI).

🚀 Setup & Configuration
1. Configure AWS & Security
You must provide your AWS IoT Endpoint and Certificates.

Update AWS_TOPIC in main.c to your desired MQTT topic.

Provide your AWS Root CA, Device Certificate, and Private Key inside a certs.h file (formatted as byte arrays).

Ensure CONFIG_AWS_IOT_BROKER_HOST_NAME in prj.conf matches your AWS ATS endpoint.

2. Configure Bluetooth MAC Address
Ensure the Gateway is looking for the exact MAC address of your Sensor Node.

Update CONFIG_BLE_ADD in prj.conf to match the static random address of your sensor (e.g., "CE:BD:BE:AF:BA:11").

3. Building and Flashing
This project is built using the standard Zephyr west build system.

To build the Gateway:

Bash
west build -b <your_board_name> -d build_gateway gateway_source_directory/
west flash -d build_gateway
To build the Sensor Node:

Bash
west build -b <your_sensor_board_name> -d build_sensor sensor_source_directory/
west flash -d build_sensor
📊 Status LEDs
The Gateway utilizes Zephyr's GPIO Devicetree aliases to provide real-time visual feedback:

BLE LED: Toggles rapidly every time a valid packet is received over the air.

Gateway LED: Blinks while searching for a SIM card. Turns Solid when successfully registered to the cellular network.

Cloud LED: Blinks during AWS TLS/MQTT negotiation. Turns Solid when fully connected to AWS IoT Core. Turns off if the connection drops.
