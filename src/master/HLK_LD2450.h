/*
 * HLK-LD2450 Radar Sensor Library
 * 
 * Handles serial communication with HLK-LD2450 24GHz radar sensor
 * Decodes target position data (x, y, speed, resolution)
 * Supports up to 3 simultaneous targets
 *
 * Based on eric-rolph/esp32-radar-tracker (arduino/HLK_LD2450.h),
 * MIT License, Copyright (c) 2025 ESP32 Radar Tracker Contributors.
 * The original copyright and license notice are retained in NOTICE.
 *
 * Copyright (C) 2026 Kanchuan Industrial
 * SPDX-License-Identifier: MIT
 */

#ifndef HLK_LD2450_H
#define HLK_LD2450_H

#include <Arduino.h>
#include <HardwareSerial.h>

// Protocol constants
#define REPORT_HEADER_SIZE 4
#define REPORT_TAIL_SIZE 2
#define TARGET_DATA_SIZE 8
#define NUM_TARGETS 3
#define FRAME_SIZE 30

// Frame markers
const uint8_t REPORT_HEADER[4] = {0xAA, 0xFF, 0x03, 0x00};
const uint8_t REPORT_TAIL[2] = {0x55, 0xCC};

struct Target {
  bool valid;
  int16_t x_mm;        // X coordinate in mm
  int16_t y_mm;        // Y coordinate in mm
  float distance_m;    // Calculated distance in meters
  float angle_deg;     // Calculated angle in degrees
  int16_t speed_cm_s;  // Speed in cm/s
  uint16_t resolution; // Distance resolution
};

class HLK_LD2450 {
public:
  HLK_LD2450(HardwareSerial &serial) : _serial(serial) {
    clearTargets();
  }
  
  void update() {
    // Read available data from serial
    while (_serial.available()) {
      uint8_t byte = _serial.read();
      rawBytes++;
      
      // Look for frame header
      if (!_receivingFrame) {
        _buffer[_bufferIndex++] = byte;
        
        // Keep only last 4 bytes to check for header
        if (_bufferIndex >= 4) {
          if (checkHeader()) {
            _receivingFrame = true;
            _bufferIndex = 4; // Keep header
          } else {
            // Shift buffer left
            for (int i = 0; i < 3; i++) {
              _buffer[i] = _buffer[i + 1];
            }
            _bufferIndex = 3;
          }
        }
      } else {
        // Receiving frame data
        _buffer[_bufferIndex++] = byte;
        
        // Check if we have a complete frame
        if (_bufferIndex >= FRAME_SIZE) {
          if (checkTail()) {
            totalFrames++;
            parseFrame();
          } else {
            badFrames++;
          }
          resetBuffer();
        }
        
        // Prevent buffer overflow
        if (_bufferIndex >= 50) {
          resetBuffer();
        }
      }
    }
  }
  
  Target targets[NUM_TARGETS];
  // ── UART 链路诊断计数器 ──
  //   ★ 这三个数由主循环的 [RADAR] 行每分钟打印一次（见 master.ino 的健康日志）。
  //     判读（这是「雷达不出数」的第一个分叉点）：
  //       raw 不涨          → 物理层没通（TX/RX 接反 / 没供电 / 还在启动）
  //       raw 涨 ok 不涨    → 波特率不对或帧被截断
  //       bad 明显增长      → 线路噪声或波特率擦边（帧头对上、帧尾不对）
  //       ok 涨但屏上无点    → 已进入算法层，与 UART 无关
  //   ⚠️ 曾经这三个计数器是「只写不读」的死代码，而文档里却让人「看 RAW:」，
  //      结果现场按文档去串口找那条日志，什么也找不到。接上日志才是正解。
  uint32_t totalFrames = 0;  // 校验通过并解析成功的完整帧数
  uint32_t badFrames = 0;    // 帧头对上、帧尾不对的帧数
  uint32_t rawBytes = 0;     // UART 累计收到的字节数
  
private:
  HardwareSerial &_serial;
  uint8_t _buffer[50];
  int _bufferIndex = 0;
  bool _receivingFrame = false;
  
  void clearTargets() {
    for (int i = 0; i < NUM_TARGETS; i++) {
      targets[i].valid = false;
      targets[i].x_mm = 0;
      targets[i].y_mm = 0;
      targets[i].distance_m = 0.0;
      targets[i].angle_deg = 0.0;
      targets[i].speed_cm_s = 0;
      targets[i].resolution = 0;
    }
  }
  
  bool checkHeader() {
    for (int i = 0; i < REPORT_HEADER_SIZE; i++) {
      if (_buffer[i] != REPORT_HEADER[i]) {
        return false;
      }
    }
    return true;
  }
  
  bool checkTail() {
    int tailStart = FRAME_SIZE - REPORT_TAIL_SIZE;
    for (int i = 0; i < REPORT_TAIL_SIZE; i++) {
      if (_buffer[tailStart + i] != REPORT_TAIL[i]) {
        return false;
      }
    }
    return true;
  }
  
  void parseFrame() {
    // Parse 3 targets (each target is 8 bytes)
    int offset = REPORT_HEADER_SIZE;
    
    for (int i = 0; i < NUM_TARGETS; i++) {
      // LD2450 protocol: little-endian, non-standard signed encoding
      // Bit15=1 means POSITIVE, Bit15=0 means NEGATIVE (inverted!)
      uint16_t rawX = (uint16_t)(_buffer[offset + 1] << 8 | _buffer[offset]);
      uint16_t rawY = (uint16_t)(_buffer[offset + 3] << 8 | _buffer[offset + 2]);
      int16_t rawSpeed = (int16_t)(_buffer[offset + 5] << 8 | _buffer[offset + 4]);
      uint16_t resolution = (uint16_t)(_buffer[offset + 7] << 8 | _buffer[offset + 6]);
      
      // Convert LD2450 signed format to standard signed
      int16_t x, y;
      if (rawX & 0x8000) x = (int16_t)(rawX - 0x8000);  // Positive
      else x = (rawX == 0) ? 0 : -(int16_t)rawX;          // Negative
      
      if (rawY & 0x8000) y = (int16_t)(rawY - 0x8000);  // Positive
      else y = (rawY == 0) ? 0 : -(int16_t)rawY;          // Negative
      
      // Check if target is valid (not zero)
      if (x == 0 && y == 0) {
        targets[i].valid = false;
      } else {
        targets[i].valid = true;
        targets[i].x_mm = x;
        targets[i].y_mm = y;
        targets[i].speed_cm_s = rawSpeed;
        targets[i].resolution = resolution;
        
        // Calculate distance and angle
        targets[i].distance_m = sqrt((float)x * x + (float)y * y) / 1000.0;
        targets[i].angle_deg = atan2((float)x, (float)y) * 180.0 / PI;
      }
      
      offset += TARGET_DATA_SIZE;
    }
  }
  
  void resetBuffer() {
    _bufferIndex = 0;
    _receivingFrame = false;
  }
};

#endif // HLK_LD2450_H
