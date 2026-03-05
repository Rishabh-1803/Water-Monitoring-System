# System Architecture

The overall system architecture of this IoT water monitoring system based on the ESP32 shows the interaction of hardware components, sensors, and control elements that work together to provide automatic monitoring and regulation of water parameters.

## Architecture Overview

The system consists of three main layers:

1. Sensing Layer  
2. Processing and Control Layer  
3. Actuation Layer  

## 1. Sensing Layer
The sensing layer includes:
- Soil Moisture Sensor
- DHT11 Sensor (Temperature and Humidity)

With help of these sensors we can monitor, sense and operate on real time values of Temprature, Soil Moisture and Humidity.

## 2. Processing and Control Layer
The ESP32 C3 Super Mini microcontroller is the central processing unit. It does:
- reading of sensor data,
- the processing of values of coded logic,
- Decision making for turning water on and off,
- and after that sending signal to relay to set solenoid valve.

## 3. Actuation Layer
The actuation layer includes:
- Relay Module (5V)
- Solenoid Valve (12V)

From the processing unit it takes the command and makes the solenoid turn ON and OFF making the water flow.

## Data and Control Flow
1. Sensors first collects data from the environment for
2. ESP32 to process the readings of sensors to 
3. Make the Control Decision then the
4. Relay module is triggered and the  
5. Solenoid valve controls flow of water  

## Key Features of the System
- Modular design  
- Real-time monitoring  
- Automated control  
- Low-cost and scalable  
- Suitable for educational and academic use  
