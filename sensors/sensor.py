import socket
import time
import random
from config import SERVER_HOST, SERVER_PORT, SEND_INTERVAL


class Sensor:
    def __init__(self, sensor_id, sensor_type):
        self.sensor_id = sensor_id
        self.sensor_type = sensor_type
        self.sock = None

    def connect(self):
        while True:
            try:
                # Resolución por nombre, nunca por IP
                self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.sock.connect((SERVER_HOST, SERVER_PORT))
                print(f"[{self.sensor_id}] Conectado al servidor")
                self._register()
                return
            except socket.error as e:
                print(f"[{self.sensor_id}] Error de conexión: {e}. Reintentando en 5s...")
                time.sleep(5)

    def _register(self):
        msg = f"REGISTER SENSOR {self.sensor_id} {self.sensor_type}\n"
        self.sock.sendall(msg.encode())
        response = self.sock.recv(1024).decode().strip()
        print(f"[{self.sensor_id}] Registro: {response}")

    def generate_value(self):
        # Cada subclase sobreescribe este método
        raise NotImplementedError

    def send_measurement(self):
        value = self.generate_value()
        timestamp = int(time.time())
        msg = f"DATA {self.sensor_id} {value:.2f} {timestamp}\n"
        try:
            self.sock.sendall(msg.encode())
            response = self.sock.recv(1024).decode().strip()
            print(f"[{self.sensor_id}] Enviado {value:.2f} → {response}")
        except socket.error as e:
            print(f"[{self.sensor_id}] Error al enviar: {e}")
            self.connect()  # Reconectar automáticamente

    def run(self):
        self.connect()
        while True:
            self.send_measurement()
            time.sleep(SEND_INTERVAL)
