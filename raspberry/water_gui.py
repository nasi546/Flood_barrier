#!/usr/bin/env python3
import tkinter as tk
from tkinter import ttk, messagebox
import mysql.connector
from mysql.connector import Error

DB_CONFIG = {
    "host": "localhost",
    "user": "sensoruser",
    "password": "sensorpass",
    "database": "sensordb",
    "charset": "utf8mb4"
}

REFRESH_INTERVAL_MS = 5000  # 5초마다 새로고침


def get_connection():
    """MariaDB 연결"""
    try:
        conn = mysql.connector.connect(**DB_CONFIG)
        if conn.is_connected():
            return conn
    except Error as e:
        print("[DB 연결 오류]", e)
    return None


class WaterMonitorApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("홍수 모니터링 - DB 뷰어")
        self.geometry("800x500")

        self.conn = None
        self.cursor = None

        self.create_widgets()
        self.connect_db()
        self.refresh_data()

    def create_widgets(self):
        # 상단: 최신 상태
        top_frame = ttk.Frame(self)
        top_frame.pack(side=tk.TOP, fill=tk.X, padx=10, pady=10)

        self.lbl_latest_rain = ttk.Label(top_frame, text="물수위: - mm", font=("맑은 고딕", 12, "bold"))
        self.lbl_latest_rain.pack(side=tk.LEFT, padx=10)

        self.lbl_latest_level = ttk.Label(top_frame, text="위험도: -", font=("맑은 고딕", 12, "bold"))
        self.lbl_latest_level.pack(side=tk.LEFT, padx=10)

        self.lbl_latest_servo = ttk.Label(top_frame, text="서보: -", font=("맑은 고딕", 12, "bold"))
        self.lbl_latest_servo.pack(side=tk.LEFT, padx=10)

        refresh_btn = ttk.Button(top_frame, text="새로고침", command=self.refresh_data)
        refresh_btn.pack(side=tk.RIGHT, padx=10)

        # 중앙: 로그 테이블
        mid_frame = ttk.Frame(self)
        mid_frame.pack(side=tk.TOP, fill=tk.BOTH, expand=True, padx=10, pady=10)

        columns = ("id", "ts", "rain_mm", "level", "servo")
        self.tree = ttk.Treeview(mid_frame, columns=columns, show="headings", height=15)

        self.tree.heading("id", text="ID")
        self.tree.heading("ts", text="시간")
        self.tree.heading("rain_mm", text="물수위(mm)")
        self.tree.heading("level", text="위험도")
        self.tree.heading("servo", text="서보상태")

        self.tree.column("id", width=50, anchor=tk.CENTER)
        self.tree.column("ts", width=180, anchor=tk.CENTER)
        self.tree.column("rain_mm", width=100, anchor=tk.CENTER)
        self.tree.column("level", width=80, anchor=tk.CENTER)
        self.tree.column("servo", width=80, anchor=tk.CENTER)

        vsb = ttk.Scrollbar(mid_frame, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=vsb.set)

        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        vsb.pack(side=tk.RIGHT, fill=tk.Y)

        # 하단: 상태바
        bottom_frame = ttk.Frame(self)
        bottom_frame.pack(side=tk.BOTTOM, fill=tk.X, padx=10, pady=5)

        self.status_label = ttk.Label(bottom_frame, text="DB 연결 상태: -")
        self.status_label.pack(side=tk.LEFT)

    def connect_db(self):
        if self.conn and self.conn.is_connected():
            return

        self.conn = get_connection()
        if self.conn:
            self.cursor = self.conn.cursor()
            self.status_label.config(text="DB 연결 상태: 연결됨")
        else:
            self.cursor = None
            self.status_label.config(text="DB 연결 상태: 연결 실패")

    def refresh_data(self):
        try:
            if not (self.conn and self.conn.is_connected()):
                self.connect_db()

            if not self.cursor:
                raise Error("커서 없음 (DB 연결 실패)")

            self.cursor.execute("""
                SELECT id, ts, rain_mm, level, servo
                FROM water_log
                ORDER BY id DESC
                LIMIT 50
            """)
            rows = self.cursor.fetchall()

            for item in self.tree.get_children():
                self.tree.delete(item)

            for row in rows:
                id_, ts, rain_mm, level, servo = row
                servo_str = "ON" if servo == 1 else "OFF"
                self.tree.insert("", tk.END, values=(id_, ts, rain_mm, level, servo_str))

            if rows:
                latest = rows[0]
                _, ts, rain_mm, level, servo = latest
                servo_str = "ON" if servo == 1 else "OFF"

                self.lbl_latest_rain.config(text=f"물수위: {rain_mm} mm")
                self.lbl_latest_level.config(text=f"위험도: {level}")
                self.lbl_latest_servo.config(text=f"서보: {servo_str}")
            else:
                self.lbl_latest_rain.config(text="물수위: - mm")
                self.lbl_latest_level.config(text="위험도: -")
                self.lbl_latest_servo.config(text="서보: -")

            self.status_label.config(text="DB 연결 상태: 연결됨 / 최근 데이터 갱신 완료")

        except Error as e:
            print("[DB 오류]", e)
            self.status_label.config(text="DB 연결 상태: 오류 발생")
            messagebox.showerror("DB 오류", f"DB 접근 중 오류 발생:\n{e}")

        # 자동 새로고침 예약
        self.after(REFRESH_INTERVAL_MS, self.refresh_data)

    def on_closing(self):
        if self.cursor:
            self.cursor.close()
        if self.conn and self.conn.is_connected():
            self.conn.close()
        self.destroy()


if __name__ == "__main__":
    app = WaterMonitorApp()
    app.protocol("WM_DELETE_WINDOW", app.on_closing)
    app.mainloop()
