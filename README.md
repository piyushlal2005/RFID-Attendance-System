RFID-Based Attendance System

This project implements a standalone RFID-based attendance management system built on a WiFi-enabled microcontroller (ESP32) with an integrated Real Time Clock for accurate and persistent timestamping. The system authenticates users via RFID tags, records timestamped attendance locally, and provides a browser accessible web interface over a self-hosted hotspot for user management and data retrieval. Attendance records are stored in structured CSV format and can be downloaded date-wise without requiring internet connectivity.

The hardware integrates an MFRC522 RFID module using SPI communication, an RTC module for reliable timekeeping, an I2C OLED display for real-time system feedback, and a buzzer for scan confirmation. 

The project demonstrates end-to-end embedded system design involving hardware integration, real-time clock management, storage management, and user-facing interface development within a resource-constrained environment.
