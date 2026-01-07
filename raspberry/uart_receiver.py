#!/usr/bin/env python3
import serial
import mysql.connector
from mysql.connector import Error
import time

# ===== UART 설정 =====
UART_PORT = "/dev/serial0"   # 필요하면 변경
BAUDRATE = 115200            # STM32와 동일하게 수정

# ===== MariaDB 설정 =====
DB_CONFIG = {
    "host": "localhost",
    "user": "sensoruser",      # 만든 계정
    "password": "sensorpass",  # 비밀번호
    "database": "sensordb",
    "charset": "utf8mb4"
}


def get_db_connection():
    """MariaDB 연결 (끊어지면 재시도)"""
    while True:
        try:
            conn = mysql.connector.connect(**DB_CONFIG)
            if conn.is_connected():
                print("[DB] 연결 성공")
                return conn
        except Error as e:
            print("[DB] 연결 오류:", e)
            print("5초 후 재시도...")
            time.sleep(5)


def ensure_table(cursor):
    """테이블 없으면 생성"""
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS water_log (
            id       INT AUTO_INCREMENT PRIMARY KEY,
            ts       TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            rain_mm  INT,
            level    VARCHAR(10),
            servo    TINYINT(1)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
    """)


def parse_line(line: str):
    """
    예: "RAIN=23,LEVEL=정상,SERVO=ON"
      -> (23, "정상", 1)
    """
    parts = line.split(',')
    if len(parts) != 3:
        raise ValueError("필드 개수 오류")

    rain_part = parts[0].strip()   # "RAIN=23"
    level_part = parts[1].strip()  # "LEVEL=정상"
    servo_part = parts[2].strip()  # "SERVO=ON"

    if not rain_part.startswith("RAIN="):
        raise ValueError("RAIN 필드 형식 오류")
    if not level_part.startswith("LEVEL="):
        raise ValueError("LEVEL 필드 형식 오류")
    if not servo_part.startswith("SERVO="):
        raise ValueError("SERVO 필드 형식 오류")

    rain_str  = rain_part.split('=', 1)[1].strip()
    level_str = level_part.split('=', 1)[1].strip()
    servo_str = servo_part.split('=', 1)[1].strip()

    rain_mm = int(rain_str)
    servo_val = 1 if servo_str.upper() == "ON" else 0

    return rain_mm, level_str, servo_val


def main():
    # 1) UART 열기
    ser = serial.Serial(
        port=UART_PORT,
        baudrate=BAUDRATE,
        timeout=1
    )
    print(f"[UART] {UART_PORT} @ {BAUDRATE}bps 열림")

    # 2) DB 연결 + 테이블 준비
    conn = get_db_connection()
    cursor = conn.cursor()
    ensure_table(cursor)
    conn.commit()

    print("[SYSTEM] 수신 시작... (Ctrl+C 로 종료)")

    try:
        while True:
            line_bytes = ser.readline()
            if not line_bytes:
                continue

            line = line_bytes.decode('utf-8', errors='ignore').strip()
            if not line:
                continue

            print("[RECV]", line)

            try:
                rain_mm, level, servo = parse_line(line)
            except Exception as e:
                print("[PARSE ERROR]", e)
                continue

            try:
                sql = """
                    INSERT INTO water_log (rain_mm, level, servo)
                    VALUES (%s, %s, %s)
                """
                cursor.execute(sql, (rain_mm, level, servo))
                conn.commit()
                print(f"[DB] 저장 -> rain={rain_mm}, level={level}, servo={servo}")
            except Error as e:
                print("[DB ERROR]", e)
                if not conn.is_connected():
                    conn = get_db_connection()
                    cursor = conn.cursor()
                    ensure_table(cursor)
                    conn.commit()

    except KeyboardInterrupt:
        print("\n[SYSTEM] 종료 요청")
    finally:
        cursor.close()
        conn.close()
        ser.close()
        print("[SYSTEM] 종료 완료")


if __name__ == "__main__":
    main()

